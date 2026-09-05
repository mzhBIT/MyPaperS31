#include "weread/WereadCrypto.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <mbedtls/base64.h>
#include <mbedtls/md5.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace weread {
namespace {

constexpr const char *TAG = "WEREAD_CRYPTO";

bool is_decimal(const std::string &value)
{
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

std::string byte_hex(const std::string &value)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() * 2);
    for (uint8_t byte : value) {
        if (byte >= 16) output.push_back(HEX[byte >> 4]);
        output.push_back(HEX[byte & 0x0F]);
    }
    return output;
}

std::vector<int> swap_positions(const uint8_t *encoded, int length)
{
    std::vector<int> result;
    if (!encoded || length < 4) return result;
    if (length < 11) return {0, 2};

    const int tail = std::min(4, (length + 9) / 10);
    std::string digits;
    for (int i = length - 1; i >= length - tail; --i) {
        uint32_t value = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((encoded[i] >> bit) & 1U) value += 1U << (bit * 2);
        }
        digits += std::to_string(value);
    }

    const int modulo = length - tail - 2;
    if (modulo <= 0) return result;
    const int step = static_cast<int>(std::to_string(modulo).size());
    for (int i = 0; result.size() < 10 && i + step < static_cast<int>(digits.size()); i += step) {
        result.push_back(std::stoi(digits.substr(i, step)) % modulo);
        result.push_back(std::stoi(digits.substr(i + 1, step)) % modulo);
    }
    return result;
}

bool append_checked_body(const uint8_t *shard, size_t size, uint8_t *destination,
                         size_t capacity, size_t *written)
{
    if (!shard || size <= 32) return true;
    std::string expected(reinterpret_cast<const char *>(shard), 32);
    std::string actual = md5_hex(shard + 32, size - 32);
    std::transform(actual.begin(), actual.end(), actual.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (expected != actual) return false;
    const size_t body_size = size - 32;
    if (*written > capacity || body_size > capacity - *written) return false;
    std::memcpy(destination + *written, shard + 32, body_size);
    *written += body_size;
    return true;
}

}  // namespace

OwnedBuffer::~OwnedBuffer() { reset(); }

OwnedBuffer::OwnedBuffer(OwnedBuffer &&other) noexcept
    : _data(other._data), _size(other._size)
{
    other._data = nullptr;
    other._size = 0;
}

OwnedBuffer &OwnedBuffer::operator=(OwnedBuffer &&other) noexcept
{
    if (this != &other) {
        reset();
        _data = other._data;
        _size = other._size;
        other._data = nullptr;
        other._size = 0;
    }
    return *this;
}

void OwnedBuffer::reset()
{
    if (_data) heap_caps_free(_data);
    _data = nullptr;
    _size = 0;
}

bool OwnedBuffer::copy_from(const uint8_t *data, size_t size)
{
    reset();
    if ((!data && size) || !size) return false;
    uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM));
    if (!copy) return false;
    std::memcpy(copy, data, size);
    copy[size] = 0;
    _data = copy;
    _size = size;
    return true;
}

std::string md5_hex(const uint8_t *data, size_t size)
{
    uint8_t digest[16] = {};
    if (!data && size) return {};
    if (mbedtls_md5(data, size, digest) != 0) return {};
    char text[33] = {};
    for (int i = 0; i < 16; ++i) std::snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}

std::string url_encode(const std::string &value)
{
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() + value.size() / 4);
    for (uint8_t c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output.push_back(static_cast<char>(c));
        } else {
            output.push_back('%');
            output.push_back(HEX[c >> 4]);
            output.push_back(HEX[c & 0x0F]);
        }
    }
    return output;
}

std::string encode_id(const std::string &value)
{
    const std::string hash = md5_hex(reinterpret_cast<const uint8_t *>(value.data()), value.size());
    if (hash.size() != 32) return {};

    std::vector<std::string> chunks;
    const bool decimal = is_decimal(value);
    if (decimal) {
        for (size_t i = 0; i < value.size(); i += 9) {
            const std::string part = value.substr(i, 9);
            unsigned long long number = std::strtoull(part.c_str(), nullptr, 10);
            char text[24] = {};
            std::snprintf(text, sizeof(text), "%llx", number);
            chunks.emplace_back(text);
        }
    } else {
        chunks.push_back(byte_hex(value));
    }

    std::string result = hash.substr(0, 3);
    result += decimal ? "32" : "42";
    result += hash.substr(30, 2);
    for (size_t i = 0; i < chunks.size(); ++i) {
        char length[4] = {};
        std::snprintf(length, sizeof(length), "%02x", static_cast<unsigned>(chunks[i].size()));
        result += length;
        result += chunks[i];
        if (i + 1 < chunks.size()) result.push_back('g');
    }
    if (result.size() < 20) result += hash.substr(0, 20 - result.size());
    result += md5_hex(reinterpret_cast<const uint8_t *>(result.data()), result.size()).substr(0, 3);
    return result;
}

