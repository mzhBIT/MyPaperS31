#include "ime/PinyinEngine.h"
#include <cassert>
#include <cstdio>

// Exercise the always-available vocabulary without an SD-card dictionary.
namespace app_paths {
const char *pinyin_dictionary_file() { return "__missing_pinyin_test.bin"; }
const char *pinyin_text_file() { return "__missing_pinyin_test.txt"; }
}

int main()
{
    ime::Pinyin9Engine engine(3);
    engine.set_digits("64");
    size_t ni = 0;
    while (ni < engine.interpretation_count() && engine.interpretation_at(ni) != "ni") ++ni;
    assert(ni < engine.interpretation_count());
    engine.select_interpretation(ni);
    assert(!engine.candidate_on_page(0).empty());
    assert(engine.append_digit('9'));
    assert(engine.composition() == "niw");
    assert(ime::Pinyin9Engine::digits_for(engine.composition()) == "649");
    assert(engine.candidate_on_page(0).empty());
    assert(engine.append_digit('9'));
    assert(engine.composition() == "niww");
    assert(engine.erase_last());
    assert(engine.composition() == "niw");
    assert(engine.erase_last());
    assert(!engine.candidate_on_page(0).empty());
    engine.set_digits("999999");
    assert(engine.composition() == "wwwwww");
    assert(engine.interpretation_count() == 1);
    assert(engine.candidate_on_page(0).empty());
    assert(engine.append_digit('0'));
    assert(engine.composition() == "wwwwww'");
    engine.clear();
    assert(engine.composition().empty() && engine.digits().empty());
    assert(engine.interpretation_count() == 0);
    assert(engine.candidate_on_page(0).empty());
    std::puts("Nine-key: unmatched spelling retained, no false candidates, backspace recovery passed");
}
