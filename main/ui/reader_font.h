#pragma once

#include "ui/ReaderProfile.h"

void reader_font_init();
const char *reader_font_path(ReaderProfile profile = ReaderProfile::Shared);
const char *reader_font_default_path();
bool reader_font_set(const char *path, ReaderProfile profile = ReaderProfile::Shared);
