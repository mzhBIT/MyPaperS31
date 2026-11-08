# Offline flasher dependencies

- **esptool-js 0.7.1**, Espressif Systems, Apache-2.0.
  Official source: https://github.com/espressif/esptool-js
  Distribution: https://registry.npmjs.org/esptool-js/-/esptool-js-0.7.1.tgz
  Vendored bundle SHA-256: `d6a4b0b7c9b747a7370cfa976164a962f56332d5faa7c3b45cdb95a2a8b1d456`.
  The official bundle includes chip targets, stub images and dependencies (including pako, MIT/Zlib). Its license comments are retained.
  Packaging changes only the final ESM export into `globalThis.MyPaperEspTool` and wraps it in an IIFE for offline `file://` use; protocol/target/stub code is unchanged.
- **js-md5 0.8.3**, Chen, Yi-Cyuan, MIT.
  Official source: https://github.com/emn178/js-md5
  Distribution: https://registry.npmjs.org/js-md5/-/js-md5-0.8.3.tgz
  Used for partition-table and Flash MD5 checks; Web Crypto provides package SHA-256.
- **pako 2.1.0**, MIT/Zlib, https://github.com/nodeca/pako (included by the official esptool-js bundle).
- **atob-lite 2.0.0**, MIT, https://github.com/hughsk/atob-lite (included by the official esptool-js bundle).

These notices cover the browser flasher dependencies. Firmware components, emulator cores, fonts and assets retain their respective original licenses; browser-tool licenses do not relicense the firmware.
