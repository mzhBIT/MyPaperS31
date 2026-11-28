import hashlib
import io
import json
import logging
import os
import re
import time
import zlib
from collections import OrderedDict
from contextlib import suppress
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from struct import pack
from tempfile import NamedTemporaryFile
from threading import Lock
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener
from uuid import uuid4

from PIL import Image, ImageDraw, ImageOps


WEB_HOSTS = {"www.xiaohongshu.com", "www.rednote.com"}
BATCH_SIZE = 6
NOTE_ID = re.compile(r"[0-9a-f]{24}")
DEMO_IMAGE = re.compile(r"demo://(\d+)/(\d+)")
MAX_IMAGE_BYTES = 16 * 1024 * 1024
MAX_IMAGE_PIXELS = 40_000_000
MAX_IMAGE_EDGE = 1400
PAGE_SCRIPT = (Path(__file__).parent / "cards.js").read_text(encoding="utf-8")
LOGGER = logging.getLogger("xhs_reader")


class ReaderError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


@dataclass(frozen=True)
class Card:
    id: str
    title: str
    author: str
    url: str
    cover: str
    likes: str | None = None


def image_url_allowed(url):
    if re.search(r"[\x00-\x20\x7f\\]", url):
        return False
    try:
        parsed = urlsplit(url)
        host = parsed.hostname or ""
        return (
            parsed.scheme == "https" and parsed.port in {None, 443}
            and parsed.username is None and parsed.password is None
            and re.fullmatch(r"[a-z0-9.-]+", host) is not None
            and (host == "ci.xiaohongshu.com" or host.endswith(".xhscdn.com"))
        )
    except ValueError:
        return False


def first_image_url(candidates):
    for value in candidates:
        if not isinstance(value, str):
            continue
        url = value.strip()
        if url.startswith("//"):
            url = "https:" + url
        elif url.startswith("http://"):
            url = "https://" + url[7:]
        if image_url_allowed(url):
            return url
    return ""


class MediaRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not image_url_allowed(newurl):
            raise ReaderError("image_unavailable")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def downloader_base_url(value):
    url = urlsplit(value.strip())
    if (url.scheme not in {"http", "https"} or not url.hostname or url.username is not None
            or url.password is not None):
        raise ValueError("Invalid XHS-Downloader URL")
    # Accept the address copied from Swagger as well as the service address.
    _ = url.port
    path = url.path.rstrip("/")
    if path.endswith(("/docs", "/redoc", "/openapi.json")):
        path = path.rsplit("/", 1)[0]
    elif url.query or url.fragment:
        raise ValueError("Invalid XHS-Downloader URL")
    return url._replace(path=path, query="", fragment="").geturl()


def interaction_count(value):
    if type(value) is int:
        value = str(value)
    if isinstance(value, str):
        value = value.strip()
        if re.fullmatch(r"[0-9]+(?:,[0-9]{3})*(?:\.[0-9]+)?[万亿]?\+?", value):
            return value
    return None


def note_ip_location(value):
    if not isinstance(value, str):
        return None
    text = value.strip().removeprefix("IP属地：").removeprefix("IP属地:").strip()
    if text in {"", "-", "-1", "未知"} or len(text) > 64:
        return None
    return text


def note_date(timestamp):
    if type(timestamp) not in (int, float):
        return None
    if 1_000_000_000_000 <= timestamp < 4_102_444_800_000:
        timestamp /= 1000
    return datetime.fromtimestamp(timestamp, UTC).isoformat() if 0 < timestamp < 4_102_444_800 else None


def note_link_matches(link, note_id):
    try:
        url = urlsplit(link)
        return url.scheme == "https" and url.netloc in WEB_HOSTS and url.path in {
            f"/explore/{note_id}", f"/discovery/item/{note_id}"
        }
    except ValueError:
        return False


def encode_gray4(image):
    width, height = image.size
    stride = (width + 1) // 2
    pixels = image.tobytes()
    packed = bytearray(stride * height)
    for row in range(height):
        start = row * width
        scanline = pixels[start:start + width]
        right = scanline[1::2]
        if width % 2:
            right += b"\xff"
        packed[row * stride:(row + 1) * stride] = bytes(
            (left_pixel & 0xf0) | (right_pixel >> 4)
            for left_pixel, right_pixel in zip(scanline[::2], right)
        )
    return pack(">4sHH", b"G4Z1", width, height) + zlib.compress(packed, level=9)


