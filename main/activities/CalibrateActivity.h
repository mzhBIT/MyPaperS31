#pragma once

#include "Activity.h"

/// Touch calibration activity — runs once on first boot.
/// Displays crosshair targets, collects raw ADC samples,
/// computes affine matrix, and saves to NVS.
class CalibrateActivity : public Activity {
public:
    void on_create() override;
    void loop() override;
    bool allow_system_back() const override { return false; }
    bool allow_system_home() const override { return false; }
    bool can_standby() const override { return false; }

    /// Check whether calibration is due and launch if needed.
    /// Returns true if calibration was launched (or already done).
    static bool run_if_needed();

private:
    enum State { DRAW, WAIT_TOUCH, DONE };

    static constexpr int NUM_POINTS = 4;
    static constexpr int NUM_SAMPLES = 8;  // samples per point for averaging

    State _state = DRAW;
    int   _point_idx = 0;
    int   _sample_cnt = 0;
    float _acc_rx = 0, _acc_ry = 0;
    bool  _was_touched = false;

    void _draw_crosshair(int sx, int sy, int num);
    void _draw_message(const char *line1, const char *line2);
    void _draw_success(float error_px);
    void _draw_fail();
};

void launch_calibration();
