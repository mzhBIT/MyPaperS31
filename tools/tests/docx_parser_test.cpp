// Host regression tests exercise production DocxBook and XHTML parsing.
// ZIP transport is mocked; archive/inflate code is unchanged from EPUB.
#include "parser/docx_parser.h"
#include "parser/epub_html.h"
#include <cassert>
#include <cstdio>
#include <unordered_map>

static std::unordered_map<std::string, std::string> package;
bool EpubZip::open(const char *path) { _path = path; return _path != "encrypted.docx"; }
void EpubZip::close() { _path.clear(); }
bool EpubZip::entry_info(const std::string &name, Entry *out) const {
    auto it = package.find(name);
    if (it == package.end()) return false;
    *out = {}; out->uncompressed_size = (uint32_t)it->second.size(); return true;
}
bool EpubZip::read_entry_to_string(const std::string &name, std::string *out, bool) {
    auto it = package.find(name);
    if (it == package.end()) return false;
    *out = it->second; return true;
}
bool EpubZip::read_entry_prefix(const std::string &name, std::vector<uint8_t> *out, size_t size) {
    auto it = package.find(name);
    if (it == package.end()) return false;
    out->assign(it->second.begin(), it->second.begin() + std::min(size, it->second.size())); return true;
}
bool EpubZip::read_entry_to_memory(const std::string &name, std::vector<uint8_t> *out, uint32_t size) {
    auto it = package.find(name);
    return it != package.end() && it->second.size() <= size && read_entry_prefix(name, out, size);
}
bool EpubZip::extract_entry_to_file(const std::string &, const std::string &, uint32_t) { return false; }

