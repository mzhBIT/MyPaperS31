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
static const char *MOBI_PROGRESS_DIR = "/sdcard/.reader/mobiprogress";
static const char *MOBI_CACHE_DIR = "/sdcard/.reader/mobi_tmp";
static const char *MOBI_METADATA_DIR = "/sdcard/.reader/mobi_meta";
static const char *FB2_PROGRESS_DIR = "/sdcard/.reader/fb2progress";
static const char *FB2_CACHE_DIR = "/sdcard/.reader/fb2_tmp";
static const char *RSS_IMAGE_CACHE_DIR = "/sdcard/.reader/rss_img";
static const char *MUSIC_COVER_CACHE_DIR = "/sdcard/.reader/music_cover";
static const char *DEEPSEEK_SESSIONS_DIR = "/sdcard/.reader/deepseek";
static const char *WEREAD_DIR = "/sdcard/.reader/weread";
static const char *WEREAD_BOOKS_DIR = "/sdcard/.reader/weread/books";
static const char *WEREAD_COVERS_DIR = "/sdcard/.reader/weread/covers";
static const char *ZHIHU_HOT_LIST_FILE = "/sdcard/.reader/zhihu_hot_list.json";
static const char *MAP_CACHE_DIR = "/sdcard/.reader/map";
static const char *MAP_IMAGE_FILE = "/sdcard/.reader/map/current.img";
static const char *SYSTEM_DIR = "/sdcard/.reader/system";
static const char *IME_DIR = "/sdcard/.reader/system/ime";
static const char *PINYIN_DICTIONARY_FILE = "/sdcard/.reader/system/ime/pinyin.bin";
static const char *PINYIN_TEXT_FILE = "/sdcard/.reader/system/ime/pinyin.txt";

const char *data_dir() { return APP_DATA_DIR; }
const char *recent_books_file() { return RECENT_BOOKS_FILE; }
const char *reader_style_file() { return READER_STYLE_FILE; }
const char *reader_font_file() { return READER_FONT_FILE; }
const char *txt_progress_dir() { return TXT_PROGRESS_DIR; }
const char *epub_progress_dir() { return EPUB_PROGRESS_DIR; }
const char *epub_cache_dir() { return EPUB_CACHE_DIR; }
const char *epub_metadata_dir() { return EPUB_METADATA_DIR; }
const char *mobi_progress_dir() { return MOBI_PROGRESS_DIR; }
const char *mobi_cache_dir() { return MOBI_CACHE_DIR; }
const char *mobi_metadata_dir() { return MOBI_METADATA_DIR; }
const char *fb2_progress_dir() { return FB2_PROGRESS_DIR; }
const char *fb2_cache_dir() { return FB2_CACHE_DIR; }
const char *rss_image_cache_dir() { return RSS_IMAGE_CACHE_DIR; }
const char *music_cover_cache_dir() { return MUSIC_COVER_CACHE_DIR; }
const char *deepseek_sessions_dir() { return DEEPSEEK_SESSIONS_DIR; }
const char *weread_dir() { return WEREAD_DIR; }
const char *weread_books_dir() { return WEREAD_BOOKS_DIR; }
const char *weread_covers_dir() { return WEREAD_COVERS_DIR; }
const char *zhihu_hot_list_file() { return ZHIHU_HOT_LIST_FILE; }
const char *map_cache_dir() { return MAP_CACHE_DIR; }
const char *map_image_file() { return MAP_IMAGE_FILE; }
const char *system_dir() { return SYSTEM_DIR; }
const char *ime_dir() { return IME_DIR; }
const char *pinyin_dictionary_file() { return PINYIN_DICTIONARY_FILE; }
const char *pinyin_text_file() { return PINYIN_TEXT_FILE; }

void ensure_data_dir()
{
    mkdir(APP_DATA_DIR, 0775);
}

void ensure_dir(const char *path)
{
    ensure_data_dir();
    if (path && path[0]) mkdir(path, 0775);
}

void ensure_ime_dir()
{
    ensure_data_dir();
    mkdir(SYSTEM_DIR, 0775);
    mkdir(IME_DIR, 0775);
}

} // namespace app_paths
