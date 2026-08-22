# main/ Application Code

This directory contains the MyPaperS3 application code that runs on ESP32-S3.

The app is organized around small `Activity` screens managed by `ActivityManager`, plus shared parser, renderer, storage, font, style, and persistence helpers.

## Entry Points

- `main.cpp`
  - Initializes hardware, SD card, touch calibration, fonts, reader style, and the activity manager.
  - Launches `HomeActivity`.
- `CMakeLists.txt`
  - Registers all application sources and ESP-IDF component dependencies.
- `version.h`
  - Version metadata.

## Activities

Located in `main/activities/`.

- `HomeActivity.cpp`
  - Home screen, main menu, RTC time display, Recent Books card, and cached EPUB cover display.
- `FileBrowserActivity.cpp`
  - SD card browser for books, images, and fonts.
  - Opens TXT, EPUB, and image files.
  - Applies selected font files.
  - Supports file delete confirmation.
- `ReaderActivity.cpp`
  - TXT reader.
  - Uses `TxtBook`, `TextRenderer`, `reader_style`, and `reader_font`.
  - Saves TXT progress and updates Recent Books.
- `EpubReaderActivity.cpp`
  - EPUB reader.
  - Parses current spine item into text/image blocks.
  - Renders text and lazy-extracted image blocks.
  - Saves EPUB progress.
  - Extracts and caches EPUB cover image on first successful open.
  - Provides EPUB TOC navigation.
- `ReaderSettingsActivity.cpp`
  - Shared reader settings panel.
  - Font picker, refresh mode, font size, line spacing, margins, and image display toggle.
- `ImageViewerActivity.cpp`
  - JPG/PNG/BMP image viewer.
  - Scans sibling images in the same folder and supports `UP`/`DOWN` navigation.
- `RssReaderActivity.cpp`
  - RSS source list, article list, article reader, image download/cache, and Bing Wallpaper mode.
- `WifiTransferActivity.cpp`
  - HTTP upload server for books, images, and fonts.
- `CalibrateActivity.cpp`
  - Resistive touch calibration UI.
- `PlaceholderActivity.h`
  - Simple placeholder screen used by unfinished menu entries.

## Parsers

Located in `main/parser/`.

- `txt_parser.*`
  - TXT file loading, encoding detection, line/page reading, and progress persistence.
  - Supports UTF-8, UTF-16LE, GBK, and Latin1 fallback.
- `epub_zip.*`
  - Lightweight ZIP central-directory reader.
  - Reads entries to memory or extracts entries to SD card files.
- `epub_parser.*`
  - EPUB container, OPF manifest/spine, title, TOC, and cover href parsing.
  - Supports EPUB3 `nav.xhtml`, EPUB2 `toc.ncx`, EPUB2 `meta name="cover"`, EPUB3 `cover-image`, and guide cover fallback.
- `epub_html.*`
  - Lightweight XHTML block parser for EPUB text and image blocks.

## Rendering

Located in `main/renderer/`.

- `TextRenderer.*`
  - Shared UTF-8 text measurement, wrapping, glyph preparation, and drawing.
  - Supports runtime FreeType fonts and EDCBook `.bin` bitmap fonts.
  - Includes fast CJK/fullwidth layout-width estimates to reduce slow first-seen glyph loading during wrapping.

## Shared State And Settings

- `reader_style.*`
  - Persistent reader style:
    - refresh mode
    - font size
    - line spacing
    - margins
    - show images toggle
- `reader_font.*`
  - Persistent selected reader font path.
- `recent_books.*`
  - Last opened TXT/EPUB metadata and EPUB cached cover path.
- `touch_calibration.*`
  - Resistive touch calibration stored in NVS.
- `adc_keys.*`
  - ADC key mapping for `BACK`, `UP`, `DOWN`, `OK`, and `EXTRA`.

## Storage

Located in `main/storage/`.

- `sd_card.*`
  - SD card mount and unmount helpers.
Important SD card paths used by code in `main/`:

```text
/sdcard/books/
/sdcard/images/
/sdcard/fonts/
/sdcard/wifi.txt
/sdcard/rss.txt
/sdcard/.reader/recent_books
/sdcard/.reader/reader_style
/sdcard/.reader/reader_font
/sdcard/.reader/txtprogress/
/sdcard/.reader/epubprogress/
/sdcard/.reader/epub_tmp/<book-key>/
/sdcard/.reader/rss_img/
```

Built-in reader font:

```text
builtin:reader.bin                # Embedded from main/spiffs/fonts/reader.bin
```

## Key Behavior

- `BACK`
  - Return to previous screen or previous list level.
- `UP`
  - Previous page/item/image depending on the current activity.
- `DOWN`
  - Next page/item/image depending on the current activity.
- `OK`
  - Open selected item, advance page, or trigger the focused action.
  - In Bing Wallpaper RSS mode, saves the current wallpaper to `/sdcard/images`.
- `EXTRA`
  - Opens reader settings from TXT/EPUB/RSS article readers.
  - Opens EPUB TOC from the EPUB reader where applicable.

## Build Notes

The root project expects ESP-IDF v5.2 with target `esp32s3`.

Run builds from the repository root after loading the ESP-IDF environment:

```bash
idf.py build
```

See the root `README.md` and `docs/project_features.md` for the user-facing feature overview.
