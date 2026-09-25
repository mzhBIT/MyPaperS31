#pragma once

#include <functional>
#include <string>

using PhoneRingtonePickCallback = std::function<void(const std::string &path)>;

void launch_phone_ringtone_picker(PhoneRingtonePickCallback callback);
