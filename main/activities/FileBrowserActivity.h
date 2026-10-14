#pragma once

void launch_file_browser(const char *start_path);
// Only the initial frame uses HD; subsequent browsing keeps Text mode.
void launch_file_browser(const char *start_path, bool initial_hd);
