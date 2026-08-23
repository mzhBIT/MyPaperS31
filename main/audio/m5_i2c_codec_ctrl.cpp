#include "audio/m5_i2c_codec_ctrl.h"

#include <M5Unified.h>

#include <cstdlib>

#include "esp_log.h"

namespace {

constexpr const char *TAG = "M5_I2C_CODEC";

struct M5I2cCodecCtrl {
    audio_codec_ctrl_if_t base;
    bool is_open;
    uint8_t port;
    uint8_t address_7bit;
    uint16_t codec_address;
    uint32_t frequency_hz;
};

M5I2cCodecCtrl *as_ctrl(const audio_codec_ctrl_if_t *ctrl)
{
    return reinterpret_cast<M5I2cCodecCtrl *>(const_cast<audio_codec_ctrl_if_t *>(ctrl));
}

int ctrl_open(const audio_codec_ctrl_if_t *ctrl, void *, int)
{
    if (ctrl == nullptr) return ESP_CODEC_DEV_INVALID_ARG;
    auto *self = as_ctrl(ctrl);
    if (!M5.In_I2C.isEnabled() || M5.In_I2C.getPort() != self->port) {
        ESP_LOGE(TAG, "M5 internal I2C is unavailable: requested=%u actual=%d enabled=%d",
                 self->port, static_cast<int>(M5.In_I2C.getPort()), M5.In_I2C.isEnabled());
        return ESP_CODEC_DEV_DRV_ERR;
    }
    self->is_open = true;
    return ESP_CODEC_DEV_OK;
}

bool ctrl_is_open(const audio_codec_ctrl_if_t *ctrl)
{
    return ctrl != nullptr && as_ctrl(ctrl)->is_open;
}

int ctrl_read_reg(const audio_codec_ctrl_if_t *ctrl,
                  int reg, int reg_len, void *data, int data_len)
{
    if (ctrl == nullptr || data == nullptr || reg_len != 1 || data_len <= 0 ||
        reg < 0 || reg > UINT8_MAX) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    auto *self = as_ctrl(ctrl);
    if (!self->is_open) return ESP_CODEC_DEV_WRONG_STATE;

    bool ok = M5.In_I2C.readRegister(self->address_7bit,
                                     static_cast<uint8_t>(reg),
                                     static_cast<uint8_t *>(data),
                                     static_cast<size_t>(data_len),
                                     self->frequency_hz);
    if (!ok) {
        ESP_LOGE(TAG, "Read failed: addr=0x%02x reg=0x%02x len=%d",
                 self->address_7bit, reg, data_len);
        return ESP_CODEC_DEV_READ_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

int ctrl_write_reg(const audio_codec_ctrl_if_t *ctrl,
                   int reg, int reg_len, void *data, int data_len)
{
    if (ctrl == nullptr || data == nullptr || reg_len != 1 || data_len <= 0 ||
        reg < 0 || reg > UINT8_MAX) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    auto *self = as_ctrl(ctrl);
    if (!self->is_open) return ESP_CODEC_DEV_WRONG_STATE;

    bool ok = M5.In_I2C.writeRegister(self->address_7bit,
                                      static_cast<uint8_t>(reg),
                                      static_cast<const uint8_t *>(data),
                                      static_cast<size_t>(data_len),
                                      self->frequency_hz);
    if (!ok) {
        ESP_LOGE(TAG, "Write failed: addr=0x%02x reg=0x%02x len=%d",
                 self->address_7bit, reg, data_len);
        return ESP_CODEC_DEV_WRITE_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

int ctrl_get_info(const audio_codec_ctrl_if_t *ctrl, audio_codec_ctrl_info_t *info)
{
    if (ctrl == nullptr || info == nullptr) return ESP_CODEC_DEV_INVALID_ARG;
    auto *self = as_ctrl(ctrl);
    info->type = AUDIO_CODEC_CTRL_I2C;
    // Preserve esp_codec_dev's address convention for physical-device
    // identity while M5Unified receives the standard 7-bit address.
    info->i2c.addr = self->codec_address;
    info->i2c.port = self->port;
    return ESP_CODEC_DEV_OK;
}

int ctrl_close(const audio_codec_ctrl_if_t *ctrl)
{
    if (ctrl == nullptr) return ESP_CODEC_DEV_INVALID_ARG;
    as_ctrl(ctrl)->is_open = false;
    return ESP_CODEC_DEV_OK;
}

}  // namespace

extern "C" const audio_codec_ctrl_if_t *audio_codec_new_m5_i2c_ctrl(
    uint8_t port, uint8_t address_7bit, uint32_t frequency_hz)
{
    if (address_7bit < 0x08 || address_7bit > 0x77 || frequency_hz == 0) {
        ESP_LOGE(TAG, "Invalid config: port=%u addr=0x%02x freq=%lu",
                 port, address_7bit, static_cast<unsigned long>(frequency_hz));
        return nullptr;
    }

    auto *ctrl = static_cast<M5I2cCodecCtrl *>(std::calloc(1, sizeof(M5I2cCodecCtrl)));
    if (ctrl == nullptr) return nullptr;

    ctrl->base.open = ctrl_open;
    ctrl->base.is_open = ctrl_is_open;
    ctrl->base.read_reg = ctrl_read_reg;
    ctrl->base.write_reg = ctrl_write_reg;
    ctrl->base.get_info = ctrl_get_info;
    ctrl->base.close = ctrl_close;
    ctrl->port = port;
    ctrl->address_7bit = address_7bit;
    ctrl->codec_address = static_cast<uint16_t>(address_7bit) << 1;
    ctrl->frequency_hz = frequency_hz;

    if (ctrl_open(&ctrl->base, nullptr, 0) != ESP_CODEC_DEV_OK) {
        std::free(ctrl);
        return nullptr;
    }
    ESP_LOGI(TAG, "Using M5.In_I2C port=%u addr=0x%02x freq=%lu",
             port, address_7bit, static_cast<unsigned long>(frequency_hz));
    return &ctrl->base;
}
