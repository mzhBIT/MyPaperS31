#pragma once

#include <functional>
#include <string>

using DeepSeekImagePickCallback = std::function<void(const std::string &path)>;

// Opens an SD-card image browser for a V4 Flash vision message.
void launch_deepseek_image_picker(DeepSeekImagePickCallback callback);
