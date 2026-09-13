#pragma once

namespace app_paths {

const char *data_dir();
const char *recent_books_file();
const char *reader_style_file();
const char *reader_font_file();
const char *txt_progress_dir();
const char *epub_progress_dir();
const char *epub_cache_dir();
const char *epub_metadata_dir();
const char *mobi_progress_dir();
const char *mobi_cache_dir();
const char *mobi_metadata_dir();
const char *fb2_progress_dir();
const char *fb2_cache_dir();
const char *rss_image_cache_dir();
const char *music_cover_cache_dir();
const char *deepseek_sessions_dir();
const char *weread_dir();
const char *weread_books_dir();
const char *weread_covers_dir();
const char *zhihu_hot_list_file();
const char *map_cache_dir();
const char *map_image_file();

void ensure_data_dir();
void ensure_dir(const char *path);

} // namespace app_paths
