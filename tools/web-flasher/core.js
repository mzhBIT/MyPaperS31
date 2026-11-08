/* MyPaperS31 offline flasher policy. Shared by browser and Node tests. */
(function (root, factory) {
  const core = factory();
  if (typeof module === 'object' && module.exports) module.exports = core;
  else root.MyPaperFlasherCore = core;
})(globalThis, function () {
  'use strict';
  const FLASH_BYTES = 0x1000000;
  const CONTRACT = Object.freeze({
    nvs: [1, 2, 0x9000, 0x6000], phy_init: [1, 1, 0xf000, 0x1000],
    factory: [0, 0, 0x10000, 0x800000], emulator: [0, 16, 0x810000, 0x400000],
    otadata: [1, 0, 0xc10000, 0x2000],
  });
  const SLOTS = Object.freeze({ bootloader: [0x2000, 0x6000], partitions: [0x8000, 0x1000],
    system: [0x10000, 0x800000], emulator: [0x810000, 0x400000], otadata: [0xc10000, 0x2000] });
  function assert(condition, message) { if (!condition) throw new Error(message); }
  function hex(value) { return '0x' + value.toString(16).padStart(6, '0').toUpperCase(); }
  function parsePartitions(bytes, md5) {
    assert(bytes instanceof Uint8Array && bytes.length >= 0xc00, '分区表读取不完整。');
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const result = {};
    let digestFound = false;
    for (let pos = 0; pos + 32 <= 0xc00; pos += 32) {
      const magic = view.getUint16(pos, true);
      if (magic === 0xebeb) {
        const actual = Array.from(bytes.subarray(pos + 16, pos + 32), b => b.toString(16).padStart(2, '0')).join('');
        assert(md5(bytes.subarray(0, pos)) === actual, '设备分区表 MD5 校验失败。');
        digestFound = true; break;
      }
      if (magic === 0xffff) break;
      assert(magic === 0x50aa, '设备尚未使用 MyPaperS31 分区表。');
      const name = new TextDecoder().decode(bytes.subarray(pos + 12, pos + 28)).split('\0')[0];
      assert(name && !result[name], '分区名称为空或重复。');
      result[name] = [bytes[pos + 2], bytes[pos + 3], view.getUint32(pos + 4, true), view.getUint32(pos + 8, true)];
      assert(view.getUint32(pos + 28, true) === 0, '不支持带加密标志的分区。');
    }
    assert(digestFound, '分区表缺少 MD5，不能确认当前布局。');
    return result;
  }
  function layoutMatches(table) {
    return Object.keys(table).length === Object.keys(CONTRACT).length &&
      Object.entries(CONTRACT).every(([name, spec]) => table[name]?.every((v, i) => v === spec[i]) && table[name].length === 4);
  }
  function validateManifest(manifest) {
    assert(manifest?.schema === 1 && manifest.board === 'MyPaperS31', '固件包格式或设备型号错误。');
    assert(manifest.chipId === 32 && manifest.flashBytes === FLASH_BYTES, '固件包必须用于 ESP32-S31 / 16 MB Flash。');
    const ids = new Set();
    for (const file of manifest.files) {
      const slot = SLOTS[file.id];
      assert(slot && !ids.has(file.id), '固件文件类型未知或重复。'); ids.add(file.id);
      assert(file.address === slot[0] && Number.isInteger(file.size) && file.size > 0 && file.size <= slot[1], '固件烧录地址或分区大小错误：' + file.id);
      assert(file.address + Math.ceil(file.size / 4096) * 4096 <= slot[0] + slot[1], '固件擦除范围超出分区。');
      assert(/^[a-f0-9]{64}$/.test(file.sha256) && /^[a-f0-9]{32}$/.test(file.md5), '固件校验值格式错误。');
    }
    for (const id of Object.keys(SLOTS)) assert(ids.has(id), '固件包缺少文件：' + id);
  }
  function validateImage(file, bytes) {
    assert(bytes.length === file.size, '固件长度不符：' + file.id);
    if (!['bootloader', 'system', 'emulator'].includes(file.id)) return;
    assert(bytes[0] === 0xe9 && new DataView(bytes.buffer, bytes.byteOffset).getUint16(12, true) === 32, '镜像芯片不是 ESP32-S31：' + file.id);
    if (file.id !== 'bootloader') {
      const view = new DataView(bytes.buffer, bytes.byteOffset);
      assert(view.getUint32(32, true) === 0xabcd5432, '镜像缺少应用描述。');
      const project = new TextDecoder().decode(bytes.subarray(80, 112)).split('\0')[0];
      assert(project === (file.id === 'system' ? 'MyPaperS31' : 'MyPaperEmulatorS31'), '镜像应用身份不符：' + project);
    }
  }
  function validateDevice(chipName, security, flashSize) {
    assert(chipName === 'ESP32-S31' && security?.chipId === 32, '仅支持 ESP32-S31，已停止操作。');
    const flags = security.parsedFlags || {};
    assert(!flags.SECURE_BOOT_EN && !flags.SECURE_DOWNLOAD_ENABLE && security.flashCryptCnt === 0,
      '设备启用了安全启动、加密或受限下载模式，本公开固件包不适用。');
    if (flashSize !== undefined) assert(flashSize === '16MB', '设备 Flash 必须为 16 MB，当前为：' + flashSize);
  }
  function makePlan(manifest, mode, includeEmulator, tableMatches) {
    validateManifest(manifest);
    let ids;
    if (mode === 'install') ids = ['bootloader', 'partitions', 'system', ...(includeEmulator ? ['emulator'] : []), 'otadata'];
    else {
      assert(tableMatches, '设备分区不同，请先选择“首次安装 / 修复安装”。');
      assert(mode === 'update' || mode === 'emulator', '未知烧录模式。');
      ids = mode === 'update' ? ['system'] : ['emulator'];
    }
    return ids.map(id => manifest.files.find(file => file.id === id));
  }
  return { FLASH_BYTES, CONTRACT, SLOTS, hex, parsePartitions, layoutMatches, validateManifest, validateImage, validateDevice, makePlan };
});
