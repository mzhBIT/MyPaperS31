#pragma once

#include "weread/WereadApi.h"

#include <string>

namespace weread {

bool load_chapter_content(const std::string &book_id, const Chapter &chapter,
                          std::string *xhtml, std::string *error);
bool extract_resource(const std::string &book_id, const Chapter *chapter,
                      const std::string &href,
                      const std::string &out_path);

}  // namespace weread
