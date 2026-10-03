#include "map/AmapClient.h"
#include "weread/WereadJson.h"
#include <cmath>
#include <cstdlib>

namespace {
std::string field(std::string_view object, const char *name)
{
    std::string_view raw; std::string value;
    if (weread::json::member(object, name, &raw)) weread::json::string(raw, &value);
    return value;
}
bool point(const std::string &text, double &lon, double &lat)
{
    char *end = nullptr; lon = std::strtod(text.c_str(), &end);
    if (!end || end == text.c_str() || *end != ',') return false;
    const char *start = end+1; lat = std::strtod(start, &end);
    return end != start && !*end && std::isfinite(lon) && std::isfinite(lat) &&
           std::abs(lon) <= 180 && std::abs(lat) <= 85.051129;
}
}

AmapSearchResult AmapClient::parse_location_response(std::string_view body, bool ip,
                                                      double lon, double lat)
{
    AmapSearchResult result;
    if (field(body, "status") != "1") {
        result = parse_search_response(body); // Share API error-code handling.
        return result;
    }
    AmapPlace place;
    if (ip) {
        const auto rectangle = field(body, "rectangle");
        const auto separator = rectangle.find(';');
        double west, south, east, north;
        if (separator == std::string::npos || !point(rectangle.substr(0,separator),west,south) ||
            !point(rectangle.substr(separator+1),east,north) || west >= east || south >= north) {
            result.error = "IP location unavailable"; return result;
        }
        place.longitude = (west+east)/2; place.latitude = (south+north)/2;
        const auto province = field(body,"province"), city = field(body,"city");
        place.name = province == city ? city : province+city;
        if (place.name.empty()) { result.error = "IP location unavailable"; return result; }
        place.address = "IP city estimate";
    } else {
        std::string_view regeo;
        if (!weread::json::member(body,"regeocode",&regeo)) {
            result.error = "Invalid location response"; return result;
        }
        place.address = field(regeo,"formatted_address");
        if (place.address.empty()) { result.error = "Address unavailable"; return result; }
        std::string_view component;
        weread::json::member(regeo,"addressComponent",&component);
        place.name = field(component,"district") + field(component,"township");
        if (place.name.empty()) place.name = place.address;
        place.longitude = lon; place.latitude = lat;
        if (!std::isfinite(lon) || !std::isfinite(lat) || std::abs(lon)>180 || std::abs(lat)>85.051129) {
            result.error = "Invalid location response"; return result;
        }
    }
    result.places.push_back(std::move(place)); result.success = true; return result;
}

AmapSearchResult AmapClient::parse_search_response(std::string_view body)
{
    AmapSearchResult result;
    if (field(body, "status") != "1") {
        const auto code = field(body, "infocode");
        if (code == "10001") result.error = "AMap API key is invalid or expired";
        else if (code == "10003") result.error = "AMap daily request limit reached";
        else if (code == "10004" || code == "10021") result.error = "AMap request rate limit reached";
        else result.error = code.empty() ? "Invalid search response" : "AMap search rejected: " + code;
        return result;
    }
    std::string_view places;
    if (!weread::json::member(body, "pois", &places) || places.empty() || places.front() != '[') {
        result.error = "Invalid search response"; return result;
    }
    weread::json::ObjectIterator iterator(places);
    std::string_view poi;
    for (int i = 0; i < 10 && iterator.next(&poi); ++i) {
        AmapPlace place; place.name = field(poi, "name");
        place.address = field(poi, "pname") + field(poi, "cityname") + field(poi, "adname") + field(poi, "address");
        const auto location = field(poi, "location");
        char *end = nullptr; place.longitude = std::strtod(location.c_str(), &end);
        if (!end || end == location.c_str() || *end != ',') continue;
        const char *lat = end+1; place.latitude = std::strtod(lat, &end);
        if (end == lat || *end || !std::isfinite(place.longitude) || !std::isfinite(place.latitude) ||
            std::abs(place.longitude) > 180 || std::abs(place.latitude) > 85.051129 || place.name.empty()) continue;
        result.places.push_back(std::move(place));
    }
    result.success = true;
    return result;
}