std::string sign_query(const std::string &query)
{
    int64_t a = 0x15051505;
    int64_t b = a;
    int i = static_cast<int>(query.size());
    const int length = i;
    while (i > 1) {
        a = (a ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 1])) << ((length - i + 1) % 30))) & 0x7fffffff;
        b = (b ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 2])) << ((i - 1) % 30))) & 0x7fffffff;
        i -= 2;
    }
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(a + b));
    return text;
}

std::string sorted_query(const std::vector<std::pair<std::string, std::string>> &params)
{
    auto sorted = params;
    std::sort(sorted.begin(), sorted.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });
    std::string output;
    for (const auto &param : sorted) {
        if (param.first == "s") continue;
        if (!output.empty()) output.push_back('&');
        output += param.first;
        output.push_back('=');
        output += url_encode(param.second);
    }
    return output;
}

std::vector<std::pair<std::string, std::string>> make_content_params(
    const std::string &book_id, const std::string &chapter_uid,
    const std::string &psvts, int64_t unix_time, uint32_t random_value,
    bool style, int section_count)
{
    if (encode_id(std::to_string(unix_time)) == psvts) ++unix_time;
    const uint64_t random_squared = static_cast<uint64_t>(random_value % 10000U) *
                                    static_cast<uint64_t>(random_value % 10000U);
    std::vector<std::pair<std::string, std::string>> params = {
        {"b", encode_id(book_id)}, {"c", encode_id(chapter_uid)},
        {"r", std::to_string(random_squared)}, {"ct", std::to_string(unix_time)},
        {"ps", psvts}, {"pc", encode_id(std::to_string(unix_time))},
        {"sc", std::to_string(section_count)}, {"prevChapter", "false"},
        {"st", style ? "1" : "0"},
    };
    params.emplace_back("s", sign_query(sorted_query(params)));
    return params;
}

bool decode_content_shards(const uint8_t *shard0, size_t size0,
                           const uint8_t *shard1, size_t size1,
                           const uint8_t *shard2, size_t size2,
                           OwnedBuffer *output, std::string *error)
{
    if (!output) return false;
    output->reset();
    const size_t capacity = size0 + size1 + size2;
    if (capacity <= 33) {
        if (error) *error = "empty shard payload";
        return false;
    }
    uint8_t *work = static_cast<uint8_t *>(heap_caps_malloc(capacity + 4, MALLOC_CAP_SPIRAM));
    if (!work) {
        if (error) *error = "PSRAM allocation failed";
        return false;
    }
    size_t payload_size = 0;
    if (!append_checked_body(shard0, size0, work, capacity, &payload_size) ||
        !append_checked_body(shard1, size1, work, capacity, &payload_size) ||
        !append_checked_body(shard2, size2, work, capacity, &payload_size)) {
        heap_caps_free(work);
        if (error) *error = "shard MD5 mismatch";
        return false;
    }
    if (payload_size <= 1) {
        heap_caps_free(work);
        if (error) *error = "empty encoded content";
        return false;
    }

    uint8_t *encoded = work + 1;
    const int encoded_size = static_cast<int>(payload_size - 1);
    const auto positions = swap_positions(encoded, encoded_size);
    for (int i = static_cast<int>(positions.size()) - 1; i > 0; i -= 2) {
        for (int offset = 1; offset >= 0; --offset) {
            const int left = positions[i] + offset;
            const int right = positions[i - 1] + offset;
            if (left >= 0 && right >= 0 && left < encoded_size && right < encoded_size)
                std::swap(encoded[left], encoded[right]);
        }
    }

    size_t filtered = 0;
    for (int i = 0; i < encoded_size; ++i) {
        uint8_t c = encoded[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        if (std::isalnum(c) || c == '+' || c == '/') encoded[filtered++] = c;
    }
    while (filtered % 4) encoded[filtered++] = '=';

    size_t required = 0;
    int decode_result = mbedtls_base64_decode(nullptr, 0, &required, encoded, filtered);
    if (decode_result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || !required) {
        heap_caps_free(work);
        if (error) *error = "invalid base64 content";
        return false;
    }
    uint8_t *decoded = static_cast<uint8_t *>(heap_caps_malloc(required + 1, MALLOC_CAP_SPIRAM));
    if (!decoded) {
        heap_caps_free(work);
        if (error) *error = "decoded PSRAM allocation failed";
        return false;
    }
    size_t written = 0;
    decode_result = mbedtls_base64_decode(decoded, required, &written, encoded, filtered);
    heap_caps_free(work);
    if (decode_result != 0) {
        heap_caps_free(decoded);
        if (error) *error = "base64 decode failed";
        return false;
    }
    decoded[written] = 0;
    output->_data = decoded;
    output->_size = written;
    return true;
}

bool crypto_self_test()
{
    const bool ok = md5_hex(reinterpret_cast<const uint8_t *>("abc"), 3) ==
                        "900150983cd24fb0d6963f7d28e17f72" &&
                    encode_id("abc") == "900427206616263900156d7" &&
                    encode_id("1234567890") == "e80329f0775bcd15g0109e5" &&
                    encode_id("1001") == "b8c32da033e9b8c37e3337b" &&
                    sign_query("b=abc&c=123&pc=x") == "2a2ad256";
    if (!ok) ESP_LOGE(TAG, "protocol primitive self-test failed");
    return ok;
}

}  // namespace weread
