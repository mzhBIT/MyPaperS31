'use strict';

const byId = (id) => document.getElementById(id);
const ui = Object.fromEntries(['mode', 'access', 'token', 'open', 'more', 'refresh', 'status', 'feed', 'article', 'title', 'author', 'counts', 'body', 'gallery', 'meta'].map((id) => [id, byId(id)]));
const noteDateFormat = new Intl.DateTimeFormat('zh-CN', { dateStyle: 'medium', timeStyle: 'short' });
const messages = {
  invalid_token: '请填写正确的设备令牌。',
  login_required: '请在运行服务的浏览器里登录或完成验证，再点「打开推荐」。',
  browser_start_failed: '启动浏览器失败，请检查浏览器安装和运行配置。',
  browser_navigation_failed: '群晖或电脑上的浏览器打开小红书失败。',
  feed_unavailable: '暂时没有读到推荐卡片，请检查浏览器里的推荐页。',
  downloader_unavailable: '无法连接 XHS-Downloader，请检查它的地址和容器状态。',
  downloader_timeout: '连接 XHS-Downloader 超时。请在 Windows 浏览器里验证群晖的 /docs 地址。',
  downloader_auth_required: 'XHS-Downloader 拒绝了请求，请检查它的访问令牌。',
  downloader_route_missing: '该地址上找不到 /xhs/detail，请核对群晖的地址和端口。',
  downloader_http_error: 'XHS-Downloader 返回错误，请查看服务终端的 HTTP 状态。',
  downloader_response_invalid: '这个地址没有返回预期的 XHS-Downloader 数据。',
  detail_unavailable: '这篇笔记的详情暂不可用。',
  detail_rate_limited: '网页暂时限制访问，请稍后再试。',
  unsupported_note_type: '当前阅读服务支持图文笔记。',
  session_expired: '推荐会话已更新，请重新打开推荐。',
  note_not_found: '这篇笔记已离开当前推荐列表，请重新选择。',
  job_missing: '这次请求已过期，请重新操作。',
  request_id_conflict: '请求编号已被不同操作使用。',
  queue_full: '服务正在处理其他请求，请稍后再试。',
  timeout: '等待结果超时，请稍后再试。',
  network_error: '连接服务失败。',
};

let token = sessionStorage.getItem('xhs-device-token') || '';
let sessionId;
let ready = false;
let busy = false;
const notes = new Set();

async function request(path, options) {
  const headers = new Headers(options?.headers);
  if (token) headers.set('Authorization', `Bearer ${token}`);
  let response;
  try {
    response = await fetch(path, { ...options, headers });
  } catch {
    throw new Error('network_error');
  }
  if (!response.ok) {
    const error = await response.json().catch(() => ({}));
    throw new Error(typeof error.detail === 'string' ? error.detail : `HTTP ${response.status}`);
  }
  return response;
}

function updateControls() {
  ui.open.disabled = !ready || busy;
  ui.more.disabled = !sessionId || busy;
  ui.refresh.disabled = !sessionId || busy;
  for (const card of ui.feed.querySelectorAll('button')) card.disabled = busy;
}

async function job(action, noteId) {
  const requestId = Array.from(crypto.getRandomValues(new Uint8Array(16)), (v) => v.toString(16).padStart(2, '0')).join('');
  const options = {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ request_id: requestId, action, session_id: sessionId, note_id: noteId }),
  };
  const response = await request('/v1/jobs', options).catch((error) => {
    if (error.message !== 'network_error') throw error;
    return request('/v1/jobs', options);
  });
  const { job_id: id } = await response.json();
  const deadline = performance.now() + 35000;
  while (performance.now() < deadline) {
    const result = await (await request(`/v1/jobs/${id}`)).json();
    if (result.status === 'done') return result.result;
    if (result.status === 'error') throw new Error(result.error);
    await new Promise((resolve) => setTimeout(resolve, 400));
  }
  throw new Error('timeout');
}

async function image(path, picture) {
  try {
    const blob = await (await request(path)).blob();
    if (!picture.isConnected) return;
    const source = URL.createObjectURL(blob);
    const release = () => URL.revokeObjectURL(source);
    picture.addEventListener('load', release, { once: true });
    picture.addEventListener('error', release, { once: true });
    picture.src = source;
  } catch {
    if (picture.isConnected) picture.alt = '图片加载失败';
  }
}

