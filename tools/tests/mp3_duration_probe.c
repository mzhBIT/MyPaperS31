#include "audio/mp3_duration.h"
#include <time.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    FILE *file = _wfopen(argv[1], L"rb");
    if (!file) return 3;
    mp3_duration_info_t info;
    clock_t begin = clock();
    bool tagged = mp3_duration_read_tags(file, &info, NULL, NULL);
    printf("tags-only: ok=%d stage=%s reason=%s frames=%u reads=%u time=%ld ms\n",
           tagged, info.stage ? info.stage : "none", info.failure ? info.failure : "none",
           info.frames_scanned, info.read_count, (long)((clock()-begin)*1000/CLOCKS_PER_SEC));
    begin = clock();
    bool ok = mp3_duration_read(file, &info, NULL, NULL);
    printf("ok=%d duration=%u rate=%u source=%s stage=%s failure=%s first=%ld frames=%u reads=%u bytes=%llu time=%ld ms\n",
           ok, info.duration_ms, info.sample_rate, info.source ? info.source : "none",
           info.stage ? info.stage : "none", info.failure ? info.failure : "none",
           info.first_frame_offset, info.frames_scanned, info.read_count,
           (unsigned long long)info.read_bytes, (long)((clock()-begin)*1000/CLOCKS_PER_SEC));
    fclose(file);
    return ok ? 0 : 1;
}
