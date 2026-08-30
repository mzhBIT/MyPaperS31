#include "audio/embedded_cover.h"

#include "storage/app_paths.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <sys/stat.h>

namespace embedded_cover {
namespace {

constexpr const char *TAG = "AUDIO_COVER";
constexpr uint32_t MAX_COVER_BYTES = 8 * 1024 * 1024;
constexpr int MAX_MP4_DEPTH = 8;

struct Candidate {
    uint64_t offset = 0;
    uint32_t size = 0;
    uint32_t picture_type = 0;
    std::string extension;
};

uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t synchsafe32(const uint8_t *p)
{
    return (uint32_t(p[0] & 0x7f) << 21) | (uint32_t(p[1] & 0x7f) << 14) |
           (uint32_t(p[2] & 0x7f) << 7) | uint32_t(p[3] & 0x7f);
}

bool read_exact(FILE *file, void *data, size_t size)
{
    return size == 0 || fread(data, 1, size, file) == size;
}

bool seek_abs(FILE *file, uint64_t offset)
{
    return offset <= LONG_MAX && fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
}

bool read_be32(FILE *file, uint32_t *value)
{
    uint8_t bytes[4] = {};
    if (!read_exact(file, bytes, sizeof(bytes))) return false;
    *value = be32(bytes);
    return true;
}

std::string lower_extension(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : nullptr;
    std::string extension = dot ? dot : "";
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension;
}

std::string image_extension(FILE *file, uint64_t offset, const std::string &mime = {})
{
    std::string lower = mime;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("jpeg") != std::string::npos || lower.find("jpg") != std::string::npos) {
        return ".jpg";
    }
    if (lower.find("png") != std::string::npos) return ".png";

    uint8_t signature[8] = {};
    if (!seek_abs(file, offset) || !read_exact(file, signature, sizeof(signature))) return {};
    if (signature[0] == 0xff && signature[1] == 0xd8) return ".jpg";
    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (memcmp(signature, png, sizeof(png)) == 0) return ".png";
    return {};
}

uint64_t hash64_bytes(const void *data, size_t size, uint64_t hash = 1469598103934665603ull)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string cache_dir_for(const char *path)
{
    struct stat st = {};
    if (stat(path, &st) != 0) return {};
    uint64_t hash = hash64_bytes(path, strlen(path));
    hash = hash64_bytes(&st.st_size, sizeof(st.st_size), hash);
    hash = hash64_bytes(&st.st_mtime, sizeof(st.st_mtime), hash);
    char name[32] = {};
    snprintf(name, sizeof(name), "/%016llx", static_cast<unsigned long long>(hash));
    app_paths::ensure_dir(app_paths::music_cover_cache_dir());
    std::string directory = std::string(app_paths::music_cover_cache_dir()) + name;
    app_paths::ensure_dir(directory.c_str());
    return directory;
}

bool valid_image_file(const std::string &path)
{
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || st.st_size < 16 || S_ISDIR(st.st_mode)) return false;
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) return false;
    std::string extension = image_extension(file, 0);
    fclose(file);
    return !extension.empty();
}

bool copy_candidate(FILE *source, const Candidate &candidate, const std::string &path)
{
    if (candidate.size < 16 || candidate.size > MAX_COVER_BYTES ||
        !seek_abs(source, candidate.offset)) return false;
    std::string temporary = path + ".tmp";
    FILE *output = fopen(temporary.c_str(), "wb");
    if (!output) return false;

    uint8_t buffer[4096];
    uint32_t remaining = candidate.size;
    bool ok = true;
    while (remaining > 0) {
        size_t amount = std::min<size_t>(sizeof(buffer), remaining);
        if (fread(buffer, 1, amount, source) != amount ||
            fwrite(buffer, 1, amount, output) != amount) {
            ok = false;
            break;
        }
        remaining -= static_cast<uint32_t>(amount);
    }
    if (fclose(output) != 0) ok = false;
    if (!ok) {
        remove(temporary.c_str());
        return false;
    }
    remove(path.c_str());
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        remove(temporary.c_str());
        return false;
    }
    return true;
}

