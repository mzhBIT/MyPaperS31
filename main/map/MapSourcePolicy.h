#pragma once
#include "map/AmapConfig.h"
#include "map/MapCoordinates.h"

namespace map_source_policy {
// Config coordinates follow its effective source; route geometry stays GCJ-02.
inline bool apply_zoom(AmapConfig &config, uint8_t zoom)
{
    const bool offline=config.hybrid ? zoom<=16 : config.offline;
    const bool changed=offline!=config.offline;
    if (changed) {
        const auto p=offline ? map_coordinates::gcj02_to_wgs84(config.longitude,config.latitude)
                             : map_coordinates::wgs84_to_gcj02(config.longitude,config.latitude);
        config.longitude=p.first; config.latitude=p.second;
    }
    config.offline=offline; config.zoom=zoom;
    return changed;
}
}
