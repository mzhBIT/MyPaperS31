#pragma once
#include <string>
#include <vector>

namespace rss_http {
constexpr unsigned max_redirects = 3;
inline bool redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
inline bool split_url(const std::string& url, std::string& origin, std::string& host) {
    const size_t start = url.compare(0, 8, "https://") == 0 ? 8 :
                         url.compare(0, 7, "http://") == 0 ? 7 : 0;
    if (!start || url.find_first_of("\r\n\t ") != std::string::npos) return false;
    const size_t end = url.find_first_of("/?#", start);
    origin = url.substr(0, end);
    const std::string authority = origin.substr(start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(1, close - 1);
    } else {
        host = authority.substr(0, authority.find(':'));
    }
    return !host.empty();
}
// Resolve public-feed redirects without changing the HTTPS hostname/SNI to an
// IP address. Explicitly disallow TLS downgrade, invalid schemes and credentials.
inline bool redirect_target(const std::string& base, const std::string& location,
                            std::string& out) {
    if (location.empty() || location.size() > 2048 ||
        location.find_first_of("\r\n\t ") != std::string::npos) return false;
    std::string origin, host;
    if (!split_url(base, origin, host)) return false;
    if (location.compare(0, 2, "//") == 0) {
        out = base.substr(0, base.find(':') + 1) + location;
    } else if (location.compare(0, 7, "http://") == 0 ||
               location.compare(0, 8, "https://") == 0) {
        out = location;
    } else {
        // A colon in the first path segment denotes another URI scheme.
        if (location.find(':') < location.find_first_of("/?#")) return false;
        std::string path = base.substr(origin.size());
        path.erase(path.find_first_of("?#") == std::string::npos ? path.size() : path.find_first_of("?#"));
        if (path.empty()) path = "/";
        if (location[0] == '/') out = origin + location;
        else if (location[0] == '?') out = origin + path + location;
        else if (location[0] == '#') out = base.substr(0, base.find('#')) + location;
        else out = origin + path.substr(0, path.rfind('/') + 1) + location;
    }
    out.erase(out.find('#') == std::string::npos ? out.size() : out.find('#'));
    std::string target_origin;
    if (!split_url(out, target_origin, host)) return false;
    if (base.compare(0, 8, "https://") == 0 && out.compare(0, 8, "https://") != 0) return false;
    // Normalize dot segments in relative Location paths.
    const size_t query = out.find('?', target_origin.size());
    const std::string suffix = query == std::string::npos ? "" : out.substr(query);
    const std::string path = out.substr(target_origin.size(), query == std::string::npos
        ? std::string::npos : query - target_origin.size());
    std::vector<std::string> parts;
    for (size_t p = 0; p < path.size();) {
        const size_t end = path.find('/', p);
        const std::string part = path.substr(p, end == std::string::npos ? end : end - p);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        if (end == std::string::npos) break;
        p = end + 1;
    }
    out = target_origin + "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    if (path.size() > 1 && path.back() == '/' && out.back() != '/') out += '/';
    out += suffix;
    return true;
}
} // namespace rss_http
