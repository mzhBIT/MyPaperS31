#pragma once

#include "ui/UiLocale.h"

#include <cstddef>
#include <functional>
#include <string>

struct TextInputOptions {
    std::string title = ui_locale::text(ui_locale::TextId::TextInputTitle);
    std::string initial_text;
    std::string fixed_suffix;
    std::string submit_label = ui_locale::text(ui_locale::TextId::Done);
    size_t max_bytes = 255;
    bool start_in_chinese = true;
    bool secret = false;
    // Opt-in for callers whose overlay should retain the page's Text refresh.
    bool text_refresh = false;
    bool unified_status_bar = false;
    // Return a localized error to keep editing, or an empty string to accept.
    std::function<std::string(const std::string&)> validate;
};

using TextInputCallback = std::function<void(const std::string &)>;

void launch_text_input(const TextInputOptions &options, TextInputCallback on_submit);
