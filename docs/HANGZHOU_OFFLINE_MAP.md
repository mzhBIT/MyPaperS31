# 杭州市离线地图集

输出目录：`E:/opensource/epdiy-main/MyPaperP4/tmp/map/hangzhou-region-packs`。

按本地 OSM 杭州市行政边界（osm_id=3221112，admin_level5）筛选现有地图包，
包括市行政区，不仅限于主城区。没有重新解码、裁剪或修改地图包像素。

共 29 个包：28 个与杭州市真实边界相交的详细区域包 + 1 个原浙江概览包。
地图包合计 7,061,145,920 字节（6.58 GiB），已全部完成复制和 CRC 校验。
详细包保留自身市界外覆盖与边缘重叠；低缩放概览仍包含浙江全省。

## 使用

1. SD 卡预留至少约 7.1 GB 可用空间，另留应用缓存余量。
2. 将全部 `.mpm` 文件和 `catalog.mpc` 复制到 SD 卡同一目录，例如 `maps/hangzhou`。
3. 设置 → 应用配置 → 地图 → 离线地图源 → 地图包，选择 `/sdcard/maps/hangzhou/catalog.mpc`。
4. 使用此前支持 MPM v2 / catalog 的固件即可，不需要新固件改动。

`manifest.json` 是可选的 PC 清单，不必复制；原浙江地图集保持不变。
目录初始中心已设为杭州市行政范围中心，而非浙江省概览中心。
原生 C++ 验证通过：概览/详细切换、实际分块边界连续性、淳安与建德的 Z16 地图读取。
保留 OpenStreetMap contributors 来源标识，分发时遵守源数据许可。
