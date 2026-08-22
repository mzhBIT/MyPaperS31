#include "parser/txt_parser.h"
#include "gbk_unicode_table.h"
#include "reader_font.h"
#include "reader_style.h"
#include "renderer/TextRenderer.h"
#include "storage/app_paths.h"

#include <M5Unified.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <vector>
#include <sys/stat.h>

static const char *TAG = "TXT";
static const uint32_t PROGRESS_MAGIC = 0x54585032; // TXP2
static const uint32_t PROGRESS_VERSION = 1;
static const uint32_t MAX_SAVED_HISTORY = 64;

enum Enc { UTF8, UTF16LE, GBK, LATIN1 };

static bool is_utf8(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < std::min(len, (size_t)4096);) {
        if (data[i] < 0x80) {
            i++;
            continue;
        }
        int elen = 0;
        if ((data[i] & 0xE0) == 0xC0) elen = 2;
        else if ((data[i] & 0xF0) == 0xE0) elen = 3;
        else if ((data[i] & 0xF8) == 0xF0) elen = 4;
        else return false;
        if (i + elen > len) return false;
        for (int j = 1; j < elen; j++) {
            if ((data[i + j] & 0xC0) != 0x80) return false;
        }
        i += elen;
    }
    return true;
}

static bool is_gbk(const uint8_t *data, size_t len)
{
    size_t gbk = 0, ascii = 0, invalid = 0;
    len = std::min(len, (size_t)4096);
    for (size_t i = 0; i < len;) {
        if (data[i] < 0x80) {
            ascii++;
            i++;
            continue;
        }
        if (data[i] >= 0x81 && data[i] <= 0xFE && i + 1 < len &&
            data[i + 1] >= 0x40 && data[i + 1] <= 0xFE && data[i + 1] != 0x7F) {
            gbk++;
            i += 2;
            continue;
        }
        invalid++;
        if (invalid > gbk / 10 + 2) return false;
        i++;
    }
    return gbk > 0 && gbk > ascii / 10;
}

static Enc detect_encoding(const uint8_t *data, size_t len)
{
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) return UTF8;
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE) return UTF16LE;
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) return UTF16LE;
    if (is_utf8(data, len)) return UTF8;
    if (is_gbk(data, len)) return GBK;
    return LATIN1;
}

static uint32_t gbk_to_ucs(uint8_t hi, uint8_t lo)
{
    int idx = gbk_table_index(hi, lo);
    if (idx < 0) return 0xFFFD;
    uint32_t cp = gbk_unicode_table[idx];
    return cp ? cp : 0xFFFD;
}

