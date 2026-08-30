#pragma once

#include <cstddef>
#include <functional>
#include <string>

struct TextInputOptions {
    std::string title = "Text input";
    std::string initial_text;
    std::string fixed_suffix;
    std::string submit_label = "Done";
    size_t max_bytes = 255;
    bool start_in_chinese = true;
    bool secret = false;
};

using TextInputCallback = std::function<void(const std::string &)>;

void launch_text_input(const TextInputOptions &options, TextInputCallback on_submit);
