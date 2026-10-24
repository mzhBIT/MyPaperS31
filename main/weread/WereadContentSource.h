#pragma once

#include "weread/WereadApi.h"

#include <string>

namespace weread {

bool load_chapter_content(const std::string &book_id, const Chapter &chapter,
                          std::string *xhtml, std::string *error);
bool resolve_progress_position(const std::string &book_id, const Chapter &chapter,
                               const std::string &summary, uint64_t *offset,
                               const char **reason = nullptr);
bool extract_resource(const std::string &book_id, const Chapter *chapter,
                      const std::string &href,
                      const std::string &out_path);

}  // namespace weread
