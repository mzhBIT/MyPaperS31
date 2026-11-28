#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xhs {

enum class ImageMode : uint8_t { Gray4, Cdn };
struct Config {
    std::string address, token;
    ImageMode image_mode = ImageMode::Gray4;
    bool ready() const;
};
constexpr size_t address_limit = 300;
constexpr size_t token_limit = 255;
bool valid_address(const std::string &address);
bool valid_token(const std::string &token);
Config load_config();
bool save_config(const Config &config);

struct Media {
    std::string gray4, cdn, jpeg;
};
struct Card {
    std::string id, title, author, likes;
    Media cover;
};
struct Note {
    std::string title, author, likes, body, published_at, ip_location;
    std::vector<Media> images;
};

enum class Action { Health, Open, Refresh, More, Detail, Images };
constexpr size_t batch_size = 6;
struct ImageRequest {
    Media media;
    int width = 0, height = 0;
};
struct Image {
    // The buffer is heap_caps_allocated in PSRAM and freed by free().
    struct Free { void operator()(uint8_t *data) const; };
    std::unique_ptr<uint8_t, Free> pixels;
    int width = 0, height = 0;
};

class Network;
std::shared_ptr<Network> make_network();

struct Task {
    explicit Task(Action operation) : action(operation) {}
    Action action;
    std::string session, note_id;
    std::array<ImageRequest, batch_size> requests;
    size_t image_count = 0;

    // Each completed image has a single producer and a single consumer.
    // Images below images_ready belong to the UI, even before done is set.
    std::atomic<size_t> images_ready{0};
    std::array<Image, batch_size> images;
    std::atomic<bool> cancel{false}, done{false};
    std::string error;
    std::vector<Card> cards;
    Note note;
};

bool start(const std::shared_ptr<Task> &task, const Config &config,
           const std::shared_ptr<Network> &network);

} // namespace xhs
