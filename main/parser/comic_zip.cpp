#include "parser/comic_zip.h"
#include <algorithm>
#include <cctype>
#include <esp_log.h>

namespace {
std::string image_extension(const std::string &name)
{
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".jpeg") ext = ".jpg";
    return ext == ".jpg" || ext == ".png" || ext == ".bmp" ? ext : "";
}
bool hidden(const std::string &path)
{
    for (size_t p = 0; p < path.size();) {
        const size_t end = path.find('/', p);
        const std::string part = path.substr(p, end == std::string::npos ? end : end - p);
        if (part.empty() || part[0] == '.' || part == "__MACOSX") return true;
        if (end == std::string::npos) break;
        p = end + 1;
    }
    return path.empty() || path.back() == '/';
}
// Compare arbitrary-length digit runs without integer overflow.
bool natural_less(const std::string &a, const std::string &b)
{
    size_t x = 0, y = 0;
    while (x < a.size() && y < b.size()) {
        if (std::isdigit((unsigned char)a[x]) && std::isdigit((unsigned char)b[y])) {
            size_t ex = x, ey = y;
            while (ex < a.size() && std::isdigit((unsigned char)a[ex])) ++ex;
            while (ey < b.size() && std::isdigit((unsigned char)b[ey])) ++ey;
            size_t sx = x, sy = y;
            while (sx < ex && a[sx] == '0') ++sx;
            while (sy < ey && b[sy] == '0') ++sy;
            if (ex - sx != ey - sy) return ex - sx < ey - sy;
            const int cmp = a.compare(sx, ex - sx, b, sy, ey - sy);
            if (cmp) return cmp < 0;
            // Same numeric value: fewer leading zeros first.
            if (ex - x != ey - y) return ex - x < ey - y;
            x = ex; y = ey;
        } else {
            const int ca = std::tolower((unsigned char)a[x]), cb = std::tolower((unsigned char)b[y]);
            if (ca != cb) return ca < cb;
            ++x; ++y;
        }
    }
    if (x != a.size() || y != b.size()) return x == a.size();
    return a < b; // deterministic tie-break for case-only differences
}
}

bool ComicZip::open(const char *path)
{
    close();
    if (!path || !zip_.open(path)) { error_ = "Could not open ZIP comic (ZIP64 is unsupported)"; return false; }
    auto names = zip_.entry_names();
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string &name) {
        return hidden(name) || image_extension(name).empty();
    }), names.end());
    std::sort(names.begin(), names.end(), natural_less);
    for (const auto &name : names) {
        EpubZip::Entry entry;
        if (!zip_.entry_info(name, &entry)) continue;
        if ((entry.flags & 1) || (entry.method != 0 && entry.method != 8)) {
            error_ = "ZIP comic contains encrypted or unsupported images";
            pages_.clear(); resources_.clear(); return false;
        }
        if (!entry.uncompressed_size) continue;
        if (entry.uncompressed_size > 12 * 1024 * 1024) {
            error_ = "ZIP comic image exceeds 12 MiB limit";
            pages_.clear(); resources_.clear(); return false;
        }
        const std::string index = std::to_string(pages_.size());
        // Aliases preserve ZIP names containing #, ?, &, non-UTF8 bytes etc.
        // Only the original name is passed to ZIP reads, never to XHTML URLs.
        const std::string image = "comic-image-" + index + image_extension(name);
        pages_.push_back({"comic-page-" + index + ".html", image, name});
        resources_[image] = name;
    }
    if (pages_.empty()) { error_ = "ZIP comic has no JPG, PNG or BMP images"; return false; }
    ESP_LOGI("COMIC_ZIP", "opened pages=%u path=%s", (unsigned)pages_.size(), path);
    return true;
}
void ComicZip::close()
{ zip_.close(); pages_.clear(); resources_.clear(); error_.clear(); }
bool ComicZip::read_page(const std::string &href, std::string *out) const
{
    if (!out) return false;
    for (const auto &page : pages_) if (page.href == href) {
        *out = "<html><body><img src=\"" + page.image_href + "\"/></body></html>";
        return true;
    }
    return false;
}
bool ComicZip::read_prefix(const std::string &href, std::vector<uint8_t> *out, size_t size)
{
    auto it = resources_.find(href);
    return it != resources_.end() && zip_.read_entry_prefix(it->second, out, size);
}
bool ComicZip::read_resource(const std::string &href, std::vector<uint8_t> *out, uint32_t size)
{
    auto it = resources_.find(href);
    return it != resources_.end() && zip_.read_entry_to_memory(it->second, out, size);
}
bool ComicZip::extract_resource(const std::string &href, const std::string &path)
{
    auto it = resources_.find(href);
    return it != resources_.end() && zip_.extract_entry_to_file(it->second, path);
}
