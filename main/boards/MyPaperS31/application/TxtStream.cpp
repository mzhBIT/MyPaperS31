#include "TxtStream.h"
#include "parser/gbk_decode.h"
#include <sys/stat.h>

namespace mypaper::board::s31 {
bool TxtStream::open(const std::string& path) {
    close();
    struct stat st = {};
    if (stat(path.c_str(), &st) || st.st_size < 0 || uint64_t(st.st_size) > INT32_MAX)
        return false;
    _file = fopen(path.c_str(), "rb");
    if (!_file) return false;
    _size = st.st_size;
    unsigned char bytes[4096];
    const size_t n = fread(bytes, 1, sizeof(bytes), _file);
    _begin = 0; _encoding = Utf8;
    if (n >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        _encoding = Utf16Le; _begin = 2;
    } else if (n >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
        _encoding = Utf16Be; _begin = 2;
    } else if (n >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
        _begin = 3;
    } else {
        for (size_t i = 0; i < n;) {
            const unsigned b = bytes[i++];
            if (b < 128) continue;
            const unsigned rest = b >= 0xc2 && b <= 0xdf ? 1 :
                                  b <= 0xef && b >= 0xe0 ? 2 :
                                  b >= 0xf0 && b <= 0xf4 ? 3 : 99;
            if (rest == 99) { _encoding = Gbk; break; }
            if (i + rest > n) break; // sample may end within a codepoint
            bool valid = true;
            for (unsigned j = 0; j < rest; ++j)
                valid &= (bytes[i + j] & 0xc0) == 0x80;
            if (!valid) { _encoding = Gbk; break; }
            i += rest;
        }
    }
    return seek(_begin);
}
void TxtStream::close() {
    if (_file) fclose(_file);
    _file = nullptr; _size = _begin = 0;
}
uint32_t TxtStream::tell() const { return _file ? static_cast<uint32_t>(ftell(_file)) : 0; }
bool TxtStream::seek(uint32_t offset) { return _file && offset <= _size && !fseek(_file, offset, SEEK_SET); }
const char* TxtStream::encoding() const {
    return _encoding == Utf8 ? "UTF-8" : _encoding == Gbk ? "GBK" :
           _encoding == Utf16Le ? "UTF-16LE" : "UTF-16BE";
}
bool TxtStream::next(uint32_t& cp) {
    if (!_file) return false;
    const int b = fgetc(_file);
    if (b == EOF) return false;
    cp = b;
    if (_encoding == Utf16Le || _encoding == Utf16Be) {
        const int b2 = fgetc(_file);
        if (b2 == EOF) { cp = 0xfffd; return true; }
        cp = _encoding == Utf16Le ? b | (b2 << 8) : (b << 8) | b2;
        if (cp >= 0xd800 && cp <= 0xdbff) {
            const uint32_t pos = tell();
            const int lo = fgetc(_file), hi = fgetc(_file);
            const uint32_t tail = lo < 0 || hi < 0 ? 0 :
                _encoding == Utf16Le ? lo | (hi << 8) : (lo << 8) | hi;
            if (tail >= 0xdc00 && tail <= 0xdfff)
                cp = 0x10000 + ((cp - 0xd800) << 10) + tail - 0xdc00;
            else { seek(pos); cp = 0xfffd; }
        } else if (cp >= 0xdc00 && cp <= 0xdfff) cp = 0xfffd;
    } else if (_encoding == Gbk && b >= 0x81) {
        const int tail = fgetc(_file);
        cp = tail == EOF ? 0xfffd : gbk_decode_codepoint(b, tail);
        if (!cp) cp = 0xfffd;
    } else if (_encoding == Utf8 && b >= 128) {
        const unsigned count = b >= 0xc2 && b <= 0xdf ? 1 :
                               b >= 0xe0 && b <= 0xef ? 2 :
                               b >= 0xf0 && b <= 0xf4 ? 3 : 0;
        cp = count ? b & ((1u << (6 - count)) - 1) : 0xfffd;
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t pos = tell();
            const int tail = fgetc(_file);
            if (tail < 0 || (tail & 0xc0) != 0x80) {
                if (tail >= 0) seek(pos);
                cp = 0xfffd; return true;
            }
            cp = (cp << 6) | (tail & 63);
        }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
            (count == 3 && cp < 0x10000)) cp = 0xfffd;
    }
    return true;
}
std::string TxtStream::utf8(uint32_t cp) {
    std::string out;
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    } else {
        out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
        out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    }
    return out;
}
}
