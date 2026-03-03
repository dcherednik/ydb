#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

struct TRegion {
    uint64_t Start = 0;
    uint64_t End = 0;
    std::string Perms;
    std::string Path;
};

struct TOptions {
    static constexpr uint64_t DefaultMagicNextLogChunkReference = 0x709DA7A709DA7A11ull;
    static constexpr uint64_t DefaultMagicLogChunk = 0x11170915A71FE111ull;
    static constexpr uint64_t DefaultMagicDataChunk = 0xDA7AC8A2CDA7AC8Aull;
    static constexpr uint64_t DefaultMagicSysLogChunk = 0x5957095957095957ull;
    static constexpr uint64_t DefaultMagicFormatChunk = 0xF088A7F088A7F088ull;
    static constexpr uint32_t DefaultPDiskChunkSize = 136314880u;

    pid_t Pid = -1;
    size_t ChunkSize = 1ULL << 20;
    size_t MaxResults = 100;
    bool ListRegions = false;
    bool ListAllRegions = false;
    bool PDiskGuidMode = false;
    uint64_t PDiskGuid = 0;
    std::optional<uint32_t> PDiskChunkSize;
    uint64_t PDiskMagicNextLogChunkReference = DefaultMagicNextLogChunkReference;
    uint64_t PDiskMagicLogChunk = DefaultMagicLogChunk;
    uint64_t PDiskMagicDataChunk = DefaultMagicDataChunk;
    uint64_t PDiskMagicSysLogChunk = DefaultMagicSysLogChunk;
    uint64_t PDiskMagicFormatChunk = DefaultMagicFormatChunk;
    std::vector<unsigned char> Pattern;
};

struct TScanResult {
    bool Success = false;
    size_t Matches = 0;
};

struct TPDiskFormatHeader {
    uint64_t Version = 0;
    uint64_t DiskSize = 0;
    uint64_t Guid = 0;
    uint64_t SysLogKey = 0;
    uint64_t LogKey = 0;
    uint64_t ChunkKey = 0;
    uint64_t MagicNextLogChunkReference = 0;
    uint64_t MagicLogChunk = 0;
    uint64_t MagicDataChunk = 0;
    uint64_t MagicSysLogChunk = 0;
    uint64_t MagicFormatChunk = 0;
    uint32_t ChunkSize = 0;
    uint32_t SectorSize = 0;
    uint32_t SysLogSectorCount = 0;
    uint32_t SystemChunkCount = 0;
};

template <typename T>
bool ParseUnsigned(std::string_view text, int base, T& value) {
    unsigned long long parsed = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return false;
    }

    if (parsed > std::numeric_limits<T>::max()) {
        return false;
    }

    value = static_cast<T>(parsed);
    return true;
}

bool ParseHexRange(std::string_view text, uint64_t& start, uint64_t& end) {
    const size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        return false;
    }
    if (!ParseUnsigned(text.substr(0, dash), 16, start) || !ParseUnsigned(text.substr(dash + 1), 16, end)) {
        return false;
    }
    return start < end;
}

std::string TrimLeft(std::string text) {
    const auto it = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    text.erase(text.begin(), it);
    return text;
}

bool ParseMapsLine(const std::string& line, TRegion& region) {
    std::istringstream in(line);
    std::string range;
    std::string offset;
    std::string dev;
    std::string inode;
    if (!(in >> range >> region.Perms >> offset >> dev >> inode)) {
        return false;
    }
    if (!ParseHexRange(range, region.Start, region.End)) {
        return false;
    }
    std::getline(in, region.Path);
    region.Path = TrimLeft(std::move(region.Path));
    return true;
}

bool ReadMaps(pid_t pid, std::vector<TRegion>& regions, std::string& error) {
    const std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(mapsPath);
    if (!maps) {
        error = "failed to open " + mapsPath + ": " + std::strerror(errno);
        return false;
    }

    std::string line;
    while (std::getline(maps, line)) {
        TRegion region;
        if (ParseMapsLine(line, region)) {
            regions.emplace_back(std::move(region));
        }
    }

    if (!maps.eof()) {
        error = "failed to read " + mapsPath;
        return false;
    }

    return true;
}

