#include "deepseek/DeepSeekSessionStore.h"

#include "storage/app_paths.h"

#include <M5Unified.h>
#include <dirent.h>
#include <esp_random.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

std::string join_path(const std::string &left, const char *right)
{
    return left + "/" + right;
}

std::string session_dir(const std::string &id)
{
    return std::string(app_paths::deepseek_sessions_dir()) + "/" + id;
}

uint64_t rtc_stamp()
{
    auto dt = M5.Rtc.getDateTime();
    return static_cast<uint64_t>(dt.date.year) * 10000000000ULL +
           static_cast<uint64_t>(dt.date.month) * 100000000ULL +
           static_cast<uint64_t>(dt.date.date) * 1000000ULL +
           static_cast<uint64_t>(dt.time.hours) * 10000ULL +
           static_cast<uint64_t>(dt.time.minutes) * 100ULL +
           static_cast<uint64_t>(dt.time.seconds);
}

std::string field_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
    }
    return out;
}

std::string field_unescape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    bool escaped = false;
    for (char c : value) {
        if (!escaped && c == '\\') {
            escaped = true;
            continue;
        }
        if (escaped) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else out.push_back(c);
            escaped = false;
        } else {
            out.push_back(c);
        }
    }
    if (escaped) out.push_back('\\');
    return out;
}

std::string json_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 16);
    for (unsigned char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20) out.push_back(static_cast<char>(c));
            break;
        }
    }
    return out;
}

bool parse_json_string(const std::string &line, const char *key, std::string *value)
{
    std::string marker = std::string("\"") + key + "\":\"";
    size_t pos = line.find(marker);
    if (pos == std::string::npos) return false;
    pos += marker.size();
    std::string out;
    bool escaped = false;
    for (; pos < line.size(); ++pos) {
        char c = line[pos];
        if (!escaped && c == '"') {
            *value = std::move(out);
            return true;
        }
        if (!escaped && c == '\\') {
            escaped = true;
            continue;
        }
        if (escaped) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else out.push_back(c);
            escaped = false;
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool read_line(FILE *file, std::string *line)
{
    line->clear();
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), file)) {
        line->append(buffer);
        if (!line->empty() && line->back() == '\n') {
            line->pop_back();
            if (!line->empty() && line->back() == '\r') line->pop_back();
            return true;
        }
    }
    return !line->empty();
}

bool valid_session_id(const std::string &id)
{
    if (id.empty()) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

bool remove_file_if_present(const std::string &path)
{
    if (unlink(path.c_str()) == 0) return true;
    return errno == ENOENT;
}

}  // namespace

bool DeepSeekSessionStore::list(std::vector<DeepSeekSessionMeta> *sessions) const
{
    if (!sessions) return false;
    sessions->clear();
    app_paths::ensure_dir(app_paths::deepseek_sessions_dir());
    DIR *dir = opendir(app_paths::deepseek_sessions_dir());
    if (!dir) return false;
    while (dirent *entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        DeepSeekSessionMeta meta;
        if (load_meta(entry->d_name, &meta)) sessions->push_back(std::move(meta));
    }
    closedir(dir);
    std::sort(sessions->begin(), sessions->end(), [](const auto &a, const auto &b) {
        return a.updated > b.updated;
    });
    return true;
}

bool DeepSeekSessionStore::create(DeepSeekModel model, bool thinking,
                                  DeepSeekSessionMeta *session) const
{
    if (!session) return false;
    app_paths::ensure_dir(app_paths::deepseek_sessions_dir());
    uint64_t stamp = rtc_stamp();
    char id[40];
    snprintf(id, sizeof(id), "%014llu-%08lx",
             static_cast<unsigned long long>(stamp),
             static_cast<unsigned long>(esp_random()));
    *session = {};
    session->id = id;
    session->title = "New Chat";
    session->model = model;
    session->thinking = thinking;
    session->created = stamp;
    session->updated = stamp;
    if (mkdir(session_dir(session->id).c_str(), 0775) != 0) return false;
    return save_meta(*session);
}

