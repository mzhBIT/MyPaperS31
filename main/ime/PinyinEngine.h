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

class Pinyin9Engine {
public:
    explicit Pinyin9Engine(size_t page_size = 7);

    void set_digits(const std::string &digits);
    bool append_digit(char digit);
    bool erase_last();
    void clear();

    const std::string &digits() const { return _digits; }
    const std::string &composition() const;
    size_t interpretation() const { return _interpretation; }
    size_t interpretation_count() const { return _compositions.size(); }
    const std::string &interpretation_at(size_t index) const;
    void select_interpretation(size_t index);
    bool next_interpretation();
    bool previous_interpretation();

    size_t page() const { return _engine.page(); }
    size_t page_count() const { return _engine.page_count(); }
    bool next_page() { return _engine.next_page(); }
    bool previous_page() { return _engine.previous_page(); }
    std::string candidate_on_page(size_t slot) const {
        return _engine.candidate_on_page(slot);
    }

    static std::string digits_for(const std::string &pinyin);

private:
    void rebuild();

    size_t _page_size;
    std::string _digits;
    std::vector<std::string> _compositions;
    size_t _interpretation = 0;
    bool _unmatched = false;
    PinyinEngine _engine;
};

}  // namespace ime
