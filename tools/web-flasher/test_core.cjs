'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const vm = require('node:vm');
const core = require('./core.js');
const root = path.resolve(__dirname, '../..');
const pkg = path.join(root, 'firmware-release/MyPaperS31');
const manifest = JSON.parse(fs.readFileSync(path.join(pkg, 'manifest.json')));
const md5 = bytes => crypto.createHash('md5').update(bytes).digest('hex');
const table = new Uint8Array(fs.readFileSync(path.join(pkg, 'firmware/partition-table.bin')));
test('the real packaged partition table has valid MD5 and the exact dual-firmware layout', () => {
  assert.equal(core.layoutMatches(core.parsePartitions(table, md5)), true);
  const altered = table.slice(); altered[7] ^= 1;
  assert.throws(() => core.parsePartitions(altered, md5), /MD5/);
  assert.throws(() => core.parsePartitions(new Uint8Array(4096).fill(255), md5));
  const changed = structuredClone(core.CONTRACT); changed.factory[3] -= 4096;
  assert.equal(core.layoutMatches(changed), false);
});
test('real firmware hashes, chip identities and application identities match', () => {
  core.validateManifest(manifest);
  for (const file of manifest.files) {
    const bytes = new Uint8Array(fs.readFileSync(path.join(pkg, file.path)));
    assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'), file.sha256);
    assert.equal(md5(bytes), file.md5);
    core.validateImage(file, bytes);
    if (file.id === 'system' || file.id === 'emulator') {
      const wrong = bytes.slice(); wrong[80] ^= 1;
      assert.throws(() => core.validateImage(file, wrong), /身份/);
    }
  }
});
test('partial updates never write bootloader, NVS, emulator or OTA selection inadvertently', () => {
  assert.deepEqual(core.makePlan(manifest, 'update', true, true).map(x => x.id), ['system']);
  assert.deepEqual(core.makePlan(manifest, 'emulator', true, true).map(x => x.id), ['emulator']);
  assert.throws(() => core.makePlan(manifest, 'update', true, false), /分区/);
  assert.throws(() => core.makePlan(manifest, 'emulator', true, false), /分区/);
  assert.deepEqual(core.makePlan(manifest, 'install', true, false).map(x => x.id), ['bootloader', 'partitions', 'system', 'emulator', 'otadata']);
  assert.deepEqual(core.makePlan(manifest, 'install', false, false).map(x => x.id), ['bootloader', 'partitions', 'system', 'otadata']);
  for (const file of core.makePlan(manifest, 'install', true, false)) {
    const end = file.address + Math.ceil(file.size / 4096) * 4096;
    assert.ok(end <= 0x9000 || file.address >= 0x10000);
  }
});
test('wrong chips, Flash sizes, secure devices and bad write addresses are blocked', () => {
  const security = { chipId: 32, flashCryptCnt: 0, parsedFlags: {} };
  core.validateDevice('ESP32-S31', security, '16MB');
  assert.throws(() => core.validateDevice('ESP32-P4', security, '16MB'));
  assert.throws(() => core.validateDevice('ESP32-S31', security, '8MB'));
  assert.throws(() => core.validateDevice('ESP32-S31', { ...security, flashCryptCnt: 1 }, '16MB'));
  assert.throws(() => core.validateDevice('ESP32-S31', { ...security, parsedFlags: { SECURE_BOOT_EN: true } }, '16MB'));
  const changed = structuredClone(manifest); changed.files.find(x => x.id === 'system').address = 0x810000;
  assert.throws(() => core.makePlan(changed, 'install', true, false), /地址/);
});
test('offline HTML has an actual S31 stub and no runtime imports or downloads', async () => {
  const html = fs.readFileSync(path.join(pkg, '刷机工具.html'), 'utf8');
  assert.doesNotMatch(html, /<script[^>]+\bsrc=/i);
  const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(x => x[1]);
  const context = { console, atob, btoa, TextEncoder, TextDecoder, setTimeout, clearTimeout };
  vm.createContext(context); vm.runInContext(scripts[0], context);
  const stub = await context.MyPaperEspTool.getStubJsonByChipName('ESP32-S31', 0);
  assert.ok(stub.decodedText.length > 1000);
  assert.ok(stub.entry >= 0x2f000000 && stub.entry < 0x2f080000);
  assert.doesNotMatch(scripts[0], /\b(?:fetch\s*\(|import\s*\()/);
});
