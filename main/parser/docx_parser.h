#pragma once
#include "memory/MetadataAllocator.h"

#include "parser/epub_zip.h"
#include <string>
#include <vector>

struct DocxSection {
    metadata_memory::String href;
    metadata_memory::String title;
    std::string html;
    int depth = 0;
};

// OOXML -> neutral XHTML, never stores generated layout on the SD card.
class DocxBook {
public:
    bool open(const char *path);
    void close();
    const std::string &title() const { return title_; }
    const std::string &last_error() const { return error_; }
    const metadata_memory::Vector<DocxSection> &sections() const { return sections_; }
    bool read_section(const std::string &href, std::string *out) const;
    bool read_prefix(const std::string &href, std::vector<uint8_t> *out, size_t size);
    bool read_resource(const std::string &href, std::vector<uint8_t> *out, uint32_t size);
    bool extract_resource(const std::string &href, const std::string &path);
private:
    EpubZip zip_;
    std::string title_, error_;
    metadata_memory::Vector<DocxSection> sections_;
    std::string notes_href_, notes_html_;
};
