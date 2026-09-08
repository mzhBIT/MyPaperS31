#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct RssCacheInfo {
    size_t files = 0;
    uint64_t bytes = 0;
};

// Reports and clears only RSS-owned temporary data. The subscription file and
// wallpapers explicitly saved by the user are intentionally excluded.
RssCacheInfo rss_cache_info();
bool clear_rss_cache(RssCacheInfo *removed = nullptr);

void launch_rss_reader();

// Opens already-downloaded text in the same reflow reader used by RSS
// articles. The source URL is retained as a fallback/reference only.
void launch_rss_article(const std::string &title,
                        const std::string &text,
                        const std::string &source_url = {});
