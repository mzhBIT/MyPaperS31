#pragma once
#include <cstdio>
#include <cstdint>
#include <string>

namespace mypaper::board::s31 {
// Bounded streaming decoder; never loads an entire book into RAM.
class TxtStream {
public:
    TxtStream() = default;
    TxtStream(const TxtStream&) = delete;
    TxtStream& operator=(const TxtStream&) = delete;
    ~TxtStream() { close(); }
    bool open(const std::string& path);
    void close();
    bool next(uint32_t& cp);
    uint32_t tell() const;
    bool seek(uint32_t offset);
    uint32_t size() const { return _size; }
    uint32_t begin() const { return _begin; }
    const char* encoding() const;
    static std::string utf8(uint32_t cp);
private:
    FILE* _file = nullptr;
    uint32_t _size = 0, _begin = 0;
    enum Encoding { Utf8, Gbk, Utf16Le, Utf16Be } _encoding = Utf8;
};
}
