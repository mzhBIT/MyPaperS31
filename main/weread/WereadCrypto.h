#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace weread {

class OwnedBuffer {
public:
    OwnedBuffer() = default;
    ~OwnedBuffer();
    OwnedBuffer(OwnedBuffer &&other) noexcept;
    OwnedBuffer &operator=(OwnedBuffer &&other) noexcept;
    OwnedBuffer(const OwnedBuffer &) = delete;
    OwnedBuffer &operator=(const OwnedBuffer &) = delete;

    uint8_t *data() { return _data; }
    const uint8_t *data() const { return _data; }
    size_t size() const { return _size; }
    explicit operator bool() const { return _data != nullptr; }
    void reset();
    bool copy_from(const uint8_t *data, size_t size);

private:
    friend bool decode_content_shards(const uint8_t *, size_t,
                                      const uint8_t *, size_t,
                                      const uint8_t *, size_t,
                                      OwnedBuffer *, std::string *);
    uint8_t *_data = nullptr;
    size_t _size = 0;
};

std::string md5_hex(const uint8_t *data, size_t size);
std::string url_encode(const std::string &value);
std::string encode_id(const std::string &value);
std::string sign_query(const std::string &query);
std::string sorted_query(const std::vector<std::pair<std::string, std::string>> &params);

std::vector<std::pair<std::string, std::string>> make_content_params(
    const std::string &book_id, const std::string &chapter_uid,
    const std::string &psvts, int64_t unix_time, uint32_t random_value,
    bool style = false, int section_count = 1);

bool decode_content_shards(const uint8_t *shard0, size_t size0,
                           const uint8_t *shard1, size_t size1,
                           const uint8_t *shard2, size_t size2,
                           OwnedBuffer *output, std::string *error = nullptr);

bool crypto_self_test();

}  // namespace weread