void prefer_candidate(Candidate *selected, const Candidate &candidate)
{
    if (candidate.extension.empty() || candidate.size < 16 || candidate.size > MAX_COVER_BYTES) return;
    if (selected->size == 0 || (candidate.picture_type == 3 && selected->picture_type != 3)) {
        *selected = candidate;
    }
}

Candidate parse_flac(FILE *file)
{
    Candidate selected;
    uint8_t first[10] = {};
    if (!seek_abs(file, 0) || !read_exact(file, first, sizeof(first))) return selected;
    uint64_t offset = memcmp(first, "ID3", 3) == 0 ? 10u + synchsafe32(first + 6) : 0;
    uint8_t marker[4] = {};
    if (!seek_abs(file, offset) || !read_exact(file, marker, sizeof(marker)) ||
        memcmp(marker, "fLaC", 4) != 0) return selected;

    bool last = false;
    while (!last) {
        uint8_t header[4] = {};
        if (!read_exact(file, header, sizeof(header))) break;
        last = (header[0] & 0x80) != 0;
        uint8_t type = header[0] & 0x7f;
        uint32_t block_size = (uint32_t(header[1]) << 16) |
                              (uint32_t(header[2]) << 8) | header[3];
        uint64_t block_start = static_cast<uint64_t>(ftell(file));
        if (type == 6 && block_size >= 32) {
            Candidate candidate;
            uint32_t mime_size = 0;
            uint32_t description_size = 0;
            uint32_t ignored = 0;
            if (!read_be32(file, &candidate.picture_type) ||
                !read_be32(file, &mime_size) || mime_size > 128 ||
                8u + mime_size + 20u > block_size) {
                seek_abs(file, block_start + block_size);
                continue;
            }
            std::string mime(mime_size, '\0');
            if (!read_exact(file, mime.data(), mime.size()) ||
                !read_be32(file, &description_size) ||
                description_size > block_size - 12u - mime_size - 20u ||
                fseek(file, static_cast<long>(description_size), SEEK_CUR) != 0 ||
                !read_be32(file, &ignored) || !read_be32(file, &ignored) ||
                !read_be32(file, &ignored) || !read_be32(file, &ignored) ||
                !read_be32(file, &candidate.size)) {
                seek_abs(file, block_start + block_size);
                continue;
            }
            candidate.offset = static_cast<uint64_t>(ftell(file));
            uint64_t block_end = block_start + block_size;
            if (candidate.offset <= block_end && candidate.size <= block_end - candidate.offset) {
                candidate.extension = image_extension(file, candidate.offset, mime);
                prefer_candidate(&selected, candidate);
            }
        }
        if (!seek_abs(file, block_start + block_size)) break;
    }
    return selected;
}

size_t encoded_terminator(uint8_t encoding)
{
    return encoding == 0 || encoding == 3 ? 1 : 2;
}

bool skip_encoded_string(FILE *file, uint64_t limit, uint8_t encoding)
{
    size_t terminator = encoded_terminator(encoding);
    uint8_t previous = 1;
    while (static_cast<uint64_t>(ftell(file)) < limit) {
        int value = fgetc(file);
        if (value == EOF) return false;
        if (terminator == 1 && value == 0) return true;
        if (terminator == 2 && previous == 0 && value == 0) return true;
        previous = static_cast<uint8_t>(value);
    }
    return false;
}

