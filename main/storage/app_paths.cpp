#include "storage/app_paths.h"

#include <sys/stat.h>

namespace app_paths {

static const char *APP_DATA_DIR = "/sdcard/.reader";
static const char *RECENT_BOOKS_FILE = "/sdcard/.reader/recent_books";
static const char *READER_STYLE_FILE = "/sdcard/.reader/reader_style";
static const char *READER_FONT_FILE = "/sdcard/.reader/reader_font";
static const char *TXT_PROGRESS_DIR = "/sdcard/.reader/txtprogress";
static const char *EPUB_PROGRESS_DIR = "/sdcard/.reader/epubprogress";
static const char *EPUB_CACHE_DIR = "/sdcard/.reader/epub_tmp";
static const char *EPUB_METADATA_DIR = "/sdcard/.reader/epub_meta";
static const char *RSS_IMAGE_CACHE_DIR = "/sdcard/.reader/rss_img";

const char *data_dir() { return APP_DATA_DIR; }
const char *recent_books_file() { return RECENT_BOOKS_FILE; }
const char *reader_style_file() { return READER_STYLE_FILE; }
const char *reader_font_file() { return READER_FONT_FILE; }
const char *txt_progress_dir() { return TXT_PROGRESS_DIR; }
const char *epub_progress_dir() { return EPUB_PROGRESS_DIR; }
const char *epub_cache_dir() { return EPUB_CACHE_DIR; }
const char *epub_metadata_dir() { return EPUB_METADATA_DIR; }
const char *rss_image_cache_dir() { return RSS_IMAGE_CACHE_DIR; }

void ensure_data_dir()
{
    mkdir(APP_DATA_DIR, 0775);
}

void ensure_dir(const char *path)
{
    ensure_data_dir();
    if (path && path[0]) mkdir(path, 0775);
}

} // namespace app_paths