class MediaCache:
    def __init__(self, directory, demo=False):
        self.directory = directory
        self.demo = demo
        self.directory.mkdir(parents=True, exist_ok=True)
        self._sources = OrderedDict()
        self._lock = Lock()
        self._write_locks = [Lock() for _ in range(16)]

    def register(self, url):
        if not isinstance(url, str):
            return None
        if url.startswith("//"):
            url = "https:" + url
        cdn_url = url if image_url_allowed(url) else None
        if cdn_url is None and not (self.demo and DEMO_IMAGE.fullmatch(url)):
            return None
        key = hashlib.sha256(url.encode()).hexdigest()
        with self._lock:
            self._sources[key] = url
            self._sources.move_to_end(key)
            if len(self._sources) > 2400:
                self._sources.popitem(last=False)
        return f"/v1/media/{key}.jpg", {
            "gray4": f"/v1/media/{key}.gray4",
            "cdn_url": cdn_url,
        }

    def get(self, key, gray4_size=None):
        if gray4_size is None:
            target = self.directory / f"{key}.jpg"
        else:
            width, height = gray4_size
            target = self.directory / f"{key}-{width}x{height}.gray4"
        with self._write_locks[int(key[:2], 16) % len(self._write_locks)]:
            if target.is_file():
                return target
            with self._lock:
                source = self._sources.get(key)
            if not source:
                raise ReaderError("image_missing")
            self._write(target, source, gray4_size)
            return target

    def _write(self, target, source, gray4_size):
        tmp_path = None
        try:
            match = DEMO_IMAGE.fullmatch(source) if self.demo else None
            if match:
                oriented = demo_image(*map(int, match.groups()))
            else:
                opener = build_opener(MediaRedirect())
                req = Request(source, headers={"Referer": "https://www.xiaohongshu.com/"})
                with opener.open(req, timeout=10) as response:
                    content = response.read(MAX_IMAGE_BYTES + 1)
                if len(content) > MAX_IMAGE_BYTES:
                    raise ReaderError("image_unavailable")
                with Image.open(io.BytesIO(content)) as original:
                    if original.width * original.height > MAX_IMAGE_PIXELS:
                        raise ReaderError("image_unavailable")
                    oriented = ImageOps.exif_transpose(original)
            with oriented:
                if gray4_size and ("A" in oriented.getbands() or "transparency" in oriented.info):
                    with oriented.convert("RGBA") as rgba, rgba.getchannel("A") as alpha, rgba.convert("L") as gray:
                        image = Image.new("L", rgba.size, 255)
                        image.paste(gray, mask=alpha)
                else:
                    image = oriented.convert("L" if gray4_size else "RGB")
            with image, NamedTemporaryFile(dir=self.directory, suffix=".tmp", delete=False) as output:
                tmp_path = Path(output.name)
                image.thumbnail(gray4_size or (MAX_IMAGE_EDGE, MAX_IMAGE_EDGE), Image.Resampling.LANCZOS)
                if gray4_size:
                    output.write(encode_gray4(image))
                else:
                    image.save(output, format="JPEG", quality=84, optimize=True)
            os.replace(tmp_path, target)
            with self._lock:
                files = sorted(self.directory.glob(f"*{target.suffix}"), key=lambda path: path.stat().st_mtime)
                for old_file in files[:-400]:
                    try:
                        old_file.unlink(missing_ok=True)
                    except PermissionError:
                        continue  # A Windows client may still be reading this file.
        except (OSError, ValueError, Image.DecompressionBombError) as error:
            raise ReaderError("image_unavailable") from error
        finally:
            if tmp_path is not None:
                tmp_path.unlink(missing_ok=True)


def demo_image(number, variant):
    palette = [("#d9ddca", "#386861"), ("#f1dfce", "#9a4e40"), ("#d4e1e8", "#445d7e")]
    background, foreground = palette[(number + variant) % len(palette)]
    image = Image.new("RGB", (880, 560), background)
    draw = ImageDraw.Draw(image)
    draw.ellipse((320, 70, 780, 530), fill=foreground)
    draw.rectangle((90, 190, 520, 440), outline="#fffdf8", width=12)
    draw.text((102, 111), f"DEMO / {number:02d}", fill=foreground, font_size=34)
    return image


