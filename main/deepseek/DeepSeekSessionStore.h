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
};

class DeepSeekSessionStore {
public:
    bool list(std::vector<DeepSeekSessionMeta> *sessions) const;
    bool create(DeepSeekModel model, bool thinking, DeepSeekSessionMeta *session) const;
    bool load_meta(const std::string &id, DeepSeekSessionMeta *session) const;
    bool load_messages(const std::string &id, std::vector<DeepSeekMessage> *messages) const;
    bool append_message(DeepSeekSessionMeta *session,
                        const std::string &role,
                        const std::string &content) const;
    bool set_model(DeepSeekSessionMeta *session, DeepSeekModel model) const;
    bool set_thinking(DeepSeekSessionMeta *session, bool enabled) const;

private:
    bool save_meta(const DeepSeekSessionMeta &session) const;
};
