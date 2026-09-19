#pragma once

#include "deepseek/DeepSeekConfig.h"

#include <cstdint>
#include <string>
#include <vector>

struct DeepSeekSessionMeta {
    std::string id;
    std::string title;
    DeepSeekModel model = DeepSeekModel::Flash;
    bool thinking = false;
    uint64_t created = 0;
    uint64_t updated = 0;
};

struct DeepSeekMessage {
    std::string role;
    std::string content;
    // Local SD-card path. The API client encodes this only while sending; the
    // image data is deliberately never copied into messages.jsonl.
    std::string image_path;
};

class DeepSeekSessionStore {
public:
    bool list(std::vector<DeepSeekSessionMeta> *sessions) const;
    bool create(DeepSeekModel model, bool thinking, DeepSeekSessionMeta *session) const;
    bool load_meta(const std::string &id, DeepSeekSessionMeta *session) const;
    bool load_messages(const std::string &id, std::vector<DeepSeekMessage> *messages) const;
    bool remove(const std::string &id) const;
    bool append_message(DeepSeekSessionMeta *session,
                        const std::string &role,
                        const std::string &content,
                        const std::string &image_path = {}) const;
    bool set_model(DeepSeekSessionMeta *session, DeepSeekModel model) const;
    bool set_thinking(DeepSeekSessionMeta *session, bool enabled) const;

private:
    bool save_meta(const DeepSeekSessionMeta &session) const;
};