Candidate parse_id3_at(FILE *file, uint64_t offset)
{
    Candidate selected;
    uint8_t header[10] = {};
    if (!seek_abs(file, offset) || !read_exact(file, header, sizeof(header)) ||
        memcmp(header, "ID3", 3) != 0) return selected;
    int version = header[3];
    if (version < 2 || version > 4 || (header[5] & 0x80) != 0) return selected;
    uint64_t end = offset + 10u + synchsafe32(header + 6);
    uint64_t cursor = offset + 10;
    if ((header[5] & 0x40) != 0 && version >= 3) {
        uint8_t bytes[4] = {};
        if (!seek_abs(file, cursor) || !read_exact(file, bytes, sizeof(bytes))) return selected;
        uint32_t size = version == 4 ? synchsafe32(bytes) : be32(bytes);
        cursor += version == 4 ? size : uint64_t(size) + 4;
    }

    while (cursor + (version == 2 ? 6u : 10u) <= end) {
        uint8_t frame[10] = {};
        size_t header_size = version == 2 ? 6 : 10;
        if (!seek_abs(file, cursor) || !read_exact(file, frame, header_size) || frame[0] == 0) break;
        char id[5] = {};
        memcpy(id, frame, version == 2 ? 3 : 4);
        uint32_t size = version == 2
                            ? (uint32_t(frame[3]) << 16) | (uint32_t(frame[4]) << 8) | frame[5]
                            : (version == 4 ? synchsafe32(frame + 4) : be32(frame + 4));
        uint64_t payload = cursor + header_size;
        cursor = payload + size;
        if (size == 0 || cursor > end) break;
        bool apic = strcmp(id, "APIC") == 0 || strcmp(id, "PIC") == 0;
        if (!apic || size > MAX_COVER_BYTES + 1024) continue;
        if (version >= 3 && (frame[9] & (version == 4 ? 0x4d : 0xe0)) != 0) continue;

        if (!seek_abs(file, payload)) continue;
        int encoding_value = fgetc(file);
        if (encoding_value == EOF) continue;
        uint8_t encoding = static_cast<uint8_t>(encoding_value);
        std::string mime;
        if (version == 2) {
            char format[4] = {};
            if (!read_exact(file, format, 3)) continue;
            mime = strcasecmp(format, "PNG") == 0 ? "image/png" : "image/jpeg";
        } else {
            while (static_cast<uint64_t>(ftell(file)) < cursor) {
                int c = fgetc(file);
                if (c == EOF || c == 0) break;
                if (mime.size() < 127) mime.push_back(static_cast<char>(c));
            }
        }
        int picture_type = fgetc(file);
        if (picture_type == EOF || !skip_encoded_string(file, cursor, encoding)) continue;
        Candidate candidate;
        candidate.offset = static_cast<uint64_t>(ftell(file));
        candidate.size = candidate.offset <= cursor ? static_cast<uint32_t>(cursor - candidate.offset) : 0;
        candidate.picture_type = static_cast<uint32_t>(picture_type);
        candidate.extension = image_extension(file, candidate.offset, mime);
        prefer_candidate(&selected, candidate);
        if (selected.picture_type == 3) break;
    }
    return selected;
}

uint32_t fourcc(const char value[4])
{
    return (uint32_t(uint8_t(value[0])) << 24) | (uint32_t(uint8_t(value[1])) << 16) |
           (uint32_t(uint8_t(value[2])) << 8) | uint8_t(value[3]);
}

void parse_mp4_atoms(FILE *file, uint64_t start, uint64_t end, int depth,
                     bool inside_cover, Candidate *selected)
{
    if (depth > MAX_MP4_DEPTH || start >= end || selected->size != 0) return;
    uint64_t pos = start;
    while (pos + 8 <= end && selected->size == 0) {
        uint8_t header[16] = {};
        if (!seek_abs(file, pos) || !read_exact(file, header, 8)) return;
        uint64_t size = be32(header);
        uint32_t type = be32(header + 4);
        uint64_t header_size = 8;
        if (size == 1) {
            if (!read_exact(file, header + 8, 8)) return;
            size = (uint64_t(be32(header + 8)) << 32) | be32(header + 12);
            header_size = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < header_size || size > end - pos) return;
        uint64_t payload = pos + header_size;
        uint64_t payload_size = size - header_size;
        if (inside_cover && type == fourcc("data") && payload_size > 8 &&
            payload_size - 8 <= MAX_COVER_BYTES) {
            Candidate candidate;
            candidate.offset = payload + 8;
            candidate.size = static_cast<uint32_t>(payload_size - 8);
            candidate.picture_type = 3;
            candidate.extension = image_extension(file, candidate.offset);
            prefer_candidate(selected, candidate);
            return;
        }

        bool cover = type == fourcc("covr");
        bool container = type == fourcc("moov") || type == fourcc("udta") ||
                         type == fourcc("meta") || type == fourcc("ilst") || cover;
        if (container) {
            uint64_t child = payload;
            if (type == fourcc("meta") && payload_size >= 4) child += 4;
            parse_mp4_atoms(file, child, pos + size, depth + 1,
                            inside_cover || cover, selected);
        }
        pos += size;
    }
}