class DemoSource:
    mode = "demo"

    def reset(self):
        self.position = 0

    def cards(self, seen, advance):
        labels = [
            "街角的一间书店", "周末步行路线", "家里的光线", "一份手冲笔记", "雨后的绿道", "去海边的一天",
            "窗台上的植物", "午后的展览", "旧城区散步", "写给春天的信", "山间小路", "移动中的阅读",
        ]
        likes = ("42", "0", "1.2万+", None)
        if advance:
            self.position = min(self.position + BATCH_SIZE, len(labels))
        return [
            Card(str(number), label, "本地演示", "", f"demo://{number}/0", likes[(number - 1) % len(likes)])
            for number, label in enumerate(labels, 1)
            if number <= self.position + BATCH_SIZE and str(number) not in seen
        ]

    def detail(self, card):
        return {
            "title": card.title, "author": card.author,
            "body": "这是一条本地图文样例。\n\n试着返回列表，再按加载更多。图片由当前服务生成。",
            "published_at": "2026-11-27T05:35:41+00:00",
            "ip_location": "上海" if int(card.id) % 2 else None,
            "type": "image_text",
            "counts": {"comments": "3", "favorites": "18", "likes": card.likes, "shares": "0"},
            "sources": [f"demo://{card.id}/1", f"demo://{card.id}/2"],
        }

    def close(self):
        pass


