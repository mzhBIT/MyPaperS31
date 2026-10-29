#pragma once
namespace mypaper::board::s31::standby {
void request();
void request_shutdown();
// UI thread only. Returns true after an entry attempt/wake, so the caller
// restarts its loop with fresh input and activity state.
bool service();
}
