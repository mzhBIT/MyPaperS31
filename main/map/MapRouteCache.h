#pragma once
#include "map/AmapRoute.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

// Session-only cache. Geometry and instruction bytes are stored in PSRAM,
// not in default-allocator STL containers. No SD writes or background work.
class MapRouteCache {
    struct Free { void operator()(unsigned char *p) const { std::free(p); } };
    struct Slot {
        std::unique_ptr<unsigned char, Free> data;
        size_t bytes=0, points=0, steps=0;
        double distance=0, west=180, south=90, east=-180, north=-90;
        int duration=-1;
    };
    std::array<Slot,3> _slots;
public:
    AmapPlace start, end;
    AmapTravelMode mode=AmapTravelMode::Driving;
    void clear() { for (auto &slot : _slots) slot=Slot{}; }
    bool store(const AmapRoute &route) {
        const int index=int(route.mode);
        if (!route.success || index<0 || index>=3 || route.points.size()>512 || route.steps.size()>128) return false;
        size_t bytes=route.points.size()*2*sizeof(double);
        for (const auto &step : route.steps) {
            if (step.size()>2048) return false;
            bytes+=sizeof(uint32_t)+step.size();
        }
#ifdef ESP_PLATFORM
        auto *raw=static_cast<unsigned char *>(heap_caps_malloc(bytes?bytes:1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
#else
        auto *raw=static_cast<unsigned char *>(std::malloc(bytes?bytes:1));
#endif
        if (!raw) return false; // Retain the previous successful result.
        Slot next;
        next.data.reset(raw); next.bytes=bytes;
        next.points=route.points.size(); next.steps=route.steps.size();
        next.distance=route.distance; next.duration=route.duration;
        next.west=route.west; next.south=route.south; next.east=route.east; next.north=route.north;
        auto *p=raw;
        for (const auto &point : route.points) {
            std::memcpy(p,&point.first,sizeof(double)); p+=sizeof(double);
            std::memcpy(p,&point.second,sizeof(double)); p+=sizeof(double);
        }
        for (const auto &step : route.steps) {
            const uint32_t length=uint32_t(step.size());
            std::memcpy(p,&length,sizeof(length)); p+=sizeof(length);
            std::memcpy(p,step.data(),length); p+=length;
        }
        _slots[index]=std::move(next);
        return true;
    }
    bool load(AmapTravelMode selected,AmapRoute &route) const {
        const int index=int(selected);
        if (index<0 || index>=3 || !_slots[index].data) return false;
        const auto &slot=_slots[index];
        AmapRoute result;
        result.success=true; result.mode=selected; result.origin=start; result.destination=end;
        result.distance=slot.distance; result.duration=slot.duration;
        result.west=slot.west; result.south=slot.south; result.east=slot.east; result.north=slot.north;
        result.points.reserve(slot.points); result.steps.reserve(slot.steps);
        const auto *p=slot.data.get();
        for (size_t i=0;i<slot.points;++i) {
            double x,y;
            std::memcpy(&x,p,sizeof(x)); p+=sizeof(x);
            std::memcpy(&y,p,sizeof(y)); p+=sizeof(y);
            result.points.emplace_back(x,y);
        }
        for (size_t i=0;i<slot.steps;++i) {
            uint32_t length;
            std::memcpy(&length,p,sizeof(length)); p+=sizeof(length);
            result.steps.emplace_back(reinterpret_cast<const char *>(p),length); p+=length;
        }
        route=std::move(result);
        return true;
    }
    size_t bytes(AmapTravelMode selected) const {
        const int index=int(selected);
        return index>=0 && index<3 ? _slots[index].bytes : 0;
    }
};