class BrowserSource:
    mode = "live"

    def __init__(self, profile, downloader_url, downloader_token=None, channel=None, headless=False):
        self.profile = profile
        self.endpoint = downloader_base_url(downloader_url) + "/xhs/detail"
        self.token = downloader_token
        self.channel = channel
        self.headless = headless
        self.driver = None
        self.context = None
        self.page = None
        self.browser_closed = False

    def close(self):
        try:
            if self.context is not None and not self.browser_closed:
                self.context.close()
        finally:
            if self.driver is not None:
                self.driver.stop()
            self.driver = self.context = self.page = None
            self.browser_closed = False

    def _tab(self):
        from playwright.sync_api import Error, sync_playwright

        if self.browser_closed:
            self.close()
        if self.context is None:
            try:
                self.driver = sync_playwright().start()
                self.context = self.driver.chromium.launch_persistent_context(
                    str(self.profile), channel=self.channel, headless=self.headless,
                    viewport={"width": 1150, "height": 850},
                )
                self.context.on("close", lambda _: setattr(self, "browser_closed", True))
            except Error as error:
                self.close()
                raise ReaderError("browser_start_failed") from error
        if self.page is None or self.page.is_closed():
            self.page = self.context.pages[0] if self.context.pages else self.context.new_page()
            self.page.set_default_timeout(5000)
        return self.page

    def reset(self):
        from playwright.sync_api import Error

        page = self._tab()
        try:
            page.goto("https://www.xiaohongshu.com/explore", wait_until="domcontentloaded", timeout=15000)
        except Error as error:
            if page.is_closed():
                self.close()
            raise ReaderError("browser_navigation_failed") from error

    def cards(self, seen, advance):
        from playwright.sync_api import Error

        page = self._tab()
        try:
            available = {}
            scrolled = False
            limit = time.monotonic() + 6
            while True:
                for card in read_cards(page):
                    if card.id in seen:
                        continue
                    previous = available.get(card.id)
                    if previous is None or (not previous.cover and card.cover):
                        available[card.id] = card
                if len(available) >= BATCH_SIZE or time.monotonic() >= limit:
                    return list(available.values())
                if not scrolled and (advance or available):
                    page.mouse.move(575, 425)
                    page.mouse.wheel(0, 1050)
                    scrolled = True
                page.wait_for_timeout(250)
        except Error as error:
            if page.is_closed():
                self.close()
            raise ReaderError("browser_navigation_failed") from error

    def _browser_note(self, card):
        if self.context is None or self.browser_closed:
            return None
        from playwright.sync_api import Error

        tab = None
        try:
            tab = self.context.new_page()
            tab.route("**/*", lambda route: route.abort() if route.request.resource_type in {
                "image", "media", "font",
            } else route.fallback())
            response = tab.goto(card.url, wait_until="domcontentloaded", timeout=4000)
            if response is None:
                return None
            final = urlsplit(tab.url)
            if final.hostname not in WEB_HOSTS and not (final.hostname or "").endswith(".xiaohongshu.com"):
                return None
            if response.status == 429:
                LOGGER.warning("Browser detail rate limited note=%s", card.id)
                return {"error": "detail_rate_limited"}
            if any(word in final.path.lower() for word in ("login", "verify", "captcha", "security")):
                return {"error": "login_required"}
            if not response.ok or final.hostname not in WEB_HOSTS:
                LOGGER.warning("Browser detail note=%s HTTP %d", card.id, response.status)
                return None
            return tab.evaluate("""id => {
                const dialogs = document.querySelectorAll('[role="dialog"], [class*="login-modal"], [class*="captcha"]');
                const challenge = /扫码登录|请先登录|安全验证|请完成验证|请完成安全验证|Scan QR code|Log in to explore/i;
                if ([...dialogs].some(dialog => dialog.getClientRects().length && challenge.test(dialog.innerText)))
                    return {error: 'login_required'};
                const store = window.__INITIAL_STATE__?.note?.noteDetailMap;
                const entries = store?._rawValue ?? store?.value ?? store;
                const note = entries?.[id]?.note;
                if (!note || (note.noteId ?? note.id ?? id) !== id) return null;
                const images = note.imageList ?? note.image_list;
                const counts = note.interactInfo ?? note.interact_info;
                return {
                    type: note.type, video: Boolean(note.video?.media), title: note.title, body: note.desc,
                    author: note.user?.nickname ?? note.user?.nickName,
                    timestamp: note.time, ip_location: note.ipLocation ?? note.ip_location,
                    likes: counts?.likedCount, comments: counts?.commentCount,
                    favorites: counts?.collectedCount, shares: counts?.shareCount,
                    images: Array.isArray(images) ? images.slice(0, 32).map(image => {
                        const variants = image?.infoList ?? image?.info_list;
                        return [image?.urlDefault, image?.url, image?.urlPre,
                                ...(Array.isArray(variants) ? variants.map(item => item?.url) : [])];
                    }) : [],
                };
            }""", card.id)
        except Error:
            return None
        finally:
            if tab is not None:
                with suppress(Error):
                    tab.close()

    def _browser_detail(self, card):
        note = self._browser_note(card)
        if not note:
            raise ReaderError("detail_unavailable")
        if note.get("error"):
            LOGGER.warning("Browser detail blocked note=%s reason=%s", card.id, note["error"])
            raise ReaderError(note["error"])
        if note.get("type") == "video" or note.get("video"):
            raise ReaderError("unsupported_note_type")
        body = note.get("body") if isinstance(note.get("body"), str) else ""
        sources = [url for candidates in note["images"] if (url := first_image_url(candidates))]
        if not body.strip() and not sources:
            raise ReaderError("detail_unavailable")
        LOGGER.info("Detail recovered from signed-in browser note=%s images=%d", card.id, len(sources))
        return {
            "title": note.get("title") or card.title, "author": note.get("author") or card.author,
            "body": body, "published_at": note_date(note.get("timestamp")),
            "ip_location": note_ip_location(note.get("ip_location")), "type": "image_text",
            "counts": {
                "comments": interaction_count(note.get("comments")),
                "favorites": interaction_count(note.get("favorites")),
                "likes": interaction_count(note.get("likes")) or card.likes,
                "shares": interaction_count(note.get("shares")),
            },
            "sources": sources,
        }

    def detail(self, card):
        payload = {"url": card.url, "download": False, "check_record": False}
        req = Request(self.endpoint, data=json.dumps(payload).encode(),
                      headers={"Content-Type": "application/json"}, method="POST")
        if self.token:
            req.add_header("Authorization", f"Bearer {self.token}")
        try:
            with build_opener(ProxyHandler({}), NoRedirect()).open(req, timeout=15) as response:
                raw = response.read(1024 * 1024)
        except HTTPError as error:
            LOGGER.warning("XHS-Downloader %s returned HTTP %d", self.endpoint, error.code)
            codes = {401: "downloader_auth_required", 403: "downloader_auth_required",
                     404: "downloader_route_missing", 405: "downloader_route_missing"}
            raise ReaderError(codes.get(error.code, "downloader_http_error")) from error
        except (TimeoutError, URLError) as error:
            reason = error.reason if isinstance(error, URLError) else error
            LOGGER.warning("XHS-Downloader %s request failed (%s)", self.endpoint, type(reason).__name__)
            raise ReaderError("downloader_timeout" if isinstance(reason, TimeoutError) else "downloader_unavailable") from error
        except OSError as error:
            LOGGER.warning("XHS-Downloader %s request failed (%s)", self.endpoint, type(error).__name__)
            raise ReaderError("downloader_unavailable") from error
        try:
            payload = json.loads(raw)
            data = payload.get("data")
        except (ValueError, AttributeError) as error:
            raise ReaderError("downloader_response_invalid") from error
        if not isinstance(data, dict) or data.get("作品ID") != card.id:
            if not data:
                reason = {"提取小红书作品链接失败": "link_extraction_failed",
                          "获取小红书作品数据失败": "upstream_detail_failed"}.get(
                              payload.get("message") if isinstance(payload.get("message"), str) else None,
                              "empty_data")
            elif not isinstance(data, dict):
                reason = "unexpected_data_type"
            elif not data.get("作品ID"):
                reason = "missing_note_id"
            else:
                reason = "mismatched_note_id"
            LOGGER.warning("XHS-Downloader incomplete detail note=%s reason=%s; trying signed-in browser",
                           card.id, reason)
            return self._browser_detail(card)
        if data.get("作品类型") in {"视频", "图集"}:
            raise ReaderError("unsupported_note_type")
        location = note_ip_location(data.get("IP归属地")) or note_ip_location(data.get("ipLocation"))
        if not location:
            browser_note = self._browser_note(card)
            if browser_note:
                location = note_ip_location(browser_note.get("ip_location"))
        images = data.get("下载地址")
        return {
            "title": data.get("作品标题") or card.title, "author": data.get("作者昵称") or card.author,
            "body": data.get("作品描述") or "", "published_at": note_date(data.get("时间戳")),
            "ip_location": location, "type": "image_text",
            "counts": {
                "comments": interaction_count(data.get("评论数量")),
                "favorites": interaction_count(data.get("收藏数量")),
                "likes": interaction_count(data.get("点赞数量")),
                "shares": interaction_count(data.get("分享数量")),
            },
            "sources": images if isinstance(images, list) else [],
        }


