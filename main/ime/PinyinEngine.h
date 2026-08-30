#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ime {

class PinyinEngine {
public:
    explicit PinyinEngine(size_t page_size = 7);

    void set_composition(const std::string &composition);
    const std::string &composition() const { return _composition; }
    const std::vector<std::string> &candidates() const { return _candidates; }
    size_t page() const { return _page; }
    size_t page_count() const;
    size_t page_size() const { return _page_size; }
    bool next_page();
    bool previous_page();
    std::string candidate_on_page(size_t slot) const;

private:
    void rebuild();
    bool load_binary_candidates();
    void load_external_candidates();

    size_t _page_size;
    size_t _page = 0;
    std::string _composition;
    std::vector<std::string> _candidates;
};

}  // namespace ime
