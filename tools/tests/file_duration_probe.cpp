#include "audio/file_duration.h"
#include <cstdio>

static bool cancel(void *) { return false; }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 3;
    fseek(file, 3, SEEK_SET);
    uint32_t duration = 123;
    const bool ok = audio_file_duration_read(file, argv[1], &duration,
                                             argc > 2 ? cancel : nullptr, nullptr);
    const bool restored = ftell(file) == 3;
    fclose(file);
    printf("%d %u %d\n", ok, duration, restored);
    return restored ? 0 : 4;
}
