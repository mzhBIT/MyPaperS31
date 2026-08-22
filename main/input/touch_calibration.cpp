#include "touch_calibration.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <cmath>
#include <esp_log.h>

namespace touch_cal {

static const char *TAG = "CAL";
static const char *NVS_NS  = "touch_cal";
static const char *NVS_KEY = "affine";

// ─── NVS helpers ─────────────────────────────────────────────────────

bool load(float affine[6]) {
    nvs_handle_t h;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READONLY, &h)) {
        ESP_LOGW(TAG, "Calibration not found — first boot");
        return false;
    }
    size_t len = sizeof(float) * 6;
    if (ESP_OK != nvs_get_blob(h, NVS_KEY, affine, &len)) {
        nvs_close(h);
        return false;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded: [%.3f %.3f %.1f  %.3f %.3f %.1f]",
             affine[0], affine[1], affine[2], affine[3], affine[4], affine[5]);
    return true;
}

bool save(const float affine[6]) {
    nvs_handle_t h;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READWRITE, &h)) {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return false;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, affine, sizeof(float) * 6);
    nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Calibration saved to NVS");
        return true;
    }
    return false;
}

bool is_calibrated() {
    float dummy[6];
    return load(dummy);
}

void erase() {
    nvs_handle_t h;
    if (ESP_OK == nvs_open(NVS_NS, NVS_READWRITE, &h)) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ─── Least-squares affine solver ──────────────────────────────────────
//
// We solve two independent 3×3 linear systems:
//
//   screen_x = a0*rx + a1*ry + a2
//   screen_y = a3*rx + a4*ry + a5
//
// For each point i:  [rxi  ryi  1] · [a0 a1 a2]ᵀ = sxi
// Minimal 4 points → overdetermined → least squares.

static bool solve3x3(const float A[][3], const float b[], float x[3], int n) {
    // Build normal equations:  AᵀA · x = Aᵀb
    float ATA[3][3] = {};
    float ATb[3]    = {};

    for (int i = 0; i < n; i++) {
        float r0 = A[i][0], r1 = A[i][1], r2 = A[i][2];
        ATA[0][0] += r0 * r0;  ATA[0][1] += r0 * r1;  ATA[0][2] += r0 * r2;
        ATA[1][0] += r1 * r0;  ATA[1][1] += r1 * r1;  ATA[1][2] += r1 * r2;
        ATA[2][0] += r2 * r0;  ATA[2][1] += r2 * r1;  ATA[2][2] += r2 * r2;
        ATb[0] += r0 * b[i];
        ATb[1] += r1 * b[i];
        ATb[2] += r2 * b[i];
    }

    // Gaussian elimination with partial pivoting (3×3)
    for (int col = 0; col < 3; col++) {
        // Pivot
        int best = col;
        for (int row = col + 1; row < 3; row++) {
            if (fabsf(ATA[row][col]) > fabsf(ATA[best][col])) best = row;
        }
        if (best != col) {
            for (int k = 0; k < 3; k++) std::swap(ATA[col][k], ATA[best][k]);
            std::swap(ATb[col], ATb[best]);
        }
        float pivot = ATA[col][col];
        if (fabsf(pivot) < 1e-12f) return false;   // singular
        // Eliminate rows below
        for (int row = col + 1; row < 3; row++) {
            float factor = ATA[row][col] / pivot;
            for (int k = col; k < 3; k++) ATA[row][k] -= factor * ATA[col][k];
            ATb[row] -= factor * ATb[col];
        }
    }
    // Back substitution
    for (int i = 2; i >= 0; i--) {
        float sum = ATb[i];
        for (int j = i + 1; j < 3; j++) sum -= ATA[i][j] * x[j];
        x[i] = sum / ATA[i][i];
    }
    return true;
}

Result compute(const Point *pts, int n) {
    Result result = {};
    if (n < 4) { result.valid = false; return result; }

    float AX[4][3];  float bx[4];
    float AY[4][3];  float by[4];
    for (int i = 0; i < n; i++) {
        AX[i][0] = pts[i].rx;  AX[i][1] = pts[i].ry;  AX[i][2] = 1.0f;
        bx[i]    = pts[i].sx;
        AY[i][0] = pts[i].rx;  AY[i][1] = pts[i].ry;  AY[i][2] = 1.0f;
        by[i]    = pts[i].sy;
    }

    float coeff_x[3] = {};
    float coeff_y[3] = {};
    if (!solve3x3(AX, bx, coeff_x, n) || !solve3x3(AY, by, coeff_y, n)) {
        result.valid = false;
        return result;
    }

    result.affine[0] = coeff_x[0];
    result.affine[1] = coeff_x[1];
    result.affine[2] = coeff_x[2];
    result.affine[3] = coeff_y[0];
    result.affine[4] = coeff_y[1];
    result.affine[5] = coeff_y[2];
    result.valid = true;

    // Residual error
    float total = 0;
    for (int i = 0; i < n; i++) {
        float px = result.affine[0] * pts[i].rx + result.affine[1] * pts[i].ry + result.affine[2];
        float py = result.affine[3] * pts[i].rx + result.affine[4] * pts[i].ry + result.affine[5];
        float dx = px - pts[i].sx;
        float dy = py - pts[i].sy;
        total += sqrtf(dx * dx + dy * dy);
    }
    result.error_px = total / n;

    ESP_LOGI(TAG, "Affine: [%.3f %.3f %.1f  %.3f %.3f %.1f]  err=%.1fpx",
             result.affine[0], result.affine[1], result.affine[2],
             result.affine[3], result.affine[4], result.affine[5],
             result.error_px);

    return result;
}

} // namespace touch_cal
