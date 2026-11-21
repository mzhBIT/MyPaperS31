// Native regression: compile this file with main/parser/epub_html.cpp, -I main.
#include "parser/epub_html.h"
#include <cassert>
#include <cstdio>

static std::vector<EpubContentBlock> parse(const std::string &html, bool inline_sup = false)
{
    std::vector<EpubContentBlock> blocks;
    assert(epub_parse_xhtml_blocks(html, "OEBPS/Text/chapter.xhtml", &blocks, inline_sup));
    return blocks;
}

static std::vector<EpubFootnoteRef> references(const std::vector<EpubContentBlock> &blocks)
{
    std::vector<EpubFootnoteRef> refs;
    for (const auto &block : blocks)
        for (const auto &ref : block.footnotes) {
            assert(ref.byte_start < ref.byte_end && ref.byte_end <= block.text.size());
            refs.push_back(ref);
        }
    return refs;
}

int main()
{
    const auto headings = parse("<h1>Chapter 1&middot;Title</h1>");
    assert(headings.size() == 1 && headings[0].heading);
    assert(headings[0].text == "Chapter 1\xC2\xB7" "Title");

    std::vector<EpubContentBlock> styled;
    assert(epub_parse_xhtml_blocks("<h2>A<strong>B<em>C</em></strong><i>D</i></h2><p>E<b>F</b>G</p>",
                                   "chapter.xhtml", &styled, false, true));
    assert(styled.size() == 2 && styled[0].heading && styled[0].text == "ABCD");
    const auto &title_styles = styled[0].styles;
    assert(title_styles.size() == 3);
    assert(title_styles[0].byte_start == 1 && title_styles[0].byte_end == 2 &&
           title_styles[0].flags == inline_style::Bold);
    assert(title_styles[1].byte_start == 2 && title_styles[1].byte_end == 3 &&
           title_styles[1].flags == (inline_style::Bold | inline_style::Italic));
    assert(title_styles[2].byte_start == 3 && title_styles[2].byte_end == 4 &&
           title_styles[2].flags == inline_style::Italic);
    assert(styled[1].text == "EFG" && styled[1].styles.size() == 1);
    assert(styled[1].styles[0].byte_start == 1 && styled[1].styles[0].byte_end == 2 &&
           styled[1].styles[0].flags == inline_style::Bold);

    // Duokan: the image alt describes the marker, while the linked li is the
    // actual footnote. The previous parser returned the literal word "note".
    auto blocks = parse(
        "<p>Body<a class='duokan-footnote' epub:type='noteref' href='#n'>"
        "<img alt='note' src='../Images/note.png'/></a></p>"
        "<aside epub:type='footnote'><ol><li id='n'><p>Actual footnote text.</p></li></ol></aside>");
    auto refs = references(blocks);
    assert(refs.size() == 1 && refs[0].content == "Actual footnote text.");
    assert(refs[0].target == "OEBPS/Text/chapter.xhtml#n");
    for (const auto &block : blocks) assert(block.text.find("Actual footnote text.") == std::string::npos);

    // New source names and cross-file definitions must retain lazy resolution.
    for (const char *label : {"note", " Note ", "footnote", "[3]", "脚注", "註釋"}) {
        blocks = parse(std::string("<p>Body<a role='doc-noteref' href='../Notes/foot.xhtml#n'>") +
                       "<img alt='" + label + "' src='../Images/icon.svg'/></a></p>");
        refs = references(blocks);
        assert(refs.size() == 1 && refs[0].content.empty());
        assert(refs[0].target == "OEBPS/Notes/foot.xhtml#n");
        std::string content;
        assert(epub_resolve_footnote("<aside id='n'><p>Cross-file note.</p></aside>",
                                    "OEBPS/Notes/foot.xhtml", refs[0].target, &content));
        assert(content == "Cross-file note.");
    }

    // Explicit payloads take precedence over image accessibility labels. Keep
    // the legitimate one-word body "note" when explicitly stored as data.
    refs = references(parse("<p>Body<img class='qqreader-footnote' alt='note' title='icon' "
                            "data-content='Payload &amp; details' src='note.png'/></p>"));
    assert(refs.size() == 1 && refs[0].content == "Payload & details");
    refs = references(parse("<p>Body<img class='footnote-icon' alt='note' data-content=' ' "
                            "data-note='Alternative payload' src='note.png'/></p>"));
    assert(refs.size() == 1 && refs[0].content == "Alternative payload");
    refs = references(parse("<p>Body<img class='footnote-icon' data-content='note' "
                            "src='note.png'/></p>"));
    assert(refs.size() == 1 && refs[0].content == "note");

    // Existing inline title and descriptive alt notes continue to work.
    refs = references(parse("<p>Body<img class='qqreader-footnote' alt='note' "
                            "title='Inline title note' src='note.png'/></p>"));
    assert(refs.size() == 1 && refs[0].content == "Inline title note");
    refs = references(parse("<p>Body<img class='qqreader-footnote' "
                            "alt='Existing inline alt note' src='note.png'/></p>"));
    assert(refs.size() == 1 && refs[0].content == "Existing inline alt note");

    // A missing definition must not be reported as successfully resolved to
    // the icon's name, and regular images/links are never converted to notes.
    refs = references(parse("<p>Body<a epub:type='noteref' href='#missing'>"
                            "<img alt='note' src='note.png'/></a></p>"));
    assert(refs.size() == 1 && refs[0].content.empty());
    std::string content;
    assert(!epub_resolve_footnote("<p>Body</p>", "OEBPS/Text/chapter.xhtml", refs[0].target, &content));
    blocks = parse("<p>Body<img alt='note' src='photo.jpg'/></p>"
                   "<a href='other.xhtml#section'><img src='diagram.png'/></a>");
    assert(references(blocks).empty());
    size_t images = 0;
    for (const auto &block : blocks) if (block.type == EpubContentBlockType::Image) ++images;
    assert(images == 2);

    refs = references(parse("<p><ruby>base<rt>reading</rt></ruby><sup>commentary</sup></p>", true));
    assert(refs.size() == 2 && refs[0].kind == EpubFootnoteRef::Kind::RubyText &&
           refs[1].kind == EpubFootnoteRef::Kind::InlineSup);
    puts("EPUB footnotes PASS: linked icon labels, cross-file targets, explicit payloads, title/alt fallback, missing notes, images, ruby and inline commentary");
}
