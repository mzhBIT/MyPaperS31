import argparse
import os
import sys
from pathlib import Path

import uvicorn

from .app import create_app
from .reader import BrowserSource, DemoSource, downloader_base_url


def main():
    parser = argparse.ArgumentParser(description="小红书图文阅读服务")
    parser.add_argument("--mode", choices=["demo", "live"], default=os.getenv("XHS_MODE", "demo"))
    parser.add_argument("--host", default=os.getenv("XHS_BIND", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=os.getenv("XHS_PORT", "8091"))
    parser.add_argument("--data-dir", type=Path, default=os.getenv("XHS_DATA_DIR", str(Path(__file__).parent / "data")))
    parser.add_argument("--downloader-url", default=os.getenv("XHS_DOWNLOADER_URL", ""))
    parser.add_argument("--browser-channel", default=os.getenv("XHS_BROWSER_CHANNEL", "msedge" if sys.platform == "win32" else ""))
    parser.add_argument("--headless", action="store_true", default=os.getenv("XHS_HEADLESS") == "1")
    args = parser.parse_args()
    token = os.getenv("XHS_API_TOKEN") or None
    if (args.mode == "live" or args.host not in {"127.0.0.1", "::1", "localhost"}) and not token:
        parser.error("请设置 XHS_API_TOKEN")
    if token and len(token) < 16:
        parser.error("XHS_API_TOKEN 需要至少 16 个字符")
    if args.mode == "live":
        try:
            address = downloader_base_url(args.downloader_url)
        except ValueError:
            parser.error("请粘贴你在 Windows 浏览器里能打开的 XHS-Downloader 地址，可以包含 /docs")
        print(f"XHS-Downloader: {address}", flush=True)
        source = BrowserSource(args.data_dir / "chromium-profile", address,
                               os.getenv("XHS_DOWNLOADER_TOKEN"), args.browser_channel or None, args.headless)
    else:
        source = DemoSource()
    uvicorn.run(create_app(source, args.data_dir, token), host=args.host, port=args.port, proxy_headers=False)


if __name__ == "__main__":
    main()
