#pragma once

#include <string>

void launch_rss_reader();

// Opens already-downloaded text in the same reflow reader used by RSS
// articles. The source URL is retained as a fallback/reference only.
void launch_rss_article(const std::string &title,
                        const std::string &text,
                        const std::string &source_url = {});