static std::string cp_to_utf8(uint32_t cp)
{
    std::string out;
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

static bool read_cp(FILE *fp, Enc enc, uint32_t &cp, uint32_t &start, uint32_t &next)
{
    start = (uint32_t)ftell(fp);
    int c0 = fgetc(fp);
    if (c0 == EOF) return false;

    switch (enc) {
    case LATIN1:
        cp = (uint8_t)c0;
        break;

    case GBK:
        if ((uint8_t)c0 < 0x80) {
            cp = (uint8_t)c0;
        } else {
            int c1 = fgetc(fp);
            cp = (c1 == EOF) ? 0xFFFD : gbk_to_ucs((uint8_t)c0, (uint8_t)c1);
        }
        break;

    case UTF16LE: {
        int c1 = fgetc(fp);
        if (c1 == EOF) return false;
        cp = (uint8_t)c0 | ((uint8_t)c1 << 8);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            int c2 = fgetc(fp);
            int c3 = fgetc(fp);
            if (c2 != EOF && c3 != EOF) {
                uint32_t lo = (uint8_t)c2 | ((uint8_t)c3 << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
            }
        }
        break;
    }

    case UTF8:
    default:
        if ((uint8_t)c0 < 0x80) {
            cp = (uint8_t)c0;
        } else {
            int len = 1;
            if (((uint8_t)c0 & 0xE0) == 0xC0) len = 2;
            else if (((uint8_t)c0 & 0xF0) == 0xE0) len = 3;
            else if (((uint8_t)c0 & 0xF8) == 0xF0) len = 4;
            cp = 0xFFFD;
            uint8_t b[4] = {(uint8_t)c0, 0, 0, 0};
            bool ok = true;
            for (int i = 1; i < len; ++i) {
                int cx = fgetc(fp);
                if (cx == EOF || ((uint8_t)cx & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                b[i] = (uint8_t)cx;
            }
            if (ok) {
                if (len == 2) cp = ((b[0] & 0x1F) << 6) | (b[1] & 0x3F);
                else if (len == 3) cp = ((b[0] & 0x0F) << 12) | ((b[1] & 0x3F) << 6) | (b[2] & 0x3F);
                else if (len == 4) cp = ((b[0] & 0x07) << 18) | ((b[1] & 0x3F) << 12) | ((b[2] & 0x3F) << 6) | (b[3] & 0x3F);
            }
        }
        break;
    }

    next = (uint32_t)ftell(fp);
    return true;
}

static int display_width()
{
    ReaderStyle style = reader_style_get();
    int width = M5.Display.width() - style.margin_left - style.margin_right;
    return width > 100 ? width : 100;
}

static int lines_per_page()
{
    ReaderStyle style = reader_style_get();
    int lh = reader_text_renderer().lineHeight();
    if (lh <= 0) lh = 48;
    int usable_h = M5.Display.height() - 50 - style.margin_top - style.margin_bottom;
    int n = usable_h / lh;
    return n > 1 ? n : 1;
}

static int estimate_codepoint_width(uint32_t cp)
{
    int measured = reader_text_renderer().measureCodepointForLayout(cp);
    if (measured > 0) return measured;

    if (cp == '\t') return 72;
    if (cp < 0x80) {
        if (cp == ' ') return 12;
        if (cp == '.' || cp == ',' || cp == ':' || cp == ';' || cp == '\'' || cp == '"') return 10;
        if (cp == 'i' || cp == 'l' || cp == 'I' || cp == '!' || cp == '|') return 10;
        if (cp == 'm' || cp == 'w' || cp == 'M' || cp == 'W') return 28;
        return 20;
    }
    if ((cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x3000 && cp <= 0x303F)) return 36;
    return 36;
}

static uint32_t hash32_bytes(const void *data, size_t len, uint32_t h = 2166136261u)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash32_string(const std::string &s, uint32_t h = 2166136261u)
{
    return hash32_bytes(s.data(), s.size(), h);
}

void TxtBook::ensure_progress_root()
{
    app_paths::ensure_dir(app_paths::txt_progress_dir());
}

std::string TxtBook::cache_key(const char *path)
{
    size_t h = 0;
    for (const char *p = path; *p; p++) h = h * 31 + (unsigned char)*p;
    char buf[32];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
    return buf;
}

uint32_t TxtBook::layout_signature() const
{
    std::string sig = reader_text_renderer().layoutSignature();
    char buf[128];
    snprintf(buf, sizeof(buf), ";w=%d;h=%d;dw=%d;lpp=%d;enc=%d;file=%lu",
             (int)M5.Display.width(),
             (int)M5.Display.height(),
             display_width(),
             lines_per_page(),
             _enc,
             (unsigned long)_file_size);
    sig += buf;
    return hash32_string(sig);
}

bool TxtBook::open(const char *path)
{
    close();

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    _file_size = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (_file_size == 0) {
        fclose(fp);
        return false;
    }

    const char *bn = strrchr(path, '/');
    if (!bn) bn = strrchr(path, '\\');
    title = bn ? (bn + 1) : path;
    auto dot = title.rfind('.');
    if (dot != std::string::npos) title = title.substr(0, dot);

    uint8_t head[4096];
    size_t head_len = fread(head, 1, sizeof(head), fp);
    Enc enc = detect_encoding(head, head_len);
    _enc = (int)enc;
    if (head_len >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) {
        _content_offset = 3;
    } else if (head_len >= 2 &&
               ((head[0] == 0xFF && head[1] == 0xFE) || (head[0] == 0xFE && head[1] == 0xFF))) {
        _content_offset = 2;
    } else {
        _content_offset = 0;
    }
    fclose(fp);

    refresh_layout_settings();
    _layout_sig = layout_signature();

    ensure_progress_root();
    _path = path;
    _progress_path = std::string(app_paths::txt_progress_dir()) + "/" + cache_key(path) + ".pos";

    const char *enc_names[] = {"UTF-8", "UTF-16LE", "GBK", "Latin1"};
    const char *renderer_name = reader_text_renderer().usingFreeType()
                                    ? "FreeType"
                                    : (reader_text_renderer().usingBinFont() ? "BinFont" : "fallback");
    ESP_LOGI(TAG, "%s: enc=%s size=%lu renderer=%s",
             title.c_str(),
             enc_names[enc],
             (unsigned long)_file_size,
             renderer_name);
    return true;
}

bool TxtBook::refresh_layout_settings()
{
    ReaderTextStyle style;
    style.font_path = reader_font_path();
    ReaderStyle reader_style = reader_style_get();
    style.font_size = reader_style.font_size;
    style.line_spacing = reader_style.line_spacing;
    bool ok = reader_text_renderer().begin(style);
    _layout_sig = layout_signature();
    return ok;
}

void TxtBook::close()
{
    title.clear();
    _path.clear();
    _progress_path.clear();
    _file_size = 0;
    _content_offset = 0;
    _layout_sig = 0;
    _enc = 0;
}

TxtPage TxtBook::read_page(uint32_t offset)
{
    TxtPage page;
    page.start_offset = offset;
    page.next_offset = offset;
    page.eof = true;

    if (_path.empty()) return page;
    if (offset > _file_size) offset = _file_size;
    if (offset < _content_offset) offset = _content_offset;
    page.start_offset = offset;
    page.next_offset = offset;

    FILE *fp = fopen(_path.c_str(), "rb");
    if (!fp) return page;
    fseek(fp, offset, SEEK_SET);

    const int max_w = std::max(100, display_width());
    const int max_lines = lines_per_page();
    int line_count = 0;
    int line_width = 0;
    std::string line;
    line.reserve(256);
    page.text.reserve(2048);
    int64_t t0 = esp_timer_get_time();

    while (line_count < max_lines) {
        uint32_t cp = 0, cp_start = 0, cp_next = 0;
        if (!read_cp(fp, (Enc)_enc, cp, cp_start, cp_next)) {
            if (!line.empty() && line_count < max_lines) {
                page.text += line;
                page.text += '\n';
                line_count++;
            }
            page.next_offset = _file_size;
            page.eof = true;
            break;
        }

        if (cp == '\r') {
            page.next_offset = cp_next;
            continue;
        }

        if (cp == '\n') {
            page.text += line;
            page.text += '\n';
            line.clear();
            line_width = 0;
            line_count++;
            page.next_offset = cp_next;
            if (line_count >= max_lines) {
                page.eof = cp_next >= _file_size;
                break;
            }
            continue;
        }

        std::string ch = cp_to_utf8(cp);
        int ch_width = estimate_codepoint_width(cp);
        if (!line.empty() && line_width + ch_width > max_w) {
            page.text += line;
            page.text += '\n';
            line.clear();
            line_width = 0;
            line_count++;
            if (line_count >= max_lines) {
                page.next_offset = cp_start;
                page.eof = cp_start >= _file_size;
                break;
            }
        }

        line += ch;
        line_width += ch_width;
        page.next_offset = cp_next;
    }

    fclose(fp);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "read_page offset=%lu next=%lu lines=%d eof=%d time=%lld ms",
             (unsigned long)page.start_offset,
             (unsigned long)page.next_offset,
             line_count,
             page.eof ? 1 : 0,
             (t1 - t0) / 1000);
    return page;
}

uint32_t TxtBook::find_window_start(uint32_t offset, uint32_t back_bytes) const
{
    if (offset <= _content_offset) return _content_offset;
    uint32_t base = offset > back_bytes ? offset - back_bytes : _content_offset;
    if (base < _content_offset) base = _content_offset;
    if (base == _content_offset) return base;

    FILE *fp = fopen(_path.c_str(), "rb");
    if (!fp) return base;

    uint32_t search_limit = std::min<uint32_t>(4096, base - _content_offset);
    uint32_t start = base - search_limit;
    fseek(fp, start, SEEK_SET);

    std::vector<uint8_t> buf(search_limit);
    size_t n = fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);

    for (int i = (int)n - 1; i >= 0; --i) {
        if (buf[i] == '\n') {
            return start + (uint32_t)i + 1;
        }
    }

    if (_enc == UTF8) {
        FILE *afp = fopen(_path.c_str(), "rb");
        if (!afp) return base;
        while (base > _content_offset) {
            fseek(afp, base, SEEK_SET);
            int c = fgetc(afp);
            if (c == EOF || (((uint8_t)c & 0xC0) != 0x80)) break;
            base--;
        }
        fclose(afp);
    } else if (_enc == UTF16LE && ((base - _content_offset) & 1)) {
        base--;
    }
    return base < _content_offset ? _content_offset : base;
}

uint32_t TxtBook::align_page_offset_near(uint32_t offset)
{
    if (offset <= _content_offset) return _content_offset;
    if (offset > _file_size) offset = _file_size;

    uint32_t windows[] = {2048, 8192, 32768, 131072};
    uint32_t best = _content_offset;

    for (uint32_t window : windows) {
        uint32_t cur = find_window_start(offset, window);
        best = cur;
        int guard = 0;

        while (cur < offset && guard++ < 256) {
            TxtPage page = read_page(cur);
            if (page.next_offset == 0 || page.next_offset <= cur) break;
            if (page.next_offset >= offset) return page.start_offset;
            best = page.start_offset;
            cur = page.next_offset;
        }
    }

    return best;
}

uint32_t TxtBook::previous_page_offset(uint32_t offset)
{
    if (_path.empty()) return _content_offset;
    if (offset <= _content_offset) return _content_offset;
    if (offset > _file_size) offset = _file_size;

    int64_t t0 = esp_timer_get_time();
    uint32_t windows[] = {2048, 8192, 32768, 131072};
    uint32_t result = _content_offset;
    int scanned_total = 0;

    for (uint32_t window : windows) {
        uint32_t cur = find_window_start(offset, window);
        uint32_t prev = cur;
        uint32_t prior = cur;
        int scanned = 0;

        while (cur < offset && scanned++ < 256) {
            TxtPage page = read_page(cur);
            scanned_total++;
            if (page.next_offset == 0 || page.next_offset <= cur) break;
            if (page.next_offset >= offset) {
                result = (page.next_offset == offset) ? page.start_offset : prior;
                int64_t t1 = esp_timer_get_time();
                ESP_LOGI(TAG, "previous_page_offset current=%lu prev=%lu window=%lu scanned=%d time=%lld ms",
                         (unsigned long)offset,
                         (unsigned long)result,
                         (unsigned long)window,
                         scanned_total,
                         (t1 - t0) / 1000);
                return result;
            }
            prev = page.start_offset;
            prior = page.start_offset;
            cur = page.next_offset;
        }

        result = prev;
    }

    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "previous_page_offset current=%lu prev=%lu window=fallback scanned=%d time=%lld ms",
             (unsigned long)offset,
             (unsigned long)result,
             scanned_total,
             (t1 - t0) / 1000);
    return result;
}

