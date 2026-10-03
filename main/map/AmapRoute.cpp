#include "map/AmapRoute.h"
#include "weread/WereadJson.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>

namespace {
std::string text(std::string_view object, const char *key)
{
    std::string_view raw; std::string out;
    if (weread::json::member(object,key,&raw)) weread::json::string(raw,&out);
    return out;
}
bool number(std::string_view object, const char *key, double &value)
{
    std::string_view raw;
    if (!weread::json::member(object,key,&raw)) return false;
    std::string s;
    if (!weread::json::string(raw,&s)) s=std::string(raw);
    char *end=nullptr; value=std::strtod(s.c_str(),&end);
    return end!=s.c_str() && !*end && std::isfinite(value) && value>=0;
}
bool point(std::string_view value, double &lon, double &lat)
{
    if (value.size()>64) return false;
    const std::string s(value); char *end=nullptr;
    lon=std::strtod(s.c_str(),&end);
    if (end==s.c_str() || *end!=',') return false;
    const char *start=end+1; lat=std::strtod(start,&end);
    return end!=start && !*end && std::isfinite(lon) && std::isfinite(lat) &&
           std::abs(lon)<=180 && std::abs(lat)<=85.051129;
}
}
AmapRoute parse_amap_route(std::string_view body)
{
    AmapRoute result;
    if (text(body,"status")!="1") {
        result.error=AmapClient::parse_search_response(body).error;
        return result;
    }
    std::string_view route, paths, path, steps;
    if (!weread::json::member(body,"route",&route) || !weread::json::member(route,"paths",&paths)) {
        result.error="Invalid route response"; return result;
    }
    weread::json::ObjectIterator plans(paths);
    if (!plans.next(&path)) { result.error="No route found"; return result; }
    if (!number(path,"distance",result.distance)) { result.error="Invalid route response"; return result; }
    std::string_view cost;
    double duration;
    if ((weread::json::member(path,"cost",&cost) && number(cost,"duration",duration)) || number(path,"duration",duration)) {
        if (duration<=31536000) result.duration=static_cast<int>(duration);
    }
    if (!weread::json::member(path,"steps",&steps)) { result.error="No route found"; return result; }
    weread::json::ObjectIterator segments(steps); std::string_view step;
    int count=0;
    while (segments.next(&step)) {
        if (++count>128) { result.error="Route is too complex"; return result; }
        auto instruction=text(step,"instruction");
        if (instruction.empty()) instruction=text(step,"road_name");
        if (!instruction.empty()) {
            if (instruction.size()>2048) { result.error="Route is too complex"; return result; }
            result.steps.push_back(std::move(instruction));
        }
        std::string_view line;
        if (!weread::json::member(step,"polyline",&line)) continue;
        if (line.size()<2 || line.front()!='"' || line.back()!='"') { result.error="Invalid route response"; return result; }
        line.remove_prefix(1); line.remove_suffix(1);
        while (!line.empty()) {
            const auto end=line.find(';'); const auto part=line.substr(0,end);
            double lon,lat;
            if (!point(part,lon,lat)) { result.error="Invalid route response"; return result; }
            result.west=std::min(result.west,lon); result.east=std::max(result.east,lon);
            result.south=std::min(result.south,lat); result.north=std::max(result.north,lat);
            if (result.points.empty() || result.points.back()!=std::make_pair(lon,lat)) {
                if (result.points.size()==512) {
                    // Bounded display geometry: coarsen in place, preserve first point.
                    size_t n=1; for (size_t i=2;i<result.points.size();i+=2) result.points[n++]=result.points[i];
                    result.points.resize(n);
                }
                result.points.emplace_back(lon,lat);
            }
            if (end==std::string_view::npos) break;
            line.remove_prefix(end+1);
        }
    }
    if (!count || (result.points.empty() && result.steps.empty())) { result.error="No route found"; return result; }
    result.success=true; return result;
}