bool IsReadable(const TRegion& region) {
    return !region.Perms.empty() && region.Perms.front() == 'r';
}

std::string FormatAddress(uint64_t address) {
    std::ostringstream out;
    out << "0x" << std::hex << address;
    return out.str();
}

std::string FormatPath(const std::string& path) {
    return path.empty() ? "[anonymous]" : path;
}

bool ParseUi64Value(std::string_view text, uint64_t& value) {
    if (text.empty()) {
        return false;
    }

    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        const std::string_view hexPart = text.substr(2);
        return !hexPart.empty() && ParseUnsigned(hexPart, 16, value);
    }

    return ParseUnsigned(text, 10, value);
}

void EncodeUi64LE(uint64_t value, std::vector<unsigned char>& pattern) {
    pattern.resize(sizeof(value));
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<unsigned char>((value >> (i * 8)) & 0xFFu);
    }
}

bool ParseUi64Pattern(std::string_view text, std::vector<unsigned char>& pattern, std::string& error) {
    if (text.empty()) {
        error = "empty --ui64 value";
        return false;
    }

    uint64_t value = 0;
    if (!ParseUi64Value(text, value)) {
        error = "invalid --ui64 value: " + std::string(text);
        return false;
    }

    EncodeUi64LE(value, pattern);

    return true;
}

void PrintUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --pid <pid> --ui64 <value> [options]\n"
        << "  " << argv0 << " --pid <pid> --pdisk-guid <value> [--pdisk-chunk-size <value>] [options]\n"
        << "  " << argv0 << " --pid <pid> --list-regions [--all-regions]\n\n"
        << "Options:\n"
        << "  --pid <pid>               Target process id.\n"
        << "  --ui64 <value>            Unsigned 64-bit value (decimal or 0x...), searched as 8 bytes (little-endian).\n"
        << "  --pdisk-guid <value>      Guid from NPDisk::TDiskFormat.\n"
        << "  --pdisk-chunk-size <val>  Expected TDiskFormat::ChunkSize when using --pdisk-guid (default 136314880).\n"
        << "  --pdisk-magic-next-log-chunk-reference <value>  Expected MagicNextLogChunkReference.\n"
        << "  --pdisk-magic-log-chunk <value>                 Expected MagicLogChunk.\n"
        << "  --pdisk-magic-data-chunk <value>                Expected MagicDataChunk.\n"
        << "  --pdisk-magic-sys-log-chunk <value>             Expected MagicSysLogChunk.\n"
        << "  --pdisk-magic-format-chunk <value>              Expected MagicFormatChunk.\n"
        << "  --max-results <count>     Stop after this many matches (default 100).\n"
        << "  --list-regions            Print /proc/<pid>/maps entries.\n"
        << "  --all-regions             Show all regions when listing.\n"
        << "  --help                    Show this message.\n\n"
        << "Notes:\n"
        << "  Scanning reads only readable regions from /proc/<pid>/maps via /proc/<pid>/mem.\n"
        << "  Access may fail without ptrace permissions/capabilities.\n";
}

