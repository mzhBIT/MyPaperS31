#!/usr/bin/env python3
"""Exercise the S31 WeRead sign-in and read-only requests from a PC."""

import argparse
import base64
import hashlib
from html.parser import HTMLParser
import http.client
import io
import json
import secrets
import socket
import ssl
import sys
import tarfile
import time
import zipfile
from pathlib import Path
from urllib.parse import urlencode


API = "i.weread.qq.com"
WECHAT = "open.weixin.qq.com"
WECHAT_POLL = "long.open.weixin.qq.com"
APP_ID = "wxab9b71ad2b90ff34"
NONCE = "weread"
SHELF = "/shelf/sync?synckey=0&lectureSynckey=0"
QR_PATH = Path(__file__).resolve().parents[1] / "tmp" / "weread_pc_qr.jpg"
TLS = ssl.create_default_context()


class TestError(Exception):
    pass


class DirectHTTPS(http.client.HTTPSConnection):
    def __init__(self, host, stage, timeout, family):
        super().__init__(host, timeout=timeout, context=TLS)
        self.stage = stage
        self.family = family
        self.phase = "dns"

    def connect(self):
        begin = time.monotonic()
        endpoints = socket.getaddrinfo(self.host, self.port, self.family, socket.SOCK_STREAM)
        print(f"{self.stage} dns={time.monotonic() - begin:.3f}s addresses={len(endpoints)}", flush=True)
        family, socktype, protocol, _, address = endpoints[0]
        self.phase = "tcp"
        begin = time.monotonic()
        connection = socket.socket(family, socktype, protocol)
        try:
            connection.settimeout(self.timeout)
            connection.connect(address)
            print(f"{self.stage} tcp={time.monotonic() - begin:.3f}s ip={address[0]}", flush=True)
            self.phase = "tls"
            begin = time.monotonic()
            self.sock = TLS.wrap_socket(connection, server_hostname=self.host)
        except BaseException:
            connection.close()
            raise
        print(f"{self.stage} tls={time.monotonic() - begin:.3f}s {self.sock.version()}", flush=True)
        self.phase = "http"


def request(family, stage, host, path, *, data=None, session=None, timeout=12, expected=200, binary=False):
    headers = {"Accept": "application/json", "User-Agent": "MyPaperS31", "Connection": "close"}
    if binary:
        headers["Accept"] = "*/*"
    if host == API:
        headers["appver"] = "2.1.0"
        if session is not None:
            headers.update(vid=session[0], accessToken=session[1])
    body = None
    if data is not None:
        headers["Content-Type"] = "application/json;charset=UTF-8"
        body = json.dumps(data, separators=(",", ":")).encode()

    client = DirectHTTPS(host, stage, timeout, family)
    begin = time.monotonic()
    try:
        client.request("POST" if body is not None else "GET", path, body=body, headers=headers)
        response = client.getresponse()
        limit = 4 * 1024 * 1024
        payload = response.read(limit + 1)
    except (OSError, http.client.HTTPException) as error:
        if stage == "poll" and type(error) is TimeoutError and client.phase == "http":
            return {"wx_errcode": 408}
        raise TestError(f"{stage} phase={client.phase}: {type(error).__name__}") from None
    finally:
        client.close()

    print(f"{stage} http={response.status} bytes={len(payload)} total={time.monotonic() - begin:.3f}s", flush=True)
    if response.status != expected or len(payload) > limit:
        raise TestError(f"{stage} unexpected HTTP status or body size")
    if binary and "json" not in response.headers.get_content_type():
        encrypted = bool(response.getheader("encryptkey"))
        return payload, response.headers.get_content_type(), encrypted
    if expected != 200:
        return {}
    try:
        result = json.loads(payload)
    except (ValueError, UnicodeDecodeError):
        raise TestError(f"{stage} response is not JSON") from None
    if not isinstance(result, dict):
        raise TestError(f"{stage} expected a JSON object")
    if result.get("errcode", result.get("errCode", 0)) not in (0, "0"):
        code = str(result.get("errcode", result.get("errCode", 0)))
        detail = f" (code={code})" if len(code) < 12 and code.lstrip("-").isdigit() else ""
        raise TestError(f"{stage} application rejected the request{detail}")
    return result


def field(result, key):
    value = result.get(key)
    if isinstance(value, int) and not isinstance(value, bool):
        value = str(value)
    if not isinstance(value, str) or not value or not value.isascii() or any(not 33 <= ord(c) <= 126 for c in value):
        raise TestError(f"Missing or invalid field {key}")
    return value