def read_cards(page):
    parsed = urlsplit(page.url)
    if parsed.hostname not in WEB_HOSTS:
        raise ReaderError("login_required" if parsed.hostname and parsed.hostname.endswith(".xiaohongshu.com")
                          else "browser_navigation_failed")
    if any(word in parsed.path.lower() for word in ("login", "verify", "captcha", "security")):
        raise ReaderError("login_required")
    snapshot = page.evaluate(PAGE_SCRIPT)
    if snapshot["requires_login"]:
        raise ReaderError("login_required")
    cards = {}
    for row in snapshot["cards"]:
        note_id = row["id"]
        if not NOTE_ID.fullmatch(note_id):
            continue
        link = row["href"]
        valid_link = note_link_matches(link, note_id)
        if row["token"] and (not valid_link or not parse_qs(urlsplit(link).query).get("xsec_token")):
            query = urlencode({"source": "webshare", "xhsshare": "pc_web", "xsec_token": row["token"],
                               "xsec_source": "pc_share"})
            link = f"https://{parsed.hostname}/discovery/item/{note_id}?{query}"
            valid_link = True
        if not valid_link:
            continue
        cover = first_image_url(row.get("covers", [row.get("cover")]))
        cards.setdefault(note_id, Card(note_id, row["title"] or "未命名笔记", row["author"],
                                       link, cover, interaction_count(row.get("likes"))))
    return list(cards.values())


class Reader:
    def __init__(self, source, media):
        self.source = source
        self.media = media
        self.session_id = None
        self.seen = OrderedDict()

    def perform(self, job):
        if job.action in {"open", "refresh"}:
            self.source.reset()
            cards = self.source.cards(set(), advance=False)
            if not cards:
                raise ReaderError("feed_unavailable")
            self.seen.clear()
            self.session_id = uuid4().hex
            return self._batch(cards)
        if not self.session_id or job.session_id != self.session_id:
            raise ReaderError("session_expired")
        if job.action == "more":
            return self._batch(self.source.cards(set(self.seen), advance=True))
        card = self.seen.get(job.note_id)
        if card is None:
            raise ReaderError("note_not_found")
        detail = self.source.detail(card)
        images = []
        image_media = []
        for source in detail.pop("sources")[:32]:
            if paths := self.media.register(source):
                jpeg, options = paths
                images.append(jpeg)
                image_media.append(options)
        return {"id": card.id, **detail, "images": images, "image_media": image_media}

    def _batch(self, cards):
        items = []
        for card in cards[:BATCH_SIZE]:
            self.seen[card.id] = card
            cover, cover_media = self.media.register(card.cover) or (None, None)
            items.append({"id": card.id, "title": card.title, "author": card.author,
                          "likes": card.likes, "cover": cover, "cover_media": cover_media})
        while len(self.seen) > 300:
            self.seen.popitem(last=False)
        LOGGER.info("Recommendation batch: items=%d covers=%d", len(items),
                    sum(item["cover_media"] is not None for item in items))
        return {"session_id": self.session_id, "items": items}
