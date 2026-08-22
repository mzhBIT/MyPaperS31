#pragma once

#include <cstdint>

namespace touch_cal {

/// Single calibration sample: raw ADC → known screen coordinate
struct Point {
    float sx, sy;   // screen coordinate (known target position)
    float rx, ry;   // raw ADC value (from getTouchRaw)
};

/// Result of calibration
struct Result {
    bool  valid;
    float affine[6];   // { a0, a1, a2, a3, a4, a5 }
    float error_px;    // average residual error in pixels
};

/// Load calibration from NVS. Returns false if not calibrated.
bool load(float affine[6]);

/// Save calibration to NVS.
bool save(const float affine[6]);

/// Check whether calibration has been performed.
bool is_calibrated();

/// Erase calibration (force recalibrate on next boot).
void erase();

/// Compute affine matrix from 4+ calibration points using least squares.
/// Point count n must be ≥ 4.
Result compute(const Point *pts, int n);

} // namespace touch_cal
