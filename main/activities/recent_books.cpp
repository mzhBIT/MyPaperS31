#include "recent_books.h"
#include "RecentBookCapacity.h"
#include "parser/epub_parser.h"
#include "storage/app_paths.h"
#include "time/time_basis.h"
#include <ctime>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <vector>
#include <esp_timer.h>
#include <esp_log.h>

static constexpr uint32_t TXT_PROGRESS_MAGIC = 0x54585032; // TXP2
static constexpr uint32_t TXT_PROGRESS_VERSION = 1;
static constexpr uint32_t EPUB_PROGRESS_MAGIC = 0x45504231; // EPB1
static constexpr uint32_t EPUB_PROGRESS_VERSION = 2;
static constexpr uint32_t MOBI_PROGRESS_MAGIC = 0x4d4f4231; // MOB1
static constexpr uint32_t MOBI_PROGRESS_VERSION = 1;
static constexpr uint32_t FB2_PROGRESS_MAGIC = 0x46423231; // FB21
static constexpr uint32_t FB2_PROGRESS_VERSION = 1;

const char *recent_book_base_name(const char *path)
{
    if (!path || !path[0]) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static std::string recent_book_file_stem(const char *path)
{
    std::string name = recent_book_base_name(path);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.erase(dot);
    return name;
}

static std::string recent_book_progress_key(const char *path)
{
    size_t hash = 0;
    for (const char *p = path ? path : ""; *p; ++p) {
        hash = hash * 31 + (unsigned char)*p;
    }
    char key[32];
    std::snprintf(key, sizeof(key), "%08lx", (unsigned long)hash);
    return key;
}

static std::string reading_time_path(const char *path)
{
    return std::string(app_paths::data_dir()) + "/reading_time/" +
           recent_book_progress_key(path) + ".time";
}

static uint64_t load_reading_time(const char *path)
{
    const std::string file = reading_time_path(path);
    // A backup remains recoverable if power was lost during replacement.
    for (const std::string &candidate : {file, file + ".bak"}) {
        FILE *fp = std::fopen(candidate.c_str(), "rb");
        if (!fp) continue;
        uint32_t magic = 0;
        uint64_t elapsed = 0;
        const bool ok = std::fread(&magic, sizeof(magic), 1, fp) == 1 &&
                        std::fread(&elapsed, sizeof(elapsed), 1, fp) == 1 &&
                        magic == 0x42525431; // BRT1, elapsed microseconds
        std::fclose(fp);
        if (ok) return elapsed;
    }
    return 0;
}

static std::string daily_path() { return std::string(app_paths::data_dir())+"/reading_time/days.bin"; }
static uint32_t days_checksum(const reading_days::History &history) {
    uint32_t hash=2166136261u;
    auto add=[&](uint64_t value) {
        for(int i=0;i<8;++i) { hash^=uint8_t(value>>(i*8)); hash*=16777619u; }
    };
    add(uint64_t(history.last_day)); for(auto value:history.us) add(value);
    return hash;
}
static reading_days::History load_days() {
    for(const std::string &file:{daily_path(),daily_path()+".bak"}) {
        FILE *fp=std::fopen(file.c_str(),"rb");
        if(!fp) continue;
        uint32_t magic=0,checksum=0;
        reading_days::History history;
        bool ok=std::fread(&magic,4,1,fp)==1 && magic==0x31444452 &&
            std::fread(&history.last_day,8,1,fp)==1 &&
            std::fread(history.us.data(),8,reading_days::count,fp)==reading_days::count &&
            std::fread(&checksum,4,1,fp)==1 && std::fgetc(fp)==EOF &&
            history.last_day>=0 && history.last_day<=47481 && checksum==days_checksum(history);
        std::fclose(fp);
        if(ok) return history;
    }
    return {};
}
static bool save_days(const reading_days::History &pending) {
    if(pending.last_day<0) return true;
    auto history=load_days(); history.merge(pending);
    app_paths::ensure_dir((std::string(app_paths::data_dir())+"/reading_time").c_str());
    const std::string file=daily_path(),temp=file+".tmp",backup=file+".bak";
    FILE *fp=std::fopen(temp.c_str(),"wb");
    if(!fp) return false;
    const uint32_t magic=0x31444452,checksum=days_checksum(history);
    bool ok=std::fwrite(&magic,4,1,fp)==1 && std::fwrite(&history.last_day,8,1,fp)==1 &&
        std::fwrite(history.us.data(),8,reading_days::count,fp)==reading_days::count &&
        std::fwrite(&checksum,4,1,fp)==1;
    if(std::fclose(fp)!=0) ok=false;
    struct stat st{};
    if(ok && stat(file.c_str(),&st)==0) {
        std::remove(backup.c_str()); ok=std::rename(file.c_str(),backup.c_str())==0;
    }
    if(ok) ok=std::rename(temp.c_str(),file.c_str())==0;
    if(ok) std::remove(backup.c_str()); else std::remove(temp.c_str());
    return ok;
}
bool recent_reading_days(std::array<uint64_t,7> *seconds,int64_t *today) {
    if(!seconds || !today) return false;
    seconds->fill(0); *today=0;
    if(!time_basis_system_clock_valid()) return false;
    *today=(int64_t(std::time(nullptr))+time_basis_get_offset_minutes()*60)/86400;
    *seconds=load_days().week(*today);
    return true;
}

void BookReadingTimer::configure(const char *path)
{
    path_ = path ? path : "";
    elapsed_us_ = 0;
    started_us_ = -1;
    started_wall_us_ = -1;
    pending_days_ = {};
}

void BookReadingTimer::resume()
{
    if (!path_.empty() && started_us_ < 0) {
        started_us_ = esp_timer_get_time();
        started_wall_us_=time_basis_system_clock_valid()?int64_t(std::time(nullptr))*1000000:-1;
        started_offset_minutes_=time_basis_get_offset_minutes();
    }
}

void BookReadingTimer::pause()
{
    if (started_us_ < 0) return;
    const uint64_t interval=static_cast<uint64_t>(esp_timer_get_time() - started_us_);
    elapsed_us_ += interval;
    if(started_wall_us_>=0) pending_days_.add_span(started_wall_us_,interval,started_offset_minutes_);
    started_us_ = -1;
    started_wall_us_ = -1;
}

void BookReadingTimer::finish()
{
    pause();
    if (path_.empty() || !elapsed_us_) return;
    const uint64_t total = load_reading_time(path_.c_str()) + elapsed_us_;
    const std::string dir = std::string(app_paths::data_dir()) + "/reading_time";
    app_paths::ensure_dir(dir.c_str());
    const std::string file = reading_time_path(path_.c_str());
    const std::string temp = file + ".tmp", backup = file + ".bak";
    FILE *fp = std::fopen(temp.c_str(), "wb");
    bool ok = false;
    if (fp) {
        const uint32_t magic = 0x42525431;
        ok = std::fwrite(&magic, sizeof(magic), 1, fp) == 1 &&
             std::fwrite(&total, sizeof(total), 1, fp) == 1;
        if (std::fclose(fp) != 0) ok = false;
    }
    if (ok) {
        struct stat st = {};
        if (stat(file.c_str(), &st) == 0) {
            std::remove(backup.c_str());
            ok = std::rename(file.c_str(), backup.c_str()) == 0;
        }
        if (ok) ok = std::rename(temp.c_str(), file.c_str()) == 0;
        if (ok) std::remove(backup.c_str());
    }
    if (ok) {
        if(save_days(pending_days_)) pending_days_={};
        else ESP_LOGW("READING_TIME","Daily history save failed");
        ESP_LOGI("READING_TIME", "saved total=%llu s session=%llu s",
                 (unsigned long long)(total / 1000000),
                 (unsigned long long)(elapsed_us_ / 1000000));
        elapsed_us_ = 0;
    } else {
        ESP_LOGW("READING_TIME", "save failed: %s", file.c_str());
        std::remove(temp.c_str());
    }
}

static int txt_progress_percent(const char *path)
{
    struct stat source = {};
    if (!path || stat(path, &source) != 0 || source.st_size <= 0) return 0;

    std::string progress_path = std::string(app_paths::txt_progress_dir()) + "/" +
                                recent_book_progress_key(path) + ".pos";
    FILE *fp = std::fopen(progress_path.c_str(), "rb");
    if (!fp) return 0;

    uint32_t first = 0;
    uint32_t offset = 0;
    bool ok = std::fread(&first, 1, sizeof(first), fp) == sizeof(first);
    if (ok && first == TXT_PROGRESS_MAGIC) {
        uint32_t version = 0, layout = 0;
        ok = std::fread(&version, 1, sizeof(version), fp) == sizeof(version) &&
             std::fread(&layout, 1, sizeof(layout), fp) == sizeof(layout) &&
             std::fread(&offset, 1, sizeof(offset), fp) == sizeof(offset) &&
             version == TXT_PROGRESS_VERSION;
    } else if (ok) {
        // Legacy TXT progress files stored the byte offset directly.
        offset = first;
    }
    std::fclose(fp);
    if (!ok) return 0;

    uint64_t file_size = (uint64_t)source.st_size;
    uint64_t bounded = std::min<uint64_t>(offset, file_size);
    return (int)std::min<uint64_t>(100, (bounded * 100 + file_size / 2) / file_size);
}

static bool read_progress_summary(FILE *fp, RecentBookProgress *result)
{
    if (!fp || !result) return false;
    uint32_t marker = 0, percent = 0, chapter = 0, chapter_count = 0;
    if (std::fread(&marker, 1, sizeof(marker), fp) != sizeof(marker) ||
        std::fread(&percent, 1, sizeof(percent), fp) != sizeof(percent) ||
        std::fread(&chapter, 1, sizeof(chapter), fp) != sizeof(chapter) ||
        std::fread(&chapter_count, 1, sizeof(chapter_count), fp) != sizeof(chapter_count) ||
        marker != READER_PROGRESS_SUMMARY_MAGIC) {
        return false;
    }
    result->percent = std::clamp(static_cast<int>(percent), 0, 100);
    result->chapter_count = static_cast<int>(chapter_count);
    result->chapter = result->chapter_count > 0
        ? std::clamp(static_cast<int>(chapter), 1, result->chapter_count)
        : 0;
    return true;
}

static RecentBookProgress epub_progress(const char *path)
{
    RecentBookProgress result;
    std::string progress_path = std::string(app_paths::epub_progress_dir()) + "/" +
                                recent_book_progress_key(path) + ".pos";
    FILE *fp = std::fopen(progress_path.c_str(), "rb");
    if (!fp) return result;

    uint32_t magic = 0, version = 0, layout = 0, spine = 0;
    uint64_t position = 0;
    bool ok = std::fread(&magic, 1, sizeof(magic), fp) == sizeof(magic) &&
              std::fread(&version, 1, sizeof(version), fp) == sizeof(version) &&
              std::fread(&layout, 1, sizeof(layout), fp) == sizeof(layout) &&
              std::fread(&spine, 1, sizeof(spine), fp) == sizeof(spine) &&
              std::fread(&position, 1, sizeof(position), fp) == sizeof(position);
    if (!ok || magic != EPUB_PROGRESS_MAGIC || version != EPUB_PROGRESS_VERSION) {
        std::fclose(fp);
        return result;
    }
    const bool has_summary = read_progress_summary(fp, &result);
    std::fclose(fp);
    if (has_summary) return result;

    EpubBook::cached_progress_info(path, spine, position,
                                   &result.percent, &result.chapter, &result.chapter_count);
    // Old files have no summary. Match the chapter-index scale used by the
    // Chapters progress bar instead of the former byte-weighted estimate.
    if (result.chapter_count > 1 && result.chapter > 0) {
        result.percent = (result.chapter - 1) * 100 / (result.chapter_count - 1);
    }
    return result;
}

static RecentBookProgress sectioned_progress(const char *path, const char *root,
                                             uint32_t expected_magic, uint32_t expected_version)
{
    RecentBookProgress result;
    std::string progress_path = std::string(root) + "/" +
                                recent_book_progress_key(path) + ".pos";
    FILE *fp = std::fopen(progress_path.c_str(), "rb");
    if (!fp) return result;

    uint32_t magic = 0, version = 0, layout = 0, section = 0;
    uint64_t position = 0;
    uint32_t section_count = 0, section_size = 0;
    bool ok = std::fread(&magic, 1, sizeof(magic), fp) == sizeof(magic) &&
              std::fread(&version, 1, sizeof(version), fp) == sizeof(version) &&
              std::fread(&layout, 1, sizeof(layout), fp) == sizeof(layout) &&
              std::fread(&section, 1, sizeof(section), fp) == sizeof(section) &&
              std::fread(&position, 1, sizeof(position), fp) == sizeof(position) &&
              std::fread(&section_count, 1, sizeof(section_count), fp) == sizeof(section_count) &&
              std::fread(&section_size, 1, sizeof(section_size), fp) == sizeof(section_size);
    if (!ok || magic != expected_magic || version != expected_version || section_count == 0) {
        std::fclose(fp);
        return result;
    }

    if (read_progress_summary(fp, &result)) {
        std::fclose(fp);
        return result;
    }
    std::fclose(fp);

    section = std::min(section, section_count - 1);
    uint64_t local_thousand = section_size
                                  ? std::min<uint64_t>(1000, position * 1000 / section_size)
                                  : 0;
    uint64_t overall_thousand = static_cast<uint64_t>(section) * 1000 + local_thousand;
    result.percent = static_cast<int>(std::min<uint64_t>(100, (overall_thousand * 100) /
                                                               (static_cast<uint64_t>(section_count) * 1000)));
    result.chapter = static_cast<int>(section + 1);
    result.chapter_count = static_cast<int>(section_count);
    return result;
}

static RecentBookProgress generic_reflow_progress(const char *path)
{
    RecentBookProgress result;
    std::string progress_path = std::string(app_paths::epub_progress_dir()) + "/" +
                                recent_book_progress_key(path) + ".pos";
    FILE *fp = std::fopen(progress_path.c_str(), "rb");
    if (!fp) return result;
    uint32_t magic = 0, version = 0, layout = 0, spine = 0;
    uint64_t position = 0;
    const bool ok = std::fread(&magic, 1, sizeof(magic), fp) == sizeof(magic) &&
                    std::fread(&version, 1, sizeof(version), fp) == sizeof(version) &&
                    std::fread(&layout, 1, sizeof(layout), fp) == sizeof(layout) &&
                    std::fread(&spine, 1, sizeof(spine), fp) == sizeof(spine) &&
                    std::fread(&position, 1, sizeof(position), fp) == sizeof(position) &&
                    magic == EPUB_PROGRESS_MAGIC && version == EPUB_PROGRESS_VERSION;
    if (ok) read_progress_summary(fp, &result);
    std::fclose(fp);
    return result;
}

static RecentBookProgress mobi_progress(const char *path)
{
    return sectioned_progress(path, app_paths::mobi_progress_dir(),
                              MOBI_PROGRESS_MAGIC, MOBI_PROGRESS_VERSION);
}

static RecentBookProgress fb2_progress(const char *path)
{
    return sectioned_progress(path, app_paths::fb2_progress_dir(),
                              FB2_PROGRESS_MAGIC, FB2_PROGRESS_VERSION);
}

static RecentBookProgress load_book_progress(const RecentBook &book)
{
    if (strcasecmp(book.type.c_str(), "epub") == 0) {
        return epub_progress(book.path.c_str());
    }
    if (strcasecmp(book.type.c_str(), "mobi") == 0) return mobi_progress(book.path.c_str());
    if (strcasecmp(book.type.c_str(), "fb2") == 0) return fb2_progress(book.path.c_str());
    if (strcasecmp(book.type.c_str(), "markdown") == 0 || strcasecmp(book.type.c_str(), "docx") == 0 ||
        strcasecmp(book.type.c_str(), "comic") == 0) {
        return generic_reflow_progress(book.path.c_str());
    }
    RecentBookProgress result;
    result.percent = txt_progress_percent(book.path.c_str());
    return result;
}

RecentBookProgress recent_book_progress(const RecentBook &book)
{
    RecentBookProgress result = load_book_progress(book);
    result.reading_seconds = load_reading_time(book.path.c_str()) / 1000000;
    return result;
}

static void trim_line(std::string *s)
{
    if (!s) return;
    while (!s->empty() && (s->back() == '\n' || s->back() == '\r' || s->back() == ' ' || s->back() == '\t')) {
        s->pop_back();
    }
}

static bool file_exists(const std::string &path)
{
    struct stat st = {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool is_weread_record(const RecentBook &book)
{
    if (strcasecmp(book.type.c_str(), "weread") == 0) return true;
    const std::string root = app_paths::weread_books_dir();
    return book.path == root ||
           (book.path.size() > root.size() &&
            book.path.compare(0, root.size(), root) == 0 &&
            book.path[root.size()] == '/');
}

static void normalize_book(RecentBook *book)
{
    if (!book) return;
    trim_line(&book->path);
    trim_line(&book->type);
    trim_line(&book->title);
    trim_line(&book->cover_path);
    if (!book->cover_path.empty() && !file_exists(book->cover_path)) book->cover_path.clear();
    if (book->type.empty()) {
        const char *dot = strrchr(book->path.c_str(), '.');
        if (dot && strcasecmp(dot, ".epub") == 0) book->type = "epub";
        else if (dot && (strcasecmp(dot, ".mobi") == 0 || strcasecmp(dot, ".prc") == 0 ||
                         strcasecmp(dot, ".azw") == 0 || strcasecmp(dot, ".azw3") == 0)) book->type = "mobi";
        else if (dot && strcasecmp(dot, ".fb2") == 0) book->type = "fb2";
        else if (dot && strcasecmp(dot, ".docx") == 0) book->type = "docx";
        else if (dot && (strcasecmp(dot, ".zip") == 0 || strcasecmp(dot, ".cbz") == 0)) book->type = "comic";
        else if (dot && (strcasecmp(dot, ".md") == 0 || strcasecmp(dot, ".markdown") == 0)) book->type = "markdown";
        else book->type = "txt";
    }
    // EPUB package titles are often extended with edition or promotional
    // text. The recent shelf is file-oriented, so always show the original
    // filename without its format suffix. The reader itself still uses the
    // OPF title.
    if (strcasecmp(book->type.c_str(), "epub") == 0 ||
        strcasecmp(book->type.c_str(), "mobi") == 0 ||
        strcasecmp(book->type.c_str(), "fb2") == 0 ||
        strcasecmp(book->type.c_str(), "markdown") == 0 ||
        strcasecmp(book->type.c_str(), "docx") == 0 ||
        strcasecmp(book->type.c_str(), "comic") == 0 || book->title.empty()) {
        book->title = recent_book_file_stem(book->path.c_str());
    }
}

static bool read_line(FILE *fp, std::string *line)
{
    if (!fp || !line) return false;
    line->clear();
    char chunk[256];
    while (std::fgets(chunk, sizeof(chunk), fp)) {
        line->append(chunk);
        if (!line->empty() && line->back() == '\n') break;
    }
    return !line->empty();
}

size_t recent_books_load(std::vector<RecentBook> *books)
{
    if (!books) return 0;
    books->clear();
    if (!recent_book_capacity::reserve_one(*books)) return 0;

    FILE *fp = std::fopen(app_paths::recent_books_file(), "rb");
    if (!fp) return 0;

    while (true) {
        RecentBook book;
        if (!read_line(fp, &book.path)) break;
        if (!read_line(fp, &book.type) || !read_line(fp, &book.title) || !read_line(fp, &book.cover_path)) break;
        normalize_book(&book);
        // WeRead owns its shelf and progress independently. Also reject legacy
        // entries by storage path in case an older build saved the wrong type.
        if (is_weread_record(book)) continue;
        if (book.path.empty() || !file_exists(book.path)) continue;

        bool duplicate = false;
        for (const auto &saved : *books) {
            if (saved.path == book.path) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            if (!recent_book_capacity::reserve_one(*books)) {
                ESP_LOGE("BOOKSHELF", "record capacity exhausted");
                break;
            }
            books->push_back(std::move(book));
        }
    }
    std::fclose(fp);
    return books->size();
}

bool recent_book_load(RecentBook *book)
{
    if (!book) return false;
    *book = RecentBook{};
    std::vector<RecentBook> books;
    if (recent_books_load(&books) == 0) return false;
    *book = std::move(books.front());
    return true;
}

bool recent_book_save(const char *path, const char *type, const char *title, const char *cover_path)
{
    if (!path || !path[0] || !type || !type[0]) return false;

    RecentBook candidate;
    candidate.path = path;
    candidate.type = type;
    if (is_weread_record(candidate)) return true;

    std::vector<RecentBook> books;
    recent_books_load(&books);
    RecentBook current;
    current.path = path;
    current.type = type;
    bool packaged_book = strcasecmp(type, "epub") == 0 || strcasecmp(type, "mobi") == 0 ||
                           strcasecmp(type, "fb2") == 0 || strcasecmp(type, "docx") == 0 ||
                           strcasecmp(type, "comic") == 0;
    current.title = packaged_book
                        ? recent_book_file_stem(path)
                        : ((title && title[0]) ? title : recent_book_base_name(path));
    current.cover_path = (cover_path && cover_path[0]) ? cover_path : "";

    for (auto it = books.begin(); it != books.end(); ++it) {
        if (it->path != current.path) continue;
        if (!packaged_book &&
            (!title || !title[0]) && !it->title.empty()) current.title = it->title;
        if ((!cover_path || !cover_path[0]) && !it->cover_path.empty()) current.cover_path = it->cover_path;
        books.erase(it);
        break;
    }
    if (!recent_book_capacity::reserve_one(books)) return false;
    books.insert(books.begin(), std::move(current));

    app_paths::ensure_data_dir();
    std::string temp_path = std::string(app_paths::recent_books_file()) + ".tmp";
    FILE *fp = std::fopen(temp_path.c_str(), "wb");
    if (!fp) return false;
    bool ok = true;
    for (const auto &book : books) {
        if (std::fprintf(fp, "%s\n%s\n%s\n%s\n",
                         book.path.c_str(), book.type.c_str(), book.title.c_str(), book.cover_path.c_str()) < 0) {
            ok = false;
            break;
        }
    }
    if (std::fclose(fp) != 0) ok = false;
    if (!ok) {
        std::remove(temp_path.c_str());
        return false;
    }
    std::remove(app_paths::recent_books_file());
    if (std::rename(temp_path.c_str(), app_paths::recent_books_file()) != 0) {
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

bool recent_books_rename_path(const char *old_path, const char *new_path)
{
    if (!old_path || !old_path[0] || !new_path || !new_path[0]) return false;
    // Timing survives renames even when the book has fallen out of the shelf.
    if (std::strcmp(old_path, new_path) != 0) {
        for (const char *suffix : {"", ".bak"}) {
            const std::string old_time = reading_time_path(old_path) + suffix;
            const std::string new_time = reading_time_path(new_path) + suffix;
            struct stat source = {}, destination = {};
            if (stat(old_time.c_str(), &source) == 0 &&
                stat(new_time.c_str(), &destination) != 0 &&
                std::rename(old_time.c_str(), new_time.c_str()) != 0) {
                ESP_LOGW("READING_TIME", "rename failed: %s", old_time.c_str());
                return false;
            }
        }
    }
    FILE *fp = std::fopen(app_paths::recent_books_file(), "rb");
    if (!fp) return true;  // No shelf state to migrate.

    std::vector<RecentBook> books;
    if (!recent_book_capacity::reserve_one(books)) {
        std::fclose(fp);
        return false;
    }
    while (true) {
        RecentBook book;
        if (!read_line(fp, &book.path)) break;
        if (!read_line(fp, &book.type) || !read_line(fp, &book.title) || !read_line(fp, &book.cover_path)) break;
        trim_line(&book.path);
        trim_line(&book.type);
        trim_line(&book.title);
        trim_line(&book.cover_path);
        if (book.path == old_path) {
            book.path = new_path;
            book.title = recent_book_file_stem(new_path);
        }
        if (!recent_book_capacity::reserve_one(books)) {
            std::fclose(fp);
            return false; // Never replace the index with a truncated list.
        }
        books.push_back(std::move(book));
    }
    std::fclose(fp);

    app_paths::ensure_data_dir();
    std::string temp_path = std::string(app_paths::recent_books_file()) + ".tmp";
    fp = std::fopen(temp_path.c_str(), "wb");
    if (!fp) return false;
    bool ok = true;
    for (const auto &book : books) {
        if (std::fprintf(fp, "%s\n%s\n%s\n%s\n", book.path.c_str(), book.type.c_str(),
                         book.title.c_str(), book.cover_path.c_str()) < 0) {
            ok = false;
            break;
        }
    }
    if (std::fclose(fp) != 0) ok = false;
    if (!ok) {
        std::remove(temp_path.c_str());
        return false;
    }
    std::remove(app_paths::recent_books_file());
    if (std::rename(temp_path.c_str(), app_paths::recent_books_file()) != 0) {
        std::remove(temp_path.c_str());
        return false;
    }

    const std::string old_key = recent_book_progress_key(old_path);
    const std::string new_key = recent_book_progress_key(new_path);
    const char *dirs[] = {app_paths::txt_progress_dir(), app_paths::epub_progress_dir(),
                          app_paths::mobi_progress_dir(), app_paths::fb2_progress_dir()};
    for (const char *dir : dirs) {
        std::string old_progress = std::string(dir) + "/" + old_key + ".pos";
        std::string new_progress = std::string(dir) + "/" + new_key + ".pos";
        struct stat st = {};
        if (stat(old_progress.c_str(), &st) == 0) {
            std::remove(new_progress.c_str());
            std::rename(old_progress.c_str(), new_progress.c_str());
        }
    }
    return true;
}