def get_ticket(family):
    return request(family, "ticket", API, "/wxticket?" + urlencode({"nonceStr": NONCE}))


def scan(family):
    ticket = get_ticket(family)
    params = {"appid": APP_ID, "noncestr": NONCE, "timestamp": field(ticket, "timeStamp"),
              "scope": "snsapi_userinfo,snsapi_friend,snsapi_favorites",
              "signature": field(ticket, "signature")}
    qr = request(family, "qr", WECHAT, "/connect/sdk/qrconnect?" + urlencode(params))
    image = qr.get("qrcode")
    if not isinstance(image, dict):
        raise TestError("QR image missing")
    qr_bytes = base64.b64decode(field(image, "qrcodebase64"), validate=True)
    if not (4 <= len(qr_bytes) <= 128 * 1024 and qr_bytes.startswith(b"\xff\xd8")):
        raise TestError("Invalid QR image")
    qr_id = field(qr, "uuid")
    QR_PATH.parent.mkdir(parents=True, exist_ok=True)
    QR_PATH.write_bytes(qr_bytes)
    print(f"QR_READY {QR_PATH}", flush=True)
    deadline = time.monotonic() + 120
    scanned = False
    try:
        while time.monotonic() < deadline:
            query = {"f": "json", "uuid": qr_id}
            if scanned:
                query["last"] = 404
            poll = request(family, "poll", WECHAT_POLL, "/connect/l/qrconnect?" + urlencode(query), timeout=8)
            status = field(poll, "wx_errcode")
            if status == "405":
                return field(poll, "wx_code")
            if status == "404" and not scanned:
                scanned = True
                print("QR_SCANNED waiting for approval in WeChat", flush=True)
            if status not in ("404", "408"):
                raise TestError(f"Scan ended with status {status}" if status in ("402", "403") else "Unexpected scan state")
            time.sleep(1)
    finally:
        QR_PATH.unlink(missing_ok=True)
    raise TestError("Scan timed out")


def array(result, *names):
    for name in names:
        items = result.get(name)
        if isinstance(items, list) and items:
            return items
    return []


class ChapterHTML(HTMLParser):
    def __init__(self):
        super().__init__()
        self.hidden = None
        self.text = []

    def handle_starttag(self, tag, attrs):
        if self.hidden is None and tag in {"head", "script", "style", "nav"}:
            self.hidden = tag

    def handle_endtag(self, tag):
        if self.hidden == tag:
            self.hidden = None

    def handle_data(self, data):
        if self.hidden is None:
            self.text.append(data)


def text_count(name, payload):
    text = payload.decode("utf-8-sig", errors="replace")
    if "\0" in text or text.count("\ufffd") > len(text) // 50:
        return 0
    if Path(name).suffix.lower() in {".html", ".htm", ".xhtml"} or text.lstrip().startswith("<"):
        parser = ChapterHTML()
        parser.feed(text)
        text = " ".join(parser.text)
    return sum(character.isalpha() for character in text)


def is_text_entry(name):
    entry = Path(name.casefold())
    return entry.suffix in {".txt", ".html", ".htm", ".xhtml"} and entry.stem not in {
        "cover", "nav", "toc", "titlepage", "index"}


def inspect_chapter(payload, media_type):
    source = io.BytesIO(payload)
    limit = 1024 * 1024
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            candidates = [entry for entry in archive.infolist() if is_text_entry(entry.filename)]
            if any(entry.flag_bits & 1 for entry in candidates):
                raise TestError("Chapter ZIP contains protected text entries")
            return "ZIP", max((text_count(e.filename, archive.read(e)) for e in candidates
                               if e.file_size <= limit), default=0)
    source.seek(0)
    try:
        with tarfile.open(fileobj=source, mode="r|*") as archive:
            count = 0
            for entry in archive:
                if entry.isreg() and entry.size <= limit and is_text_entry(entry.name):
                    with archive.extractfile(entry) as chapter:
                        count = max(count, text_count(entry.name, chapter.read(limit)))
            return "TAR", count
    except tarfile.ReadError:
        if media_type == "text/plain" and len(payload) <= limit:
            return "TXT", text_count("chapter.txt", payload)
        raise TestError("Chapter response is not a supported plaintext package") from None