bool DeepSeekSessionStore::load_meta(const std::string &id, DeepSeekSessionMeta *session) const
{
    if (!session || id.empty()) return false;
    FILE *file = fopen(join_path(session_dir(id), "meta.tsv").c_str(), "rb");
    if (!file) return false;
    DeepSeekSessionMeta result;
    std::string lines[5];
    bool ok = true;
    for (auto &line : lines) ok = ok && read_line(file, &line);
    std::string thinking;
    bool has_thinking = ok && read_line(file, &thinking);
    fclose(file);
    if (!ok) return false;
    result.id = field_unescape(lines[0]);
    result.title = field_unescape(lines[1]);
    result.model = lines[2] == "deepseek-v4-pro" ? DeepSeekModel::Pro : DeepSeekModel::Flash;
    result.created = strtoull(lines[3].c_str(), nullptr, 10);
    result.updated = strtoull(lines[4].c_str(), nullptr, 10);
    if (has_thinking) result.thinking = thinking == "1";
    if (result.id.empty()) result.id = id;
    *session = std::move(result);
    return true;
}

bool DeepSeekSessionStore::load_messages(const std::string &id,
                                         std::vector<DeepSeekMessage> *messages) const
{
    if (!messages) return false;
    messages->clear();
    FILE *file = fopen(join_path(session_dir(id), "messages.jsonl").c_str(), "rb");
    if (!file) return true;
    std::string line;
    while (read_line(file, &line)) {
        DeepSeekMessage message;
        if (parse_json_string(line, "role", &message.role) &&
            parse_json_string(line, "content", &message.content)) {
            messages->push_back(std::move(message));
        }
    }
    fclose(file);
    return true;
}

bool DeepSeekSessionStore::remove(const std::string &id) const
{
    if (!valid_session_id(id)) return false;
    const std::string dir = session_dir(id);
    bool ok = remove_file_if_present(join_path(dir, "messages.jsonl"));
    ok = remove_file_if_present(join_path(dir, "meta.tsv.tmp")) && ok;
    ok = remove_file_if_present(join_path(dir, "meta.tsv")) && ok;
    if (rmdir(dir.c_str()) != 0 && errno != ENOENT) ok = false;
    return ok;
}

bool DeepSeekSessionStore::append_message(DeepSeekSessionMeta *session,
                                          const std::string &role,
                                          const std::string &content) const
{
    if (!session || session->id.empty() || role.empty()) return false;
    FILE *file = fopen(join_path(session_dir(session->id), "messages.jsonl").c_str(), "ab");
    if (!file) return false;
    std::string line = "{\"role\":\"" + json_escape(role) +
                       "\",\"content\":\"" + json_escape(content) + "\"}\n";
    bool ok = fwrite(line.data(), 1, line.size(), file) == line.size();
    ok = fclose(file) == 0 && ok;
    if (!ok) return false;
    session->updated = rtc_stamp();
    if (session->title == "New Chat" && role == "user" && !content.empty()) {
        size_t bytes = std::min<size_t>(content.size(), 48);
        while (bytes > 0 && bytes < content.size() &&
               (static_cast<unsigned char>(content[bytes]) & 0xC0) == 0x80) --bytes;
        session->title = content.substr(0, bytes);
    }
    return save_meta(*session);
}

bool DeepSeekSessionStore::set_model(DeepSeekSessionMeta *session, DeepSeekModel model) const
{
    if (!session) return false;
    session->model = model;
    session->updated = rtc_stamp();
    return save_meta(*session);
}

bool DeepSeekSessionStore::set_thinking(DeepSeekSessionMeta *session, bool enabled) const
{
    if (!session) return false;
    session->thinking = enabled;
    session->updated = rtc_stamp();
    return save_meta(*session);
}

bool DeepSeekSessionStore::save_meta(const DeepSeekSessionMeta &session) const
{
    std::string dir = session_dir(session.id);
    std::string path = join_path(dir, "meta.tsv");
    std::string temp = path + ".tmp";
    FILE *file = fopen(temp.c_str(), "wb");
    if (!file) return false;
    int written = fprintf(file, "%s\n%s\n%s\n%llu\n%llu\n%d\n",
                          field_escape(session.id).c_str(),
                          field_escape(session.title).c_str(),
                          deepseek_model_id(session.model),
                          static_cast<unsigned long long>(session.created),
                          static_cast<unsigned long long>(session.updated),
                          session.thinking ? 1 : 0);
    bool ok = written > 0 && fclose(file) == 0;
    if (!ok) {
        remove(temp.c_str());
        return false;
    }
    remove(path.c_str());
    return rename(temp.c_str(), path.c_str()) == 0;
}
