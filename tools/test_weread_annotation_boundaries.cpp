#include "weread/AnnotationBoundaryIndex.h"
#include "parser/epub_html.h"
#include <cassert>
#include <chrono>
#include <iostream>

// Previous implementation, retained here to verify valid-input equivalence.
static void old_snap(const std::string &html, size_t &start, size_t &end)
{
    if (start >= end || end > html.size()) return;
    auto prev = [&](char c, size_t p) { return p ? html.rfind(c, p-1) : std::string::npos; };
    size_t open = prev('<', start), close = prev('>', start);
    if (open != std::string::npos && (close == std::string::npos || open > close)) {
        close = html.find('>', start);
        if (close == std::string::npos || close+1 >= end) { start=end; return; }
        start=close+1;
    }
    open=prev('<', end); close=prev('>', end);
    if (open != std::string::npos && (close == std::string::npos || open > close)) end=open;
    size_t amp=prev('&', start), semi=prev(';', start);
    if (amp != std::string::npos && (semi == std::string::npos || amp > semi) && start-amp <= 12) {
        semi=html.find(';', start);
        if (semi != std::string::npos && semi+1 < end) start=semi+1;
    }
    amp=prev('&', end); semi=prev(';', end);
    if (amp != std::string::npos && (semi == std::string::npos || amp > semi) && end-amp <= 12) end=amp;
}

int main()
{
    size_t comparisons=0;
    for (const std::string html : {"<p>中文 &amp; &#12345; &#x4e2d; text</p>",
                                  "abc <span class='test'>正文</span> xyz", "plain text", "<unclosed tag"}) {
        weread::AnnotationBoundaryIndex index(html, [] {});
        for (size_t a=0; a<html.size(); ++a) for (size_t b=a+1; b<=html.size(); ++b) {
            size_t old_a=a,old_b=b,new_a=a,new_b=b;
            old_snap(html,old_a,old_b);
            index.snap(&new_a,&new_b);
            assert(old_a==new_a && old_b==new_b);
            ++comparisons;
        }
    }
    // Malformed bare '&' must not make a start jump to a distant semicolon.
    const std::string malformed="abc &"+std::string(10000,'x')+"; tail";
    weread::AnnotationBoundaryIndex malformed_index(malformed, [] {});
    size_t start=6,end=malformed.size();
    malformed_index.snap(&start,&end);
    assert(start==6 && end==malformed.size());

    // Chapter-sized input, 858 comments; check checkpoint cadence and speed.
    const std::string chapter="<p>"+std::string(400000,'x')+"</p>";
    size_t checkpoints=0;
    const auto begin=std::chrono::steady_clock::now();
    weread::AnnotationBoundaryIndex index(chapter, [&] { ++checkpoints; });
    for (size_t i=0;i<858;++i) {
        size_t a=3+i*450,b=a+30;
        index.snap(&a,&b);
        assert(a==3+i*450 && b==a+30);
    }
    assert(checkpoints>90);
    std::cout << "Boundary comparisons=" << comparisons << ", 858-range benchmark="
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count()
              << " us (PC only)\n";

    std::string html="<html><body>";
    for (int i=0;i<858;++i) {
        html+="<p><span class=\"wr-annotation\" data-wr-range=\""+std::to_string(i)+"-"+std::to_string(i+1)+
              "\">正文 &amp; &#x4e2d;</span></p>";
    }
    html+="<p><a href=\"#note1\" epub:type=\"noteref\">[1]</a></p><aside id=\"note1\">脚注内容</aside></body></html>";
    std::vector<EpubContentBlock> blocks;
    assert(epub_parse_xhtml_blocks(html,"chapter.xhtml",&blocks));
    size_t comments=0,notes=0;
    for (const auto &block : blocks) {
        for (const auto &ref : block.footnotes) {
            if (ref.kind==EpubFootnoteRef::Kind::WereadComment) {
                ++comments;
                assert(block.text=="正文 & 中");
                assert(ref.byte_start==0 && ref.byte_end==block.text.size());
            } else if (ref.kind==EpubFootnoteRef::Kind::LinkedFootnote) {
                ++notes;
                assert(ref.content=="脚注内容");
            }
        }
    }
    assert(comments==858 && notes==1);
    // Large text normalizes correctly, including bare ampersands and whitespace.
    assert(epub_parse_xhtml_blocks("<p>"+std::string(20000,'&')+std::string(40,' ')+"A &amp; B </p>","large.xhtml",&blocks));
    assert(blocks.size()==1 && blocks[0].text==std::string(20000,'&')+" A & B");
    std::cout << "Parser: 858 comments, linked footnote and large entity input PASS\n";
}
