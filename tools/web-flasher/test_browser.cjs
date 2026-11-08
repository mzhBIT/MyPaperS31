/* Browser flow tests use mocked serial/loader objects. They never open USB. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '../..');
const htmlPath = path.join(root, 'firmware-release/MyPaperS31/刷机工具.html');
const table = fs.readFileSync(path.join(root, 'firmware-release/MyPaperS31/firmware/partition-table.bin')).toString('base64');
const shots = path.join(root, 'tmp/flasher-tests/screenshots'); fs.mkdirSync(shots, { recursive: true });
const executablePath = process.env.CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
async function main() {
  const browser = await chromium.launch({ headless: true, executablePath });
  const failures = [], network = [];
  async function open(config = {}, mocked = true, file = htmlPath) {
    const page = await browser.newPage({ viewport: { width: 1280, height: 1120 } });
    page.on('pageerror', error => failures.push(error.message));
    page.on('request', request => { if (/^https?:/.test(request.url())) network.push(request.url()); });
    if (mocked) await page.addInitScript(({ config, table }) => {
      globalThis.__serialTest = { writes: [], resets: 0, reads: 0, requests: 0 };
      const state = globalThis.__serialTest;
      const bytes = Uint8Array.from(atob(table), ch => ch.charCodeAt(0));
      const fakePort = { getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; } };
      Object.defineProperty(navigator, 'serial', { configurable: true, value: {
        async requestPort() { state.requests++; return fakePort; }
      } });
      Object.defineProperty(globalThis, 'MyPaperEspTool', { configurable: true, set(value) {
        class MockTransport { async disconnect() { state.disconnected = true; } }
        class MockLoader {
          constructor(options) {
            this.baudrate = options.baudrate; this.romBaudrate = options.romBaudrate;
            const chipName = config.wrongChip ? 'ESP32-P4' : 'ESP32-S31';
            this.chip = { CHIP_NAME: chipName, async getChipDescription() { return chipName + ' (revision v0.0)'; }, async postConnect() {} };
          }
          async detectChip() {}
          async getSecurityInfo() { return { chipId: config.wrongChip ? 18 : 32, flashCryptCnt: 0, parsedFlags: { SECURE_BOOT_EN: !!config.secure } }; }
          async runStub() { this.IS_STUB = true; }
          async changeBaud() {}
          async detectFlashSize() { return config.flashSize || '16MB'; }
          async readFlash() { state.reads++; return config.blankTable ? new Uint8Array(4096).fill(255) : bytes; }
          async writeFlash(options) {
            state.writes.push({ addresses: options.fileArray.map(x => x.address), eraseAll: options.eraseAll });
            for (let i = 0; i < options.fileArray.length; ++i) {
              const file = options.fileArray[i];
              if (config.writeFailure) throw new Error('injected write failure');
              if (config.verifyFailure) throw new Error('MD5 of file does not match data in flash');
              if (options.calculateMD5Hash(file.data) !== globalThis.md5(file.data)) throw new Error('hash callback broken');
              options.reportProgress(i, file.data.length, file.data.length);
            }
          }
          async after() { state.resets++; if (config.resetFailure) throw new Error('USB disconnected during reset'); }
        }
        Object.defineProperty(globalThis, 'MyPaperEspTool', { configurable: true, writable: true,
          value: { ...value, ESPLoader: MockLoader, Transport: MockTransport } });
      } });
    }, { config, table });
    await page.goto(pathToFileURL(file).href);
    await page.locator('#status').filter({ hasText: /固件包校验(通过|失败)/ }).waitFor();
    return page;
  }
  async function connect(page) { await page.locator('#connect').click(); await page.locator('#status').filter({ hasText: /设备已连接|连接未完成/ }).waitFor(); }
  try {
    let page = await open({}, false);
    assert.equal(await page.evaluate(() => isSecureContext && !!navigator.serial), true);
    await page.screenshot({ path: path.join(shots, 'desktop.png'), fullPage: true });
    assert.equal(await page.locator('#connect').isEnabled(), true);
    assert.equal(await page.locator('#flash').isDisabled(), true);
    await page.setViewportSize({ width: 390, height: 844 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    await page.screenshot({ path: path.join(shots, 'mobile.png'), fullPage: true }); await page.close();

    page = await open(); await connect(page); await page.locator('#flash').click();
    await page.locator('#status').filter({ hasText: '烧录成功' }).waitFor();
    let state = await page.evaluate(() => __serialTest);
    assert.deepEqual(state.writes[0], { addresses: [0x10000], eraseAll: false });
    assert.equal(state.reads, 2); assert.equal(state.resets, 1); await page.close();

    page = await open(); await connect(page); await page.locator('input[value="emulator"]').check(); await page.locator('#flash').click();
    await page.locator('#status').filter({ hasText: '烧录成功' }).waitFor();
    state = await page.evaluate(() => __serialTest); assert.deepEqual(state.writes[0].addresses, [0x810000]); await page.close();

    page = await open({ blankTable: true }); await connect(page); assert.equal(await page.locator('#flash').isDisabled(), true);
    await page.locator('input[value="install"]').check();
    page.once('dialog', dialog => dialog.accept()); await page.locator('#flash').click();
    await page.locator('#status').filter({ hasText: '烧录成功' }).waitFor();
    state = await page.evaluate(() => __serialTest);
    assert.deepEqual(state.writes[0].addresses, [0x2000, 0x8000, 0x10000, 0x810000, 0xc10000]);
    assert.equal(state.writes[0].eraseAll, false); await page.close();

    for (const config of [{ wrongChip: true }, { flashSize: '8MB' }, { secure: true }]) {
      page = await open(config); await connect(page);
      assert.equal(await page.locator('#flash').isDisabled(), true);
      assert.equal((await page.evaluate(() => __serialTest)).writes.length, 0); await page.close();
    }
    for (const config of [{ writeFailure: true }, { verifyFailure: true }]) {
      page = await open(config); await connect(page); await page.locator('#flash').click();
      await page.locator('#status').filter({ hasText: '烧录未完成' }).waitFor();
      assert.equal((await page.evaluate(() => __serialTest)).resets, 0); await page.close();
    }
    page = await open({ resetFailure: true }); await connect(page); await page.locator('#flash').click();
    await page.locator('#status').filter({ hasText: '烧录成功 · 请手动重启' }).waitFor(); await page.close();

    const html = fs.readFileSync(htmlPath, 'utf8');
    const match = html.match(/(<script id="firmware-package" type="application\/json">)([\s\S]*?)(<\/script>)/);
    const payload = JSON.parse(match[2]); const corrupt = Buffer.from(payload.images.system, 'base64'); corrupt[corrupt.length - 16] ^= 1;
    payload.images.system = corrupt.toString('base64');
    const brokenFile = path.join(root, 'tmp/flasher-tests/corrupt.html');
    fs.writeFileSync(brokenFile, html.replace(match[0], match[1] + JSON.stringify(payload) + match[3]));
    page = await open({}, false, brokenFile);
    assert.match(await page.locator('#status').textContent(), /校验失败/);
    assert.equal(await page.locator('#connect').isDisabled(), true); await page.close();
    assert.deepEqual(network, []); assert.deepEqual(failures, []);
    console.log('PASS: file:// offline UI, responsive layout, system/emulator/install plans, incompatible/security blocks, corruption detection, no reset on write/MD5 failure, reset-only warning. No USB port was opened.');
  } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