bool ParseArgs(int argc, char** argv, TOptions& options, std::string& error) {
    std::optional<std::string> ui64Pattern;
    std::optional<std::string> pdiskGuidPattern;
    std::optional<std::string> pdiskChunkSizePattern;
    std::optional<std::string> pdiskMagicNextLogChunkReferencePattern;
    std::optional<std::string> pdiskMagicLogChunkPattern;
    std::optional<std::string> pdiskMagicDataChunkPattern;
    std::optional<std::string> pdiskMagicSysLogChunkPattern;
    std::optional<std::string> pdiskMagicFormatChunkPattern;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--list-regions") {
            options.ListRegions = true;
        } else if (arg == "--all-regions") {
            options.ListAllRegions = true;
        } else if (arg == "--pid" || arg.rfind("--pid=", 0) == 0) {
            std::string value;
            if (arg == "--pid") {
                if (i + 1 >= argc) {
                    error = "--pid requires a value";
                    return false;
                }
                value = argv[++i];
            } else {
                value = arg.substr(std::string("--pid=").size());
            }

            unsigned int parsedPid = 0;
            if (!ParseUnsigned(value, 10, parsedPid) || parsedPid == 0 || parsedPid > static_cast<unsigned int>(std::numeric_limits<pid_t>::max())) {
                error = "invalid pid: " + value;
                return false;
            }
            options.Pid = static_cast<pid_t>(parsedPid);
        } else if (arg == "--ui64" || arg.rfind("--ui64=", 0) == 0) {
            if (arg == "--ui64") {
                if (i + 1 >= argc) {
                    error = "--ui64 requires a value";
                    return false;
                }
                ui64Pattern = argv[++i];
            } else {
                ui64Pattern = arg.substr(std::string("--ui64=").size());
            }
        } else if (arg == "--pdisk-guid" || arg.rfind("--pdisk-guid=", 0) == 0) {
            if (arg == "--pdisk-guid") {
                if (i + 1 >= argc) {
                    error = "--pdisk-guid requires a value";
                    return false;
                }
                pdiskGuidPattern = argv[++i];
            } else {
                pdiskGuidPattern = arg.substr(std::string("--pdisk-guid=").size());
            }
        } else if (arg == "--pdisk-chunk-size" || arg.rfind("--pdisk-chunk-size=", 0) == 0) {
            if (arg == "--pdisk-chunk-size") {
                if (i + 1 >= argc) {
                    error = "--pdisk-chunk-size requires a value";
                    return false;
                }
                pdiskChunkSizePattern = argv[++i];
            } else {
                pdiskChunkSizePattern = arg.substr(std::string("--pdisk-chunk-size=").size());
            }
        } else if (arg == "--pdisk-magic-next-log-chunk-reference" ||
                arg.rfind("--pdisk-magic-next-log-chunk-reference=", 0) == 0) {
            if (arg == "--pdisk-magic-next-log-chunk-reference") {
                if (i + 1 >= argc) {
                    error = "--pdisk-magic-next-log-chunk-reference requires a value";
                    return false;
                }
                pdiskMagicNextLogChunkReferencePattern = argv[++i];
            } else {
                pdiskMagicNextLogChunkReferencePattern =
                    arg.substr(std::string("--pdisk-magic-next-log-chunk-reference=").size());
            }
        } else if (arg == "--pdisk-magic-log-chunk" || arg.rfind("--pdisk-magic-log-chunk=", 0) == 0) {
            if (arg == "--pdisk-magic-log-chunk") {
                if (i + 1 >= argc) {
                    error = "--pdisk-magic-log-chunk requires a value";
                    return false;
                }
                pdiskMagicLogChunkPattern = argv[++i];
            } else {
                pdiskMagicLogChunkPattern = arg.substr(std::string("--pdisk-magic-log-chunk=").size());
            }
        } else if (arg == "--pdisk-magic-data-chunk" || arg.rfind("--pdisk-magic-data-chunk=", 0) == 0) {
            if (arg == "--pdisk-magic-data-chunk") {
                if (i + 1 >= argc) {
                    error = "--pdisk-magic-data-chunk requires a value";
                    return false;
                }
                pdiskMagicDataChunkPattern = argv[++i];
            } else {
                pdiskMagicDataChunkPattern = arg.substr(std::string("--pdisk-magic-data-chunk=").size());
            }
        } else if (arg == "--pdisk-magic-sys-log-chunk" || arg.rfind("--pdisk-magic-sys-log-chunk=", 0) == 0) {
            if (arg == "--pdisk-magic-sys-log-chunk") {
                if (i + 1 >= argc) {
                    error = "--pdisk-magic-sys-log-chunk requires a value";
                    return false;
                }
                pdiskMagicSysLogChunkPattern = argv[++i];
            } else {
                pdiskMagicSysLogChunkPattern = arg.substr(std::string("--pdisk-magic-sys-log-chunk=").size());
            }
        } else if (arg == "--pdisk-magic-format-chunk" || arg.rfind("--pdisk-magic-format-chunk=", 0) == 0) {
            if (arg == "--pdisk-magic-format-chunk") {
                if (i + 1 >= argc) {
                    error = "--pdisk-magic-format-chunk requires a value";
                    return false;
                }
                pdiskMagicFormatChunkPattern = argv[++i];
            } else {
                pdiskMagicFormatChunkPattern = arg.substr(std::string("--pdisk-magic-format-chunk=").size());
            }
        } else if (arg == "--max-results" || arg.rfind("--max-results=", 0) == 0) {
            std::string value;
            if (arg == "--max-results") {
                if (i + 1 >= argc) {
                    error = "--max-results requires a value";
                    return false;
                }
                value = argv[++i];
            } else {
                value = arg.substr(std::string("--max-results=").size());
            }

            if (!ParseUnsigned(value, 10, options.MaxResults) || options.MaxResults == 0) {
                error = "invalid --max-results value: " + value;
                return false;
            }
        } else {
            error = "unknown option: " + arg;
            return false;
        }
    }

    if (options.Pid <= 0) {
        error = "--pid is required";
        return false;
    }

    const size_t suppliedPatterns =
        static_cast<size_t>(ui64Pattern.has_value()) +
        static_cast<size_t>(pdiskGuidPattern.has_value());
    if (suppliedPatterns > 1) {
        error = "use only one of --ui64 or --pdisk-guid";
        return false;
    }

    if (ui64Pattern) {
        if (!ParseUi64Pattern(*ui64Pattern, options.Pattern, error)) {
            return false;
        }
    } else if (pdiskGuidPattern) {
        uint64_t guid = 0;
        if (!ParseUi64Value(*pdiskGuidPattern, guid)) {
            error = "invalid --pdisk-guid value: " + *pdiskGuidPattern;
            return false;
        }
        EncodeUi64LE(guid, options.Pattern);
        options.PDiskGuidMode = true;
        options.PDiskGuid = guid;
    }

    if (pdiskChunkSizePattern) {
        uint64_t value = 0;
        if (!ParseUi64Value(*pdiskChunkSizePattern, value) || value > std::numeric_limits<uint32_t>::max()) {
            error = "invalid --pdisk-chunk-size value: " + *pdiskChunkSizePattern;
            return false;
        }
        options.PDiskChunkSize = static_cast<uint32_t>(value);
    }

    auto parseMagic = [&](const std::optional<std::string>& source, const char* name, uint64_t& target) {
        if (!source) {
            return true;
        }
        uint64_t value = 0;
        if (!ParseUi64Value(*source, value)) {
            error = std::string("invalid ") + name + " value: " + *source;
            return false;
        }
        target = value;
        return true;
    };

    if (!parseMagic(pdiskMagicNextLogChunkReferencePattern, "--pdisk-magic-next-log-chunk-reference",
            options.PDiskMagicNextLogChunkReference)) {
        return false;
    }
    if (!parseMagic(pdiskMagicLogChunkPattern, "--pdisk-magic-log-chunk", options.PDiskMagicLogChunk)) {
        return false;
    }
    if (!parseMagic(pdiskMagicDataChunkPattern, "--pdisk-magic-data-chunk", options.PDiskMagicDataChunk)) {
        return false;
    }
    if (!parseMagic(pdiskMagicSysLogChunkPattern, "--pdisk-magic-sys-log-chunk", options.PDiskMagicSysLogChunk)) {
        return false;
    }
    if (!parseMagic(pdiskMagicFormatChunkPattern, "--pdisk-magic-format-chunk", options.PDiskMagicFormatChunk)) {
        return false;
    }

    if (options.PDiskGuidMode && !options.PDiskChunkSize.has_value()) {
        options.PDiskChunkSize = TOptions::DefaultPDiskChunkSize;
    }
    if (!options.PDiskGuidMode && options.PDiskChunkSize.has_value()) {
        error = "--pdisk-chunk-size can be used only with --pdisk-guid";
        return false;
    }
    if (!options.PDiskGuidMode && (
            pdiskMagicNextLogChunkReferencePattern.has_value() ||
            pdiskMagicLogChunkPattern.has_value() ||
            pdiskMagicDataChunkPattern.has_value() ||
            pdiskMagicSysLogChunkPattern.has_value() ||
            pdiskMagicFormatChunkPattern.has_value())) {
        error = "--pdisk-magic-* options can be used only with --pdisk-guid";
        return false;
    }

    if (options.Pattern.empty() && !options.ListRegions) {
        error = "value is required for scanning (use --ui64 or --pdisk-guid)";
        return false;
    }

    return true;
}

