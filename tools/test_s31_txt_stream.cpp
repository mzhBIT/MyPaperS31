#include "boards/MyPaperS31/application/TxtStream.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <vector>
using mypaper::board::s31::TxtStream;

static void check(const std::filesystem::path& root, const char* name,
                  std::initializer_list<unsigned char> bytes,
                  const std::vector<uint32_t>& expected, const char* encoding) {
    const auto path = root / name;
    FILE* f = fopen(path.string().c_str(), "wb");
    assert(f);
    for (auto b : bytes) assert(fputc(b, f) != EOF);
    fclose(f);
    TxtStream stream;
    assert(stream.open(path.string()));
    assert(std::string(stream.encoding()) == encoding);
    const auto start = stream.begin();
    for (auto value : expected) {
        uint32_t cp = 0;
        const auto at = stream.tell();
        assert(stream.next(cp) && cp == value);
        const auto after = stream.tell();
        assert(after > at);
        assert(stream.seek(at));
        assert(stream.next(cp) && cp == value && stream.tell() == after);
    }
    uint32_t cp;
    assert(!stream.next(cp));
    assert(stream.seek(start));
    stream.close();
    assert(!stream.next(cp));
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    check(root, "utf8.txt", {0xef,0xbb,0xbf,0x41,0xe4,0xb8,0xad,0x0a},
          {0x41,0x4e2d,10}, "UTF-8");
    check(root, "gbk.txt", {0xd6,0xd0,0xce,0xc4}, {0x4e2d,0x6587}, "GBK");
    check(root, "le.txt", {0xff,0xfe,0x2d,0x4e,0x3d,0xd8,0x00,0xde},
          {0x4e2d,0x1f600}, "UTF-16LE");
    check(root, "be.txt", {0xfe,0xff,0x4e,0x2d,0xd8,0x3d,0xde,0x00},
          {0x4e2d,0x1f600}, "UTF-16BE");
    check(root, "invalid.txt", {0xef,0xbb,0xbf,0xe4,0x41,0xc0,0xaf},
          {0xfffd,0x41,0xfffd,0xfffd}, "UTF-8");
    check(root, "truncated.txt", {0xff,0xfe,0x3d,0xd8,0x41,0x00,0x42},
          {0xfffd,0x41,0xfffd}, "UTF-16LE");
    check(root, "empty.txt", {}, {}, "UTF-8");
    TxtStream stream;
    assert(!stream.open((root / "missing.txt").string()));
    assert(TxtStream::utf8(0x1f600) == "\xf0\x9f\x98\x80");
    puts("S31 TXT stream: UTF-8/GBK/UTF-16, offsets, malformed input, EOF PASS");
}
