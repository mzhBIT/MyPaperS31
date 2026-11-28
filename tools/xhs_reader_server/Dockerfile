ARG READER_BASE_IMAGE=joeanamier/xhs-downloader:2.8
FROM ${READER_BASE_IMAGE}

WORKDIR /app
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/reader-browsers \
    XHS_BROWSER_CHANNEL=chromium

COPY requirements*.txt /app/tools/xhs_reader_server/
RUN python3 -m venv /opt/reader-venv \
    && /opt/reader-venv/bin/pip install --no-cache-dir -r /app/tools/xhs_reader_server/requirements-live.txt

ARG DEBIAN_MIRROR
RUN if [ -n "$DEBIAN_MIRROR" ]; then \
        sed -i "s|https\?://deb.debian.org|${DEBIAN_MIRROR}|g" /etc/apt/sources.list.d/debian.sources; \
    fi \
    && apt-get update \
    && apt-get install -y --no-install-recommends novnc websockify x11vnc x11-utils xvfb \
    && /opt/reader-venv/bin/python -m playwright install-deps chromium \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home reader \
    && install -d -o reader -g reader /data

ARG PLAYWRIGHT_CHROMIUM_DOWNLOAD_HOST
RUN /opt/reader-venv/bin/python -m playwright install --no-shell chromium

COPY . /app/tools/xhs_reader_server/

USER reader
ENTRYPOINT ["/bin/sh", "/app/tools/xhs_reader_server/entrypoint.sh"]