Candidate parse_mp4(FILE *file)
{
    Candidate selected;
    if (fseek(file, 0, SEEK_END) != 0) return selected;
    long length = ftell(file);
    if (length > 0) parse_mp4_atoms(file, 0, static_cast<uint64_t>(length), 0, false, &selected);
    return selected;
}

Candidate parse_wave(FILE *file)
{
    Candidate selected;
    uint8_t header[12] = {};
    if (!seek_abs(file, 0) || !read_exact(file, header, sizeof(header)) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) return selected;
    uint64_t pos = 12;
    while (true) {
        uint8_t chunk[8] = {};
        if (!seek_abs(file, pos) || !read_exact(file, chunk, sizeof(chunk))) break;
        uint32_t size = uint32_t(chunk[4]) | (uint32_t(chunk[5]) << 8) |
                        (uint32_t(chunk[6]) << 16) | (uint32_t(chunk[7]) << 24);
        if (memcmp(chunk, "ID3 ", 4) == 0 || memcmp(chunk, "id3 ", 4) == 0) {
            return parse_id3_at(file, pos + 8);
        }
        pos += 8u + size + (size & 1u);
    }
    return selected;
}

std::string sidecar_cover(const char *audio_path)
{
    std::string path(audio_path);
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    std::string stem = dot != std::string::npos && (slash == std::string::npos || dot > slash)
                           ? path.substr(0, dot)
                           : path;
    std::string directory = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    const char *candidates[] = {
        ".jpg", ".jpeg", ".png", "cover.jpg", "Cover.jpg", "folder.jpg", "Folder.jpg",
        "front.jpg", "Front.jpg", "cover.png", "Cover.png", "folder.png", "Folder.png",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        std::string candidate = i < 3 ? stem + candidates[i] : directory + candidates[i];
        if (valid_image_file(candidate)) return candidate;
    }
    return {};
}

}  // namespace

bool find_or_extract(const char *audio_path, std::string *image_path)
{
    if (image_path == nullptr) return false;
    image_path->clear();
    if (audio_path == nullptr || audio_path[0] == '\0') return false;

    FILE *file = fopen(audio_path, "rb");
    if (!file) return false;
    std::string extension = lower_extension(audio_path);
    Candidate candidate;
    if (extension == ".flac") candidate = parse_flac(file);
    else if (extension == ".m4a") candidate = parse_mp4(file);
    else if (extension == ".wav") candidate = parse_wave(file);
    else candidate = parse_id3_at(file, 0);

    if (candidate.size != 0 && !candidate.extension.empty()) {
        std::string directory = cache_dir_for(audio_path);
        std::string cached = directory.empty()
                                 ? std::string()
                                 : directory + "/embedded" + candidate.extension;
        bool ok = !cached.empty() &&
                  (valid_image_file(cached) || copy_candidate(file, candidate, cached));
        fclose(file);
        if (ok) {
            *image_path = cached;
            ESP_LOGI(TAG, "Embedded cover type=%lu bytes=%lu cache=%s source=%s",
                     static_cast<unsigned long>(candidate.picture_type),
                     static_cast<unsigned long>(candidate.size), cached.c_str(), audio_path);
            return true;
        }
    } else {
        fclose(file);
    }

    std::string sidecar = sidecar_cover(audio_path);
    if (!sidecar.empty()) {
        *image_path = sidecar;
        ESP_LOGI(TAG, "Using sidecar cover: %s", sidecar.c_str());
        return true;
    }
    ESP_LOGI(TAG, "No cover found: %s", audio_path);
    return false;
}

std::string square_thumbnail_path(const char *audio_path)
{
    if (audio_path == nullptr || audio_path[0] == '\0') return {};
    std::string directory = cache_dir_for(audio_path);
    return directory.empty() ? std::string() : directory + "/cover_square_v1.jpg";
}

}  // namespace embedded_cover
