# S31 vendored USB components

These components are used **only** by the ESP32-S31 build
(`MYPAPER_BOARD=MyPaperS31`, ESP-IDF 6.1). The P4 build keeps its
Component Manager dependencies unchanged.

## Why they are vendored

The S31 needs `esp_tinyusb >= 2.2.0` for `esp32s31` support and
`TINYUSB_CONFIG_HIGH_SPEED` (HS UTMI). The P4 baseline pins
`espressif/esp_tinyusb` 2.0.1~1 in `dependencies.lock`, and the S31 build
runs with `IDF_COMPONENT_MANAGER=0`, so the newer packages are committed
here instead of being resolved from the registry.

The root `CMakeLists.txt` adds both directories to `COMPONENT_DIRS` for the
S31 branch only.

## Contents

| Directory | Package | Version | License |
| --- | --- | --- | --- |
| `esp_tinyusb/` | `espressif/esp_tinyusb` | 2.4.0 | Apache-2.0 (`LICENSE`) |
| `tinyusb/` | `espressif/tinyusb` | 0.21.0~1 | MIT (`LICENSE`) |

Upstream sources:

- esp_tinyusb: <https://github.com/espressif/esp-usb/tree/master/device/esp_tinyusb>
  (commit `7963dbeaddee8798cae953cbcd2f1dc48c7589bf`)
- tinyusb: <https://github.com/espressif/tinyusb>
  (commit `7049c58a0e895acc92c6407574b05b5536eddfc8`)

The directories were extracted from the Component Manager archives with the
paths renamed to `esp_tinyusb` and `tinyusb`; `esp_tinyusb`'s CMake requires a
component literally named `tinyusb` when the Component Manager is disabled.
No source files were modified.

## Updating

1. Download the matching `esp_tinyusb` and `tinyusb` archives from
   <https://components.espressif.com/>.
2. Replace both directories (keep the directory names).
3. Rebuild `build-s31` and re-check `CONFIG_TINYUSB_*` values in
   `build-s31/sdkconfig`.
4. Update the version table above.
