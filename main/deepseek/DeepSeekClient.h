#pragma once

#include "deepseek/DeepSeekConfig.h"
#include "deepseek/DeepSeekSessionStore.h"

#include <string>
#include <vector>

struct DeepSeekClientResult {
    bool success = false;
    bool canceled = false;
    int http_status = 0;
    std::string content;
    std::string error;
};

using DeepSeekStreamCallback = bool (*)(void *context, const std::string &chunk);

class DeepSeekClient {
public:
    static DeepSeekClientResult validate(const DeepSeekConfig &config);
    static DeepSeekClientResult chat(const DeepSeekConfig &config,
                                     DeepSeekModel model,
                                     bool thinking,
                                     const std::vector<DeepSeekMessage> &messages,
                                     const volatile bool *cancel_requested = nullptr,
                                     DeepSeekStreamCallback stream_callback = nullptr,
                                     void *stream_context = nullptr);
};