static std::string doc(const std::string &body) {
    return "<w:document><w:body>" + body + "</w:body></w:document>";
}
static std::string para(const std::string &text) {
    return "<w:p><w:r><w:t>" + text + "</w:t></w:r></w:p>";
}
int main() {
    package["_rels/.rels"] = "<Relationships><Relationship Id='main' Type='x/officeDocument' Target='word/document.xml'/><Relationship Id='core' Type='x/core-properties' Target='docProps/core.xml'/></Relationships>";
    package["docProps/core.xml"] = "<cp:coreProperties><dc:title>中文 &amp; English</dc:title></cp:coreProperties>";
    package["word/_rels/document.xml.rels"] = "<Relationships><!-- > --><Relationship Id='pic' Type='x/image' Target='media/test.png'/><Relationship Id='webpic' Type='x/image' TargetMode='External' Target='https://example.org/p.png'/><Relationship Id='sty' Type='x/styles' Target='styles.xml'/><Relationship Id='notes' Type='x/footnotes' Target='footnotes.xml'/><Relationship Id='end' Type='x/endnotes' Target='endnotes.xml'/></Relationships>";
    package["word/styles.xml"] = "<w:styles><w:style w:styleId='H'><w:pPr><w:outlineLvl w:val='0'/></w:pPr><w:rPr><w:b/></w:rPr></w:style><w:style w:styleId='child'><w:basedOn w:val='H'/></w:style></w:styles>";
    package["word/document.xml"] = doc(
        "<w:p><w:pPr><w:pStyle w:val='child'/></w:pPr><w:r><w:t>第一章</w:t></w:r></w:p>"
        "<w:p><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>粗斜体 &amp; &#20013;</w:t></w:r>"
        "<w:r><w:rPr><w:b w:val='0'/></w:rPr><w:t>常规</w:t></w:r>"
        "<w:del><w:r><w:delText>删除内容</w:delText></w:r></w:del>"
        "<w:r><w:footnoteReference w:id='1'/><w:endnoteReference w:id='2'/></w:r></w:p>"
        "<w:p><w:r><w:drawing><a:blip r:embed='pic'/></w:drawing></w:r></w:p>"
        "<w:p><w:r><a:blip r:link='webpic'/></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:numId w:val='1'/></w:numPr></w:pPr><w:r><w:t>列表项</w:t></w:r></w:p>"
        "<w:tbl><w:tr><w:tc>" + para("单元格A") + "</w:tc><w:tc>" + para("单元格B") + "</w:tc></w:tr></w:tbl>"
        "<w:p><w:pPr><w:pStyle w:val='Heading2'/></w:pPr><w:r><w:t>第二节</w:t></w:r></w:p>" + para("正文末尾"));
    package["word/footnotes.xml"] = "<w:footnotes><w:footnote w:id='-1'>" + para("分隔线") + "</w:footnote><w:footnote w:id='1'>" + para("脚注内容") + "</w:footnote></w:footnotes>";
    package["word/endnotes.xml"] = "<w:endnotes><w:endnote w:id='2'>" + para("尾注内容") + "</w:endnote></w:endnotes>";
    package["word/media/test.png"] = "PNG bytes";
    DocxBook book;
    assert(book.open("sample.docx"));
    assert(book.title() == "中文 & English");
    assert(book.sections().size() == 2);
    assert(book.sections()[0].title == "第一章" && book.sections()[1].depth == 1);
    std::vector<EpubContentBlock> blocks;
    // Other formats keep the old XHTML behavior unless explicitly opted in.
    assert(epub_parse_xhtml_blocks(book.sections()[0].html, book.sections()[0].href, &blocks));
    for (const auto &b : blocks) assert(b.styles.empty());
    assert(epub_parse_xhtml_blocks(book.sections()[0].html, book.sections()[0].href, &blocks, false, true));
    bool image = false, style = false, reference = false;
    std::string text;
    for (const auto &b : blocks) {
        text += b.text;
        if (b.type == EpubContentBlockType::Image) { assert(b.href == "word/media/test.png"); image = true; }
        for (const auto &s : b.styles) if ((s.flags & (inline_style::Bold | inline_style::Italic)) == (inline_style::Bold | inline_style::Italic)) style = true;
        for (const auto &f : b.footnotes) {
            std::string html, note;
            const size_t hash = f.target.find('#');
            assert(book.read_section(f.target.substr(0, hash), &html));
            assert(epub_resolve_footnote(html, f.target.substr(0, hash), f.target, &note));
            assert(note == "脚注内容" || note == "尾注内容");
            reference = true;
        }
    }
    if (!image || !style || !reference) {
        std::fprintf(stderr, "image=%d style=%d reference=%d html=%s\n", image, style, reference, book.sections()[0].html.c_str());
        for (const auto &b : blocks) std::fprintf(stderr, "block type=%d text=%s href=%s styles=%zu notes=%zu\n", (int)b.type, b.text.c_str(), b.href.c_str(), b.styles.size(), b.footnotes.size());
    }
    assert(image && style && reference);
    assert(text.find("粗斜体 & 中") != std::string::npos);
    assert(text.find("删除内容") == std::string::npos);
    assert(text.find("单元格A") < text.find("单元格B"));
    assert(text.find("列表项") != std::string::npos);
    std::vector<uint8_t> bytes;
    assert(book.read_resource("word/media/test.png", &bytes, 100));
    assert(!book.read_resource("word/media/test.png", &bytes, 1));
    assert(book.read_prefix("word/media/test.png", &bytes, 3) && bytes.size() == 3);
    // Explicit formatting-off overrides inherited paragraph formatting.
    package["word/document.xml"] = doc("<w:p><w:pPr><w:pStyle w:val='H'/></w:pPr><w:r><w:rPr><w:b w:val='0'/></w:rPr><w:t>not bold</w:t></w:r></w:p>");
    assert(book.open("off.docx"));
    assert(book.sections()[0].html.find("<strong>") == std::string::npos);
    // Bounded sections retain every paragraph; no generated pages are stored.
    std::string long_body;
    for (int i = 0; i < 1000; ++i) long_body += para(std::string(100, 'x'));
    package["word/document.xml"] = doc(long_body);
    assert(book.open("large.docx") && book.sections().size() > 1);
    size_t characters = 0;
    for (const auto &section : book.sections()) {
        blocks.clear(); epub_parse_xhtml_blocks(section.html, section.href, &blocks);
        for (const auto &b : blocks) characters += b.text.size();
    }
    assert(characters == 100000);
    package["word/document.xml"] = "<w:document><w:p><w:r><w:t>truncated";
    assert(!book.open("broken.docx"));
    package["word/document.xml"] = "<w:document>" + para("unclosed document");
    assert(!book.open("unclosed.docx"));
    package["word/document.xml"] = "<w:document><w:p><w:r><w:t>bad</w:r></w:t></w:p></w:document>";
    assert(!book.open("misnested.docx"));
    package["word/document.xml"] = std::string(8 * 1024 * 1024 + 1, 'x');
    assert(!book.open("oversize.docx"));
    assert(!book.open("encrypted.docx"));
    assert(book.sections().empty());
    std::puts("DOCX parser regression tests passed.");
}