function clearDetail() {
  ui.title.textContent = '笔记详情';
  ui.author.textContent = '';
  ui.counts.hidden = true;
  ui.meta.hidden = true;
  ui.body.textContent = '从列表选择一篇笔记，查看正文和图片。';
  ui.gallery.replaceChildren();
}

function appendNotes(items) {
  for (const item of items) {
    if (notes.has(item.id)) continue;
    notes.add(item.id);
    const li = document.createElement('li');
    const button = document.createElement('button');
    button.className = 'note';
    button.dataset.noteId = item.id;
    button.addEventListener('click', () => run('detail', item.id));
    const title = document.createElement('span');
    title.textContent = item.title;
    const author = document.createElement('small');
    author.textContent = [item.author, item.likes != null && `赞 ${item.likes}`].filter(Boolean).join(' · ');
    title.append(author);
    li.append(button);
    ui.feed.append(li);
    if (item.cover) {
      const cover = document.createElement('img');
      cover.alt = '';
      button.append(cover);
      void image(item.cover, cover);
    }
    button.append(title);
  }
}

function showDetail(result) {
  for (const card of ui.feed.querySelectorAll('button')) {
    card.setAttribute('aria-current', String(card.dataset.noteId === result.id));
  }
  ui.title.textContent = result.title;
  ui.author.textContent = result.author;
  for (const field of ui.counts.querySelectorAll('[data-count]')) {
    field.textContent = result.counts?.[field.dataset.count] ?? '—';
  }
  ui.counts.hidden = false;
  ui.body.textContent = result.body || (result.images.length ? '' : '这篇笔记没有可显示的正文或图片。');
  const timestamp = result.published_at ? Date.parse(result.published_at) : NaN;
  const date = Number.isFinite(timestamp) ? noteDateFormat.format(timestamp) : '';
  ui.meta.textContent = [date, result.ip_location].filter(Boolean).join(' · ');
  ui.meta.hidden = !ui.meta.textContent;
  ui.gallery.replaceChildren();
  result.images.forEach((path, index) => {
    const picture = document.createElement('img');
    picture.alt = `笔记图片 ${index + 1}`;
    ui.gallery.append(picture);
    void image(path, picture);
  });
  if (matchMedia('(max-width: 740px)').matches) ui.article.scrollIntoView({ block: 'start' });
}

async function run(action, noteId) {
  if (busy) return;
  busy = true;
  updateControls();
  ui.status.textContent = action === 'detail' ? '正在加载笔记…' : '正在读取推荐…';
  try {
    const result = await job(action, noteId);
    if (action === 'detail') {
      showDetail(result);
      ui.status.textContent = `详情已取得，共 ${result.images.length} 张图`;
    } else {
      if (action !== 'more') {
        ui.feed.replaceChildren();
        notes.clear();
        clearDetail();
      }
      sessionId = result.session_id;
      appendNotes(result.items);
      ui.status.textContent = result.items.length ? `已加载 ${notes.size} 篇` : '这次没有出现新笔记';
    }
  } catch (error) {
    ui.status.textContent = messages[error.message] || `请求失败（${error.message}）`;
  } finally {
    busy = false;
    updateControls();
  }
}

ui.access.addEventListener('submit', (event) => {
  event.preventDefault();
  token = ui.token.value.trim();
  sessionStorage.setItem('xhs-device-token', token);
  ui.status.textContent = '令牌已保存到本标签页';
});
for (const action of ['open', 'more', 'refresh']) ui[action].addEventListener('click', () => run(action));

(async function initialize() {
  try {
    const status = await (await request('/v1/health')).json();
    ui.mode.textContent = status.mode === 'demo' ? '本地演示 · 使用样例图文' : '实时模式 · 使用已登录的推荐页';
    ui.token.value = token;
    ui.access.hidden = !status.token_required;
    ready = true;
  } catch (error) {
    ui.mode.textContent = messages[error.message] || '服务连接失败';
  }
  updateControls();
})();