uint32_t TxtBook::seek_percent(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t rough = (uint32_t)(((uint64_t)_file_size * percent) / 100);
    if (rough < _content_offset) rough = _content_offset;
    uint32_t aligned = align_page_offset_near(rough);
    ESP_LOGI(TAG, "seek_percent percent=%u rough=%lu aligned=%lu",
             percent,
             (unsigned long)rough,
             (unsigned long)aligned);
    return aligned;
}

void TxtBook::save_progress(uint32_t offset, const std::vector<uint32_t> &history)
{
    if (_progress_path.empty()) return;
    FILE *fp = fopen(_progress_path.c_str(), "wb");
    if (!fp) return;

    uint32_t magic = PROGRESS_MAGIC;
    uint32_t version = PROGRESS_VERSION;
    uint32_t layout = _layout_sig;
    uint32_t count = std::min<uint32_t>((uint32_t)history.size(), MAX_SAVED_HISTORY);
    fwrite(&magic, 1, sizeof(magic), fp);
    fwrite(&version, 1, sizeof(version), fp);
    fwrite(&layout, 1, sizeof(layout), fp);
    fwrite(&offset, 1, sizeof(offset), fp);
    fwrite(&count, 1, sizeof(count), fp);
    if (count > 0) {
        const uint32_t start = (uint32_t)history.size() - count;
        fwrite(history.data() + start, sizeof(uint32_t), count, fp);
    }
    fclose(fp);
}