void PrintRegions(const std::vector<TRegion>& regions, bool allRegions) {
    for (const auto& region : regions) {
        if (!allRegions && !IsReadable(region)) {
            continue;
        }
        std::cout
            << FormatAddress(region.Start) << "-"
            << FormatAddress(region.End) << " "
            << region.Perms << " "
            << FormatPath(region.Path) << '\n';
    }
}

uint64_t ReadUi64LE(const unsigned char* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

uint32_t ReadUi32LE(const unsigned char* data) {
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        value |= static_cast<uint32_t>(data[i]) << (i * 8);
    }
    return value;
}

bool ReadProcessMemory(int fd, uint64_t address, unsigned char* data, size_t size) {
    size_t done = 0;
    while (done < size) {
        const ssize_t readBytes = ::pread(fd, data + done, size - done, static_cast<off_t>(address + done));
        if (readBytes <= 0) {
            return false;
        }
        done += static_cast<size_t>(readBytes);
    }
    return true;
}

bool ReadPDiskFormatHeader(int fd, uint64_t guidAddress, TPDiskFormatHeader& header) {
    constexpr size_t PrefixSize = 16; // Version + DiskSize
    constexpr size_t Ui64Count = 11; // Version..MagicFormatChunk
    constexpr size_t Ui32Count = 4;  // ChunkSize..SystemChunkCount
    constexpr size_t BlockSize = Ui64Count * sizeof(uint64_t) + Ui32Count * sizeof(uint32_t);

    if (guidAddress < PrefixSize) {
        return false;
    }

    unsigned char block[BlockSize] = {};
    const uint64_t base = guidAddress - PrefixSize;
    if (!ReadProcessMemory(fd, base, block, sizeof(block))) {
        return false;
    }

    header.Version = ReadUi64LE(block + 0);
    header.DiskSize = ReadUi64LE(block + 8);
    header.Guid = ReadUi64LE(block + 16);
    header.SysLogKey = ReadUi64LE(block + 24);
    header.LogKey = ReadUi64LE(block + 32);
    header.ChunkKey = ReadUi64LE(block + 40);
    header.MagicNextLogChunkReference = ReadUi64LE(block + 48);
    header.MagicLogChunk = ReadUi64LE(block + 56);
    header.MagicDataChunk = ReadUi64LE(block + 64);
    header.MagicSysLogChunk = ReadUi64LE(block + 72);
    header.MagicFormatChunk = ReadUi64LE(block + 80);
    header.ChunkSize = ReadUi32LE(block + 88);
    header.SectorSize = ReadUi32LE(block + 92);
    header.SysLogSectorCount = ReadUi32LE(block + 96);
    header.SystemChunkCount = ReadUi32LE(block + 100);
    return true;
}

