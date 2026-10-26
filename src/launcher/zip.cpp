#include "zip.h"

#include <zlib.h>

#include <fstream>

namespace rd::zip {

namespace {

std::uint32_t le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
std::uint32_t le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

std::vector<Entry> list(const std::filesystem::path &archive) {
    std::vector<Entry> entries;
    std::ifstream file(archive, std::ios::binary | std::ios::ate);
    if (!file) return entries;
    const std::streamoff length = file.tellg();
    // The end of central directory record sits in the last 64K + 22 bytes
    const std::streamoff tail = std::min<std::streamoff>(length, 65557);
    std::vector<unsigned char> end(static_cast<std::size_t>(tail));
    file.seekg(length - tail);
    file.read(reinterpret_cast<char *>(end.data()), tail);
    std::streamoff eocd = -1;
    for (std::streamoff i = tail - 22; i >= 0; --i) {
        if (le32(&end[static_cast<std::size_t>(i)]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return entries;
    const unsigned char *record = &end[static_cast<std::size_t>(eocd)];
    const std::uint32_t count = le16(record + 10), dirSize = le32(record + 12), dirOffset = le32(record + 16);
    if (static_cast<std::streamoff>(dirOffset) + dirSize > length) return entries;
    std::vector<unsigned char> dir(dirSize);
    file.seekg(dirOffset);
    file.read(reinterpret_cast<char *>(dir.data()), dirSize);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < count && at + 46 <= dir.size(); ++i) {
        const unsigned char *h = &dir[at];
        if (le32(h) != 0x02014b50) break;
        const std::uint32_t nameLength = le16(h + 28), extraLength = le16(h + 30), commentLength = le16(h + 32);
        if (at + 46 + nameLength > dir.size()) break;
        Entry entry;
        entry.method = static_cast<std::uint16_t>(le16(h + 10));
        entry.compressedSize = le32(h + 20);
        entry.size = le32(h + 24);
        entry.localHeader = le32(h + 42);
        entry.name.assign(reinterpret_cast<const char *>(h + 46), nameLength);
        const bool encrypted = (le16(h + 8) & 1) != 0;
        if (!entry.name.empty() && entry.name.back() != '/' && !encrypted) entries.push_back(entry);
        at += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
}

bool extract(const std::filesystem::path &archive, const Entry &entry, const std::filesystem::path &target) {
    std::ifstream file(archive, std::ios::binary);
    if (!file) return false;
    unsigned char header[30];
    file.seekg(entry.localHeader);
    if (!file.read(reinterpret_cast<char *>(header), sizeof header) || le32(header) != 0x04034b50) return false;
    file.seekg(entry.localHeader + 30 + le16(header + 26) + le16(header + 28));
    std::vector<unsigned char> packed(entry.compressedSize);
    if (!file.read(reinterpret_cast<char *>(packed.data()), static_cast<std::streamsize>(packed.size()))) return false;

    std::vector<unsigned char> data;
    if (entry.method == 0) {
        data = std::move(packed);
    } else if (entry.method == 8) {
        data.resize(entry.size);
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
        stream.next_in = packed.data();
        stream.avail_in = static_cast<uInt>(packed.size());
        stream.next_out = data.data();
        stream.avail_out = static_cast<uInt>(data.size());
        const int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (result != Z_STREAM_END) return false;
    } else {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    const std::filesystem::path partial = target.string() + ".part";
    {
        std::ofstream out(partial, std::ios::binary);
        if (!out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()))) return false;
    }
    std::filesystem::rename(partial, target, error);
    return !error;
}

} // namespace rd::zip
