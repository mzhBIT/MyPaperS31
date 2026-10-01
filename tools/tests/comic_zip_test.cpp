// Tests production ComicZip + EpubZip + miniz + XHTML, not a mock ZIP reader.
#include "parser/comic_zip.h"
#include "parser/epub_html.h"
#include <cassert>
#include <cstdio>
#include <direct.h>

namespace app_paths {
const char *epub_metadata_dir() { return "tmp/comic-tests/index"; }
void ensure_dir(const char *path) { _mkdir(path); }
}

int main() {
    ComicZip book;
    assert(book.open("tmp/comic-tests/normal.cbz"));
    const char *expected[] = {"book/1.jpg", "book/2.JPEG", "book/02.png", "book/3# & ?.png",
                             "book/10.bmp", "part2/01.jpg", "part10/01.png"};
    assert(book.pages().size() == 7);
    for (size_t i = 0; i < book.pages().size(); ++i) {
        const auto &page = book.pages()[i];
        assert(page.title == expected[i]);
        std::string html;
        assert(book.read_page(page.href, &html));
        std::vector<EpubContentBlock> blocks;
        assert(epub_parse_xhtml_blocks(html, page.href, &blocks));
        assert(blocks.size() == 1 && blocks[0].type == EpubContentBlockType::Image);
        assert(blocks[0].href == page.image_href);
        std::vector<uint8_t> bytes;
        assert(book.read_resource(page.image_href, &bytes, 4096));
        const std::string raw(bytes.begin(), bytes.end());
        std::string wanted;
        for (int n = 0; n < 100; ++n) wanted += expected[i];
        assert(raw == wanted);
        assert(book.read_prefix(page.image_href, &bytes, 5) && bytes.size() == 5);
        assert(!book.read_resource(page.image_href, &bytes, 1));
    }
    std::string html;
    assert(!book.read_page("missing", &html));
    std::vector<uint8_t> bytes;
    assert(!book.read_resource("../../secret.png", &bytes, 100));
    // Reopen through persisted ZIP index, including flags and compression methods.
    assert(book.open("tmp/comic-tests/normal.cbz") && book.pages().size() == 7);
    assert(!book.open("tmp/comic-tests/empty.zip"));
    assert(book.last_error().find("no JPG") != std::string::npos);
    assert(!book.open("tmp/comic-tests/encrypted.zip"));
    assert(book.last_error().find("encrypted") != std::string::npos);
    EpubZip zip;
    assert(zip.open("tmp/comic-tests/encrypted.zip"));
    assert(!zip.read_entry_to_memory("1.png", &bytes));
    assert(!book.open("tmp/comic-tests/unsupported.zip"));
    assert(!book.open("tmp/comic-tests/zip64.zip"));
    assert(book.pages().empty());
    assert(!book.open("tmp/comic-tests/oversize.zip"));
    assert(book.last_error().find("12 MiB") != std::string::npos);
    assert(book.open("tmp/comic-tests/numbers.zip"));
    const char *numbers[] = {"9.jpg", "10.jpg", "00010.jpg", "9999999999999999999.jpg", "10000000000000000000.jpg"};
    assert(book.pages().size() == 5);
    for (size_t i = 0; i < 5; ++i) assert(book.pages()[i].title == numbers[i]);
    std::puts("Comic ZIP integration tests passed (stored/deflate/cache/sort/aliases/errors).");
}
