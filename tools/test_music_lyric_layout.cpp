#include "ui/MusicLyricLayout.h"
#include <cassert>
#include <cstdio>

int main()
{
    const auto measure = [](const std::string &s) {
        int width = 0;
        for (size_t p = 0; p < s.size(); p = music_lyric_layout::next_char(s, p))
            width += static_cast<unsigned char>(s[p]) < 128 ? 10 : 36;
        return width;
    };
    auto lines = music_lyric_layout::wrap("这是很长的歌词不能省略", 108, measure);
    std::string joined;
    for (const auto &line : lines) { assert(measure(line) <= 108); joined += line; }
    assert(joined == "这是很长的歌词不能省略");
    lines = music_lyric_layout::wrap("hello world again", 110, measure);
    assert((lines == std::vector<std::string>{"hello world", "again"}));
    lines = music_lyric_layout::wrap("superlongword", 50, measure);
    joined.clear();
    for (const auto &line : lines) { assert(measure(line) <= 50); joined += line; }
    assert(joined == "superlongword");
    lines = music_lyric_layout::wrap("你好\nworld", 110, measure);
    assert((lines == std::vector<std::string>{"你好", "world"}));
    assert(music_lyric_layout::window_start(4, 6, 20, 4, 0) == 3);
    assert(music_lyric_layout::window_start(4, 12, 20, 3, 0) == 4);
    assert(music_lyric_layout::window_start(4, 12, 20, 3, 5000) == 7);
    assert(music_lyric_layout::window_start(4, 12, 20, 3, 10000) == 9);
    assert(music_lyric_layout::window_start(4, 12, 20, 3, 15000) == 4);
    assert(music_lyric_layout::window_start(0, 1, 1, 3, 0) == 0);
    std::puts("Music lyrics: UTF-8 wrap, word wrap, lossless long lines and active windows passed");
}
