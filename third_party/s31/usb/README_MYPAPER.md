# MyPaperS31 integration

Unmodified Espressif `espressif/usb` 1.6.0 sources (Apache-2.0).
Vendored because the S31 build disables Component Manager and ESP-IDF 6.1
does not contain the former SDK USB Host component.

Registry: https://components.espressif.com/components/espressif/usb/versions/1.6.0
Upstream commit: 630f1c4e61f71d62bb57ff1b840931da24c3329f (host/usb).
Archive SHA256: 8c79125c1eb7ea94190470922d47f5b741c20223947ec782d9e429e8a750fc41.

Only MyPaperS31 includes this component. MyPaperP4 retains `components/usb`.
Board routing, VBUS ownership and verification are documented in
`docs/S31_USB_AUDIO.md` at the repository root.