bool MatchesPDiskFormat(const TOptions& options, const TPDiskFormatHeader& header) {
    return
        header.Guid == options.PDiskGuid &&
        header.MagicLogChunk == options.PDiskMagicLogChunk &&
        header.MagicDataChunk == options.PDiskMagicDataChunk &&
        header.MagicSysLogChunk == options.PDiskMagicSysLogChunk &&
        header.MagicFormatChunk == options.PDiskMagicFormatChunk &&
        options.PDiskChunkSize.has_value() &&
        header.ChunkSize == *options.PDiskChunkSize;
}

void PrintPDiskFormatKeys(const TOptions&, uint64_t, const TRegion&,
        const TPDiskFormatHeader& header) {
/*
    std::cout
        << "pdisk_keys pid=" << options.Pid
        << " guid_address=" << FormatAddress(guidAddress)
        << " region=" << FormatAddress(region.Start) << "-" << FormatAddress(region.End)
        << " path=" << FormatPath(region.Path)
        << " SysLogKey=" << header.SysLogKey
        << " LogKey=" << header.LogKey
        << " ChunkKey=" << header.ChunkKey
        << '\n';
*/
    std::cout
        << "SysLogKey=" << header.SysLogKey
        << " LogKey=" << header.LogKey
        << " ChunkKey=" << header.ChunkKey
        << '\n';

}

