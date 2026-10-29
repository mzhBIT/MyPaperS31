#pragma once
#include "reader_background.h"
bool standby_wallpaper_enabled();
const char* standby_wallpaper_path();
ReaderBackgroundResult standby_wallpaper_set(const char* path, int width, int height, std::string* message = nullptr);
bool standby_wallpaper_clear();
bool standby_wallpaper_prepare(int width, int height);
bool standby_wallpaper_draw(lgfx::LGFXBase& gfx, int width, int height);
void standby_wallpaper_release();