def test_chapter(family, session, book_id, shelf_book, catalog_book, chapters):
    detail = catalog_book.get("book") or {}
    chapter = next((item for item in chapters if str(item.get("chapterIdx", "0")).isdigit()
                    and int(item.get("chapterIdx", 0)) > 0
                    and str(item.get("wordCount", "0")).isdigit() and int(item.get("wordCount", 0)) >= 100),
                   None)
    if chapter is None:
        chapter = next((item for item in chapters if str(item.get("chapterIdx", "0")).isdigit()
                        and int(item.get("chapterIdx", 0)) > 0), None)
    if chapter is None:
        raise TestError("No chapter candidate for preview")
    book_format = str(catalog_book.get("format") or detail.get("format") or shelf_book.get("format") or "")
    params = {
        "bookId": book_id, "chapters": field(chapter, "chapterUid"),
        "bookVersion": detail.get("version", shelf_book.get("version", 0)),
        "bookType": book_format.lower() if book_format.lower() in {"epub", "txt"} else "",
        "preview": 1, "offline": 0, "preload": 0, "stopAutoPayWhenBNE": 1,
    }
    result = request(family, "chapter-preview", API, "/book/chapterdownload?" + urlencode(params),
                     session=session, binary=True)
    if isinstance(result, dict):
        raise TestError("Chapter endpoint returned JSON instead of a text package")
    payload, media_type, encrypted = result
    if encrypted:
        raise TestError(f"Chapter endpoint returned a protected package ({len(payload)} bytes)")
    kind, letters = inspect_chapter(payload, media_type)
    if letters < 100:
        raise TestError(f"Chapter package {kind} has no verified body text ({letters} letters)")
    print(f"CHAPTER_OK preview=1 format={kind} text_chars={letters}", flush=True)


def run_login(family, chapter_test):
    code = scan(family)
    device = "eink" + secrets.token_hex(6)
    stamp = time.time_ns() // 1_000_000
    nonce = secrets.randbelow(1000)
    signature = hashlib.sha256(f"{stamp}{device}{nonce}".encode()).hexdigest()
    data = {"code": code, "isFromQrcode": 1, "isAutoLogout": 0, "deviceType": 3,
            "deviceId": device, "deviceName": "MyPaper PC Test", "trackId": "",
            "timestamp": stamp, "random": nonce, "signature": signature}
    login = request(family, "login", API, "/login", data=data)
    session = field(login, "vid"), field(login, "accessToken")
    shelf = request(family, "shelf", API, SHELF, session=session)
    if "vid" in shelf and field(shelf, "vid") != session[0]:
        raise TestError("Shelf account mismatch")
    books = shelf.get("books")
    if not isinstance(books, list):
        raise TestError("Shelf response missing books")
    print(f"SHELF_OK books={len(books)}", flush=True)
    if not books:
        print("CATALOG_SKIPPED shelf is empty", flush=True)
        if chapter_test:
            raise TestError("No shelf book available for chapter preview")
        return
    book_id = field(books[0], "bookId")
    catalog = request(family, "chapterInfos", API, "/book/chapterInfos", session=session,
                      data={"bookIds": [book_id], "synckeys": [0]})
    for book in array(catalog, "data", "updated"):
        detail = book.get("book") or {}
        if str(book.get("bookId", detail.get("bookId"))) == book_id:
            chapters = array(book, "data", "updated")
            if chapters:
                print(f"CATALOG_OK chapters={len(chapters)}", flush=True)
                if chapter_test:
                    test_chapter(family, session, book_id, books[0], book, chapters)
                return
    raise TestError("No chapters for first shelf book")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--login", action="store_true", help="wait for WeChat QR approval and read shelf/catalog")
    parser.add_argument("--chapter", action="store_true", help="sign in and test a single chapter preview")
    parser.add_argument("--family", choices=("IPv4", "auto"), default="IPv4")
    args = parser.parse_args()
    family = socket.AF_INET if args.family == "IPv4" else socket.AF_UNSPEC
    try:
        if args.login or args.chapter:
            run_login(family, args.chapter)
        else:
            request(family, "shelf-anonymous", API, SHELF, expected=401)
            ticket = get_ticket(family)
            field(ticket, "timeStamp")
            field(ticket, "signature")
            print("PROBE_OK", flush=True)
    except (TestError, OSError, ValueError, TypeError, AttributeError, zipfile.BadZipFile, tarfile.TarError) as error:
        detail = str(error) if isinstance(error, TestError) else type(error).__name__
        print(f"FAILED {detail}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