uint32_t TxtBook::load_progress(std::vector<uint32_t> *history)
{
    if (history) history->clear();
    if (_progress_path.empty()) return _content_offset;

    FILE *fp = fopen(_progress_path.c_str(), "rb");
    if (!fp) return _content_offset;

    uint32_t magic = 0;
    if (fread(&magic, 1, sizeof(magic), fp) != sizeof(magic)) {
        fclose(fp);
        return _content_offset;
    }

    if (magic != PROGRESS_MAGIC) {
        uint32_t old_offset = magic;
        fclose(fp);
        if (old_offset > _file_size) return _content_offset;
        return old_offset < _content_offset ? _content_offset : old_offset;
    }

    uint32_t version = 0, saved_layout = 0, offset = _content_offset, count = 0;
    bool ok = fread(&version, 1, sizeof(version), fp) == sizeof(version) &&
              fread(&saved_layout, 1, sizeof(saved_layout), fp) == sizeof(saved_layout) &&
              fread(&offset, 1, sizeof(offset), fp) == sizeof(offset) &&
              fread(&count, 1, sizeof(count), fp) == sizeof(count);

    if (!ok || version != PROGRESS_VERSION) {
        fclose(fp);
        return _content_offset;
    }

    bool layout_ok = saved_layout == _layout_sig;
    count = std::min<uint32_t>(count, MAX_SAVED_HISTORY);
    if (history && layout_ok && count > 0) {
        history->resize(count);
        size_t n = fread(history->data(), sizeof(uint32_t), count, fp);
        if (n != count) history->clear();
    }
    fclose(fp);

    if (!layout_ok && history) history->clear();
    if (offset > _file_size) return _content_offset;
    return offset < _content_offset ? _content_offset : offset;
}

void TxtBook::save_progress_offset(uint32_t offset)
{
    std::vector<uint32_t> empty;
    save_progress(offset, empty);
}

uint32_t TxtBook::load_progress_offset()
{
    return load_progress(nullptr);
}
