// Intentionally include this first: the FILE specialization must be available
// without another application header having already included stdio.h.
#include "lgfx/v1/misc/DataWrapper.hpp"
#include <type_traits>

static_assert(std::is_base_of<lgfx::DataWrapperT<FILE>,
                              lgfx::DataWrapperT<void>>::value,
              "default file decoder must use FILE I/O, never the no-op wrapper");

// Optional host smoke test; target builds also compile-check the wrapper API.
int main() {
    FILE* fp = tmpfile();
    if (!fp) return 1;
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (fwrite(png, 1, sizeof(png), fp) != sizeof(png)) { fclose(fp); return 2; }
    lgfx::DataWrapperT<FILE> file(fp);
    if (!file.seek(0)) { file.close(); return 3; }
    unsigned char header[8]{};
    if (file.read(header, sizeof(header)) != sizeof(header)
        || memcmp(header, png, sizeof(header))) { file.close(); return 4; }
    if (!file.seek(1)) { file.close(); return 5; }
    file.skip(-1);
    const bool ok = file.tell() == 0 && file.read8() == 0x89;
    file.close();
    return ok ? 0 : 6;
}