TScanResult ScanMemory(const TOptions& options, const std::vector<TRegion>& regions) {
    const std::string memPath = "/proc/" + std::to_string(options.Pid) + "/mem";
    const int fd = ::open(memPath.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "failed to open " << memPath << ": " << std::strerror(errno) << '\n';
        return {};
    }

    const long pageSize = std::max<long>(::sysconf(_SC_PAGESIZE), 4096);
    std::vector<unsigned char> buffer(options.ChunkSize);
    std::vector<unsigned char> carry;
    size_t matches = 0;

    for (const auto& region : regions) {
        if (!IsReadable(region)) {
            continue;
        }

        carry.clear();
        uint64_t offset = region.Start;
        while (offset < region.End) {
            const size_t toRead = std::min<size_t>(options.ChunkSize, region.End - offset);
            const ssize_t readBytes = ::pread(fd, buffer.data(), toRead, static_cast<off_t>(offset));
            if (readBytes < 0) {
                const int err = errno;
                std::cerr
                    << "warning: pread failed at " << FormatAddress(offset)
                    << " in region " << FormatAddress(region.Start) << "-" << FormatAddress(region.End)
                    << ": " << std::strerror(err) << '\n';
                offset += pageSize;
                carry.clear();
                continue;
            }
            if (readBytes == 0) {
                break;
            }

            std::vector<unsigned char> window;
            window.reserve(carry.size() + static_cast<size_t>(readBytes));
            window.insert(window.end(), carry.begin(), carry.end());
            window.insert(window.end(), buffer.begin(), buffer.begin() + readBytes);

            const uint64_t windowBase = offset - carry.size();
            auto it = window.begin();
            while (it != window.end()) {
                it = std::search(it, window.end(), options.Pattern.begin(), options.Pattern.end());
                if (it == window.end()) {
                    break;
                }

                const uint64_t address = windowBase + std::distance(window.begin(), it);
                // Skip duplicates that were fully contained in the previous chunk.
                if (address + options.Pattern.size() > offset) {
                    if (options.PDiskGuidMode) {
                        TPDiskFormatHeader header;
                        if (ReadPDiskFormatHeader(fd, address, header) && MatchesPDiskFormat(options, header)) {
                            PrintPDiskFormatKeys(options, address, region, header);
                            if (++matches >= options.MaxResults) {
                                ::close(fd);
                                return {.Success = true, .Matches = matches};
                            }
                        }
                    } else {
                        std::cout
                            << "match pid=" << options.Pid
                            << " address=" << FormatAddress(address)
                            << " region=" << FormatAddress(region.Start) << "-" << FormatAddress(region.End)
                            << " perms=" << region.Perms
                            << " path=" << FormatPath(region.Path) << '\n';
                        if (++matches >= options.MaxResults) {
                            ::close(fd);
                            return {.Success = true, .Matches = matches};
                        }
                    }
                }
                ++it;
            }

            const size_t overlap = std::min(options.Pattern.size() - 1, window.size());
            carry.assign(window.end() - overlap, window.end());
            offset += readBytes;
        }
    }

    ::close(fd);
    return {.Success = true, .Matches = matches};
}

} // namespace

int main(int argc, char** argv) {
    TOptions options;
    std::string error;
    if (!ParseArgs(argc, argv, options, error)) {
        std::cerr << error << '\n';
        PrintUsage(argv[0]);
        return 1;
    }

    std::vector<TRegion> regions;
    if (!ReadMaps(options.Pid, regions, error)) {
        std::cerr << error << '\n';
        return 2;
    }

    if (options.ListRegions) {
        PrintRegions(regions, options.ListAllRegions);
    }

    if (options.Pattern.empty()) {
        return 0;
    }

    const TScanResult scanResult = ScanMemory(options, regions);
    if (!scanResult.Success) {
        return 3;
    }

    std::cout << "total_matches=" << scanResult.Matches << '\n';
    return 0;
}
