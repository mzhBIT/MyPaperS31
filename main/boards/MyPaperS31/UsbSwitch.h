#pragma once

namespace mypaper::board::s31 {

// ANA_SW (GPIO36) selects the USB-C data path on the reference board.
// usb_device=true drives the switch low, routing the connector to the
// ESP32-S31 HS UTMI PHY for USB Host audio or TinyUSB MSC. false restores the normal path
// (USB Serial/JTAG and the rest of the product).
void usb_analog_switch_set(bool usb_device);

} // namespace mypaper::board::s31
