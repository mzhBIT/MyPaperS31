(async function () {
  'use strict';
  const core = globalThis.MyPaperFlasherCore;
  const { ESPLoader, Transport } = globalThis.MyPaperEspTool;
  const $ = id => document.getElementById(id);
  const bundle = JSON.parse($('firmware-package').textContent);
  const manifest = bundle.manifest;
  const images = new Map();
  const labels = { bootloader: '引导程序', partitions: '分区表', system: '主系统', emulator: '游戏模拟器', otadata: '主系统启动选择' };
  let ready = false, busy = false, connected = false, layoutOK = false, loader = null, transport = null;
  const supported = Boolean(navigator.serial && globalThis.isSecureContext);
  const logLines = [];
  function log(value) {
    const text = String(value).replace(/\x1b\[[0-9;]*[A-Za-z]/g, '');
    const line = `[${new Date().toLocaleTimeString()}] ${text}`;
    logLines.push(line);
    if (logLines.length > 2000) logLines.shift();
    $('log').textContent = logLines.join('\n');
    $('log').scrollTop = $('log').scrollHeight;
  }
  function status(title, detail, kind = '', percent) {
    $('status').textContent = title; $('detail').textContent = detail;
    $('status-dot').className = 'status-dot ' + kind;
    if (percent !== undefined) { $('progress').value = percent; $('percent').textContent = Math.floor(percent) + '%'; }
  }
  function mode() { return document.querySelector('input[name="mode"]:checked').value; }
  function includeEmulator() { return $('include-emulator').checked; }
  function currentPlan(preview = false) { return core.makePlan(manifest, mode(), includeEmulator(), preview || layoutOK); }
  function refresh() {
    const install = mode() === 'install';
    $('emulator-option').hidden = !install;
    $('connect').disabled = !supported || !ready || busy || connected;
    $('disconnect').disabled = busy || !connected;
    $('baud').disabled = busy || connected;
    $('reset').disabled = busy || connected;
    for (const control of document.querySelectorAll('input[name="mode"], #include-emulator')) control.disabled = busy;
    $('flash').disabled = !ready || !connected || busy || (!install && !layoutOK);
    $('plan').replaceChildren();
    if (!ready) return;
    for (const file of currentPlan(true)) {
      const row = document.createElement('tr');
      for (const text of [labels[file.id], core.hex(file.address), (file.size / 1024).toFixed(1) + ' KB']) {
        const cell = document.createElement('td'); cell.textContent = text; row.appendChild(cell);
      }
      $('plan').appendChild(row);
    }
    $('mode-hint').textContent = install
      ? '会更新分区表并恢复从主系统启动；目标范围内的旧固件会被覆盖。NVS 设置区与 SD 卡不写入。'
      : mode() === 'emulator'
        ? '仅写入模拟器。需要先安装匹配的主系统和双固件分区表。'
        : '仅写入主系统，保留模拟器、设置和启动选择；若重启仍进入模拟器，可从其菜单返回主系统。';
  }
  async function release() {
    connected = false; layoutOK = false; loader = null;
    const old = transport; transport = null;
    if (old) { try { await old.disconnect(); } catch (error) { log('串口关闭提示：' + error.message); } }
  }
  function errorDetail(error) {
    return String(error?.message || error) + '。请关闭其他串口工具；必要时按住 BOOT、短按 RESET、松开 BOOT，再选择手动下载模式重新连接。';
  }
  $('connect').addEventListener('click', async () => {
    busy = true; refresh();
    try {
      const port = await navigator.serial.requestPort();
      status('正在连接设备', '正在识别芯片，请不要拔出 USB 线。', 'busy', 0);
      transport = new Transport(port, false);
      loader = new ESPLoader({ transport, baudrate: Number($('baud').value), romBaudrate: 115200,
        terminal: { clean() {}, write: log, writeLine: log }, enableTracing: false });
      await loader.detectChip($('reset').value);
      const security = await loader.getSecurityInfo();
      core.validateDevice(loader.chip?.CHIP_NAME, security);
      $('device').textContent = await loader.chip.getChipDescription(loader);
      if (loader.chip.postConnect) await loader.chip.postConnect(loader);
      await loader.runStub();
      if (!loader.IS_STUB) throw new Error('S31 烧录辅助程序未启动，已停止操作');
      if (loader.romBaudrate !== loader.baudrate) await loader.changeBaud();
      const flashSize = await loader.detectFlashSize();
      if (!flashSize) throw new Error('无法确认设备 Flash 容量，已停止操作');
      core.validateDevice(loader.chip.CHIP_NAME, security, flashSize);
      $('flash-size').textContent = flashSize;
      const table = await loader.readFlash(0x8000, 0x1000);
      try {
        layoutOK = core.layoutMatches(core.parsePartitions(table, globalThis.md5));
      } catch (error) { layoutOK = false; log('分区检查：' + error.message); }
      $('layout').textContent = layoutOK ? 'MyPaperS31 双固件布局 · 匹配' : '尚未安装 / 布局不同';
      connected = true;
      status('设备已连接', layoutOK ? '请选择安装方式。点击“开始烧录”后才会写入。' : '芯片和容量符合要求；请使用“首次安装 / 修复安装”。', 'success', 0);
      log('预检查完成；尚未擦除或写入 Flash。');
    } catch (error) {
      await release();
      if (error.name === 'NotFoundError') status('未选择串口', '可以重新点击“连接设备”。', '', 0);
      else { log('连接失败：' + error.message); status('连接未完成', errorDetail(error), 'error', 0); }
    } finally { busy = false; refresh(); }
  });
  $('disconnect').addEventListener('click', async () => {
    busy = true; refresh(); await release(); busy = false;
    $('device').textContent = '未连接'; $('flash-size').textContent = '—'; $('layout').textContent = '连接后检查';
    status('已断开连接', '设备可能仍在下载模式；松开 BOOT 后按 RESET 可恢复运行。', '', 0); refresh();
  });
  $('flash').addEventListener('click', async () => {
    if (busy || !ready || !connected) return;
    let plan;
    try { plan = currentPlan(); } catch (error) { status('不能开始烧录', error.message, 'error', 0); return; }
    if (mode() === 'install' && !globalThis.confirm('将写入引导程序、分区表和主系统' + (includeEmulator() ? '及模拟器' : '') + '，覆盖目标地址的旧固件，并恢复从主系统启动。\n\n不会整片擦除，也不会操作 SD 卡。确认继续？')) return;
    busy = true; refresh();
    let verified = false;
    try {
      // A layout check is required immediately before every partial update,
      // not merely when the device was first connected.
      if (mode() !== 'install') {
        const table = await loader.readFlash(0x8000, 0x1000);
        if (!core.layoutMatches(core.parsePartitions(table, globalThis.md5))) throw new Error('设备分区表已改变，请重新连接并使用首次安装');
      }
      const total = plan.reduce((sum, file) => sum + file.size, 0);
      const doneBefore = plan.map((_, index) => plan.slice(0, index).reduce((sum, file) => sum + file.size, 0));
      status('正在写入并校验', '请保持 USB 连接，不要关闭网页或让电脑休眠。', 'busy', 0);
      log('写入计划：' + plan.map(file => `${labels[file.id]}@${core.hex(file.address)}`).join('，'));
      await loader.writeFlash({ fileArray: plan.map(file => ({ address: file.address, data: images.get(file.id) })),
        flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep', compress: true, eraseAll: false,
        calculateMD5Hash: data => globalThis.md5(data),
        reportProgress: (index, written, fileTotal) => {
          const ratio = fileTotal > 0 ? Math.min(1, written / fileTotal) : 0;
          const percent = Math.min(99, (doneBefore[index] + plan[index].size * ratio) / total * 100);
          status('正在写入并校验', `${labels[plan[index].id]} · ${core.hex(plan[index].address)}`, 'busy', percent);
        } });
      verified = true;
      log('全部目标文件已写入，Flash MD5 校验通过。');
      status('烧录成功', '写入和校验已完成，正在尝试自动重启。', 'success', 100);
      try {
        await loader.after('hard_reset');
        status('烧录成功', '固件写入及校验完成。若设备未自动启动，松开 BOOT 后按 RESET 或重新上电。', 'success', 100);
      } catch (error) {
        log('写入已成功，自动重启提示：' + error.message);
        status('烧录成功 · 请手动重启', '写入与校验已完成。USB 复位未完成，请松开 BOOT 后按 RESET 或重新上电。', 'success', 100);
      }
    } catch (error) {
      log('烧录失败：' + error.message);
      status('烧录未完成', errorDetail(error) + '重新连接后可再次烧录。', 'error');
    } finally {
      // Never reset a device after a failed write/verification.
      await release(); busy = false; refresh();
      if (verified) $('layout').textContent = '重新连接后复核';
    }
  });
  for (const control of document.querySelectorAll('input[name="mode"], #include-emulator')) control.addEventListener('change', refresh);
  $('save-log').addEventListener('click', () => {
    const data = `MyPaperS31 ${manifest.version}\n${new Date().toISOString()}\n\n${logLines.join('\n')}\n`;
    const url = URL.createObjectURL(new Blob([data], { type: 'text/plain;charset=utf-8' }));
    const link = document.createElement('a'); link.href = url; link.download = 'MyPaperS31-flash-log.txt'; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  });
  globalThis.addEventListener('beforeunload', event => { if (busy) { event.preventDefault(); event.returnValue = ''; } });
  $('unsupported').hidden = supported;
  try {
    core.validateManifest(manifest);
    for (const file of manifest.files) {
      const encoded = bundle.images[file.id];
      if (typeof encoded !== 'string') throw new Error('固件数据缺失：' + file.id);
      const bytes = Uint8Array.from(atob(encoded), c => c.charCodeAt(0));
      core.validateImage(file, bytes);
      const digest = await crypto.subtle.digest('SHA-256', bytes);
      const sha256 = Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
      if (sha256 !== file.sha256 || globalThis.md5(bytes) !== file.md5) throw new Error('固件文件校验失败：' + file.id);
      images.set(file.id, bytes);
    }
    if (!core.layoutMatches(core.parsePartitions(images.get('partitions'), globalThis.md5))) throw new Error('包内分区表与 S31 布局不一致');
    ready = true;
    $('version').textContent = manifest.version;
    $('build-time').textContent = '打包 ' + new Date(manifest.packagedAt).toLocaleString('zh-CN', { hour12: false });
    log('包内固件 SHA-256 / MD5 校验通过。浏览器烧录库和数据均已内置。');
    status('固件包校验通过', supported ? '连接设备以检查芯片和分区，当前尚未操作串口。' : '请使用电脑上的 Chrome 或 Edge 打开本文件。', 'success', 0);
  } catch (error) {
    log('固件包无效：' + error.message); status('固件包校验失败', '请重新获取完整发布包。' + error.message, 'error', 0);
  }
  refresh();
})();
