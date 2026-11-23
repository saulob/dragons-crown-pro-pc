#include "boot_image.h"

#include <cstdio>
#include "atlas_signatures.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "crypto.h"
#include "dcp_launcher.h"
#include "game_files.h"
#include "game_profile.h"
#include "utf8.h"

namespace dcp {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7;
constexpr uint32_t PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_PROCPARAM = 0x61000001, PT_SCE_RELRO = 0x61000010;
constexpr uint64_t DT_NEEDED = 1, DT_INIT = 12;
constexpr uint64_t kMaxImage = 512ull << 20;
constexpr uint64_t kKindCode = 1, kKindData = 2;
constexpr uint8_t kSelfMagic[] = {'O', 0x15, '=', 0x1d};
// mov rax, fs:[0]: initial-exec TLS loads are rewritten to GS because the host owns FS.
constexpr uint8_t kFsLoad[] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t kNidSalt[] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
                                0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
constexpr char kIdAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void Fail(const std::string& message) { throw Failure(message); }

std::string Hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

void CheckSpan(size_t size, uint64_t offset, uint64_t length) {
    if (offset > size || length > size - offset) {
        Fail("out of bounds: " + Hex(offset) + "+" + Hex(length) + " / " + Hex(size));
    }
}

template <typename T> T Read(const Bytes& data, uint64_t offset) {
    CheckSpan(data.size(), offset, sizeof(T));
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

Bytes Span(const Bytes& data, uint64_t offset, uint64_t length) {
    CheckSpan(data.size(), offset, length);
    return Bytes(data.begin() + offset, data.begin() + offset + length);
}

void Put64(Bytes& image, uint64_t offset, uint64_t value) {
    CheckSpan(image.size(), offset, 8);
    std::memcpy(image.data() + offset, &value, 8);
}

Bytes ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Fail("cannot read " + path.filename().string());
    }
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

std::string Base64(const uint8_t* data, size_t size) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t chunk = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                               (i + 2 < size ? data[i + 2] : 0);
        out += digits[chunk >> 18 & 63];
        out += digits[chunk >> 12 & 63];
        out += i + 1 < size ? digits[chunk >> 6 & 63] : '=';
        out += i + 2 < size ? digits[chunk & 63] : '=';
    }
    return out;
}

// The NID of a symbol name: SHA-1 of the name and a fixed salt, first 8 bytes reversed, URL-ish base64.
std::string Nid(const std::string& name) {
    Bytes input(name.begin(), name.end());
    input.insert(input.end(), std::begin(kNidSalt), std::end(kNidSalt));
    uint8_t digest[20] = {};
    if (!crypto::Sha1(input.data(), input.size(), digest)) {
        Fail("SHA-1 failed");
    }
    uint8_t reversed[8];
    std::reverse_copy(digest, digest + 8, reversed);
    std::string id = Base64(reversed, 8);
    id.erase(id.find_last_not_of('=') + 1);
    std::replace(id.begin(), id.end(), '/', '-');
    return id;
}

std::string EncodeId(uint64_t value) {
    std::string result(1, kIdAlphabet[value & 63]);
    for (value >>= 6; value; value >>= 6) {
        result.insert(result.begin(), kIdAlphabet[value & 63]);
    }
    return result;
}

struct ProgramHeader {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct LibraryId {
    std::string name;
    uint32_t version = 0;
    bool operator==(const LibraryId&) const = default;
};

// (NID, (library, version), (module, version)), as an import or export names it.
struct Identity {
    std::string nid;
    LibraryId library, module;
    std::string Key() const {
        return nid + '\0' + library.name + '\0' + std::to_string(library.version) + '\0' + module.name + '\0' +
               std::to_string(module.version);
    }
    std::string Canonical() const { return nid + "#" + library.name; }
};

struct Symbol {
    std::string name;
    uint8_t type, binding;
    uint16_t section;
    uint64_t value, size;
    std::optional<Identity> identity;
};

struct Relocation {
    uint64_t target;
    uint32_t kind, symbol;
    int64_t addend;
};

struct Module {
    Bytes elf;
    uint64_t entry = 0;
    std::vector<ProgramHeader> ph;
    std::map<uint64_t, uint64_t> tags;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocs;
    std::vector<std::string> needed;
    std::string sha256;
};

const ProgramHeader* FindHeader(const Module& module, uint32_t type) {
    for (const ProgramHeader& p : module.ph) {
        if (p.type == type) {
            return &p;
        }
    }
    return nullptr;
}

const ProgramHeader& RequireHeader(const Module& module, uint32_t type) {
    const ProgramHeader* p = FindHeader(module, type);
    if (!p) {
        Fail("missing program header " + Hex(type));
    }
    return *p;
}

bool IsLoadable(const ProgramHeader& p) { return p.type == PT_LOAD || p.type == PT_SCE_RELRO; }

// A plaintext PS4 SELF (fake-signed or decrypted dump) back to its ELF image.
void ParseSelf(const Bytes& data, Module& module) {
    if (data.size() < 4 || std::memcmp(data.data(), kSelfMagic, 4) != 0) {
        Fail("expected PS4 SELF");
    }
    const uint16_t count = Read<uint16_t>(data, 24);
    const uint64_t base = 32 + uint64_t(count) * 32;
    CheckSpan(data.size(), base, 64);
    static const uint8_t elf_ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    if (std::memcmp(data.data() + base, elf_ident, sizeof(elf_ident)) != 0 || Read<uint16_t>(data, base + 18) != 62) {
        Fail("expected little-endian x86-64 ELF");
    }
    module.entry = Read<uint64_t>(data, base + 24);
    const uint64_t phoff = Read<uint64_t>(data, base + 32);
    const uint16_t phentsize = Read<uint16_t>(data, base + 54), phnum = Read<uint16_t>(data, base + 56);
    if (phentsize != 56 || phnum == 0 || phnum >= 256) {
        Fail("unsupported program headers");
    }
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint64_t at = base + phoff + uint64_t(i) * 56;
        module.ph.push_back({Read<uint32_t>(data, at), Read<uint32_t>(data, at + 4), Read<uint64_t>(data, at + 8),
                             Read<uint64_t>(data, at + 16), Read<uint64_t>(data, at + 24), Read<uint64_t>(data, at + 32),
                             Read<uint64_t>(data, at + 40), Read<uint64_t>(data, at + 48)});
    }
    uint64_t end = 0;
    for (const ProgramHeader& p : module.ph) {
        end = std::max(end, p.offset + p.filesz);
    }
    if (end > kMaxImage) {
        Fail("image exceeds 512 MiB limit");
    }
    const uint64_t header_end = phoff + uint64_t(phnum) * 56;
    CheckSpan(data.size(), base, header_end);
    module.elf.assign(std::max(end, header_end), 0);
    std::memcpy(module.elf.data(), data.data() + base, header_end);
    std::vector<std::pair<uint64_t, uint64_t>> covered{{0, header_end}};
    for (uint16_t i = 0; i < count; ++i) {
        const uint64_t at = 32 + uint64_t(i) * 32;
        const uint64_t flags = Read<uint64_t>(data, at), offset = Read<uint64_t>(data, at + 8);
        const uint64_t size = Read<uint64_t>(data, at + 16), memory = Read<uint64_t>(data, at + 24);
        if (!(flags & 0x800)) {
            continue;
        }
        if (flags & 10) {
            Fail("encrypted/compressed SELF segment is unsupported");
        }
        const uint64_t index = (flags >> 20) & 4095;
        if (index >= module.ph.size()) {
            Fail("invalid segment index");
        }
        const ProgramHeader& p = module.ph[index];
        if (size != p.filesz || size != memory) {
            Fail("unsupported blocked segment layout");
        }
        CheckSpan(data.size(), offset, size);
        std::memcpy(module.elf.data() + p.offset, data.data() + offset, size);
        covered.emplace_back(p.offset, p.offset + size);
    }
    for (size_t i = 0; i < module.ph.size(); ++i) {
        const ProgramHeader& p = module.ph[i];
        const bool present = std::any_of(covered.begin(), covered.end(), [&](const auto& range) {
            return range.first <= p.offset && p.offset + p.filesz <= range.second;
        });
        if (p.filesz && !present &&
            (p.type == PT_LOAD || p.type == PT_DYNAMIC || p.type == PT_SCE_PROCPARAM || p.type == PT_SCE_RELRO)) {
            Fail("required segment " + std::to_string(i) + " is unavailable");
        }
    }
}

uint64_t Tag(const Module& module, uint64_t tag) {
    const auto it = module.tags.find(tag);
    if (it == module.tags.end()) {
        Fail("missing dynamic tag " + Hex(tag));
    }
    return it->second;
}

// The module's dynamic data: imports, exports, relocations and library identities.
Module ReadModule(const std::filesystem::path& path) {
    const Bytes source = ReadFile(path);
    Module m;
    ParseSelf(source, m);
    const ProgramHeader& dp = RequireHeader(m, PT_DYNAMIC);
    std::vector<std::pair<uint64_t, uint64_t>> dynamic;
    for (uint64_t at = dp.offset; at < dp.offset + dp.filesz; at += 16) {
        const uint64_t tag = Read<uint64_t>(m.elf, at), value = Read<uint64_t>(m.elf, at + 8);
        if (!tag) {
            break;
        }
        dynamic.emplace_back(tag, value);
        m.tags[tag] = value;
    }
    const ProgramHeader& lp = RequireHeader(m, PT_SCE_DYNLIBDATA);
    const Bytes blob = Span(m.elf, lp.offset, lp.filesz);
    const Bytes strings = Span(blob, Tag(m, 0x61000035), Tag(m, 0x61000037));
    const auto string = [&](uint64_t offset) {
        CheckSpan(strings.size(), offset, 1);
        const auto end = std::find(strings.begin() + offset, strings.end(), 0);
        if (end == strings.end()) {
            Fail("unterminated string");
        }
        std::string text(strings.begin() + offset, end);
        if (std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
            Fail("non-ASCII symbol string");
        }
        return text;
    };
    std::map<std::string, LibraryId> libraries, modules;
    for (const auto& [tag, value] : dynamic) {
        std::map<std::string, LibraryId>* table = tag == 0x61000013 || tag == 0x61000015   ? &libraries
                                                  : tag == 0x6100000d || tag == 0x6100000f ? &modules
                                                                                           : nullptr;
        if (!table) {
            continue;
        }
        const std::string key = EncodeId(value >> 48);
        const LibraryId identity{string(value & 0xffffffff), uint32_t((value >> 32) & 65535)};
        const auto [it, added] = table->emplace(key, identity);
        if (!added && !(it->second == identity)) {
            Fail("conflicting module/library IDs");
        }
    }
    const auto identity = [&](const std::string& name) -> std::optional<Identity> {
        const size_t first = name.find('#');
        const size_t second = first == std::string::npos ? first : name.find('#', first + 1);
        if (second == std::string::npos || name.find('#', second + 1) != std::string::npos) {
            return std::nullopt; // plain names such as module_start are not linkable
        }
        const auto library = libraries.find(name.substr(first + 1, second - first - 1));
        const auto module = modules.find(name.substr(second + 1));
        if (library == libraries.end() || module == modules.end()) {
            Fail("unknown library or module in " + name);
        }
        return Identity{name.substr(0, first), library->second, module->second};
    };
    const Bytes symbols = Span(blob, Tag(m, 0x61000039), Tag(m, 0x6100003f));
    for (uint64_t at = 0; at < symbols.size(); at += 24) {
        Symbol s;
        s.name = string(Read<uint32_t>(symbols, at));
        const uint8_t info = Read<uint8_t>(symbols, at + 4);
        s.type = info & 15;
        s.binding = info >> 4;
        s.section = Read<uint16_t>(symbols, at + 6);
        s.value = Read<uint64_t>(symbols, at + 8);
        s.size = Read<uint64_t>(symbols, at + 16);
        if (!s.name.empty()) {
            s.identity = identity(s.name);
        }
        m.symbols.push_back(std::move(s));
    }
    for (const auto& [offset_tag, size_tag] : {std::pair{0x61000029ull, 0x6100002dull}, {0x6100002full, 0x61000031ull}}) {
        const Bytes table = Span(blob, Tag(m, offset_tag), Tag(m, size_tag));
        if (table.size() % 24) {
            Fail("bad relocation table size");
        }
        for (uint64_t at = 0; at < table.size(); at += 24) {
            const uint64_t info = Read<uint64_t>(table, at + 8);
            m.relocs.push_back({Read<uint64_t>(table, at), uint32_t(info), uint32_t(info >> 32),
                                Read<int64_t>(table, at + 16)});
        }
    }
    for (const auto& [tag, value] : dynamic) {
        if (tag == DT_NEEDED) {
            m.needed.push_back(string(value));
        }
    }
    m.sha256 = Sha256Hex(path);
    if (m.sha256.empty()) {
        Fail("cannot read " + path.filename().string());
    }
    return m;
}

// The runtime stands in for libc's _init_env, which is only valid when the original is a bare RET.
bool LibcInitEnvIsRet(const Module& libc) {
    const std::string wanted = Nid("_init_env");
    for (const Symbol& s : libc.symbols) {
        if (s.identity && s.identity->nid == wanted && s.section && s.type == 2) {
            const auto p = std::find_if(libc.ph.begin(), libc.ph.end(), [&](const ProgramHeader& p) {
                return p.type == PT_LOAD && (p.flags & 1) && p.vaddr <= s.value && s.value < p.vaddr + p.filesz;
            });
            if (p == libc.ph.end()) {
                Fail("_init_env outside the code");
            }
            return Span(libc.elf, p->offset + s.value - p->vaddr, s.size) == Bytes{0xc3};
        }
    }
    return false;
}

int PatchFsLoads(Bytes& image, const std::vector<ProgramHeader>& ph, uint64_t base) {
    int patched = 0;
    for (const ProgramHeader& p : ph) {
        if (p.type != PT_LOAD || !(p.flags & 1)) {
            continue;
        }
        const uint64_t start = std::min<uint64_t>(base + p.vaddr, image.size());
        const uint64_t end = std::min<uint64_t>(base + p.vaddr + p.filesz, image.size());
        auto at = std::search(image.begin() + start, image.begin() + end, std::begin(kFsLoad), std::end(kFsLoad));
        while (at != image.begin() + end) {
            *at = 0x65;
            ++patched;
            at = std::search(at + sizeof(kFsLoad), image.begin() + end, std::begin(kFsLoad), std::end(kFsLoad));
        }
    }
    return patched;
}

struct OutRelocation {
    uint64_t target, kind;
    int64_t value, addend;
};

struct Export {
    uint64_t address, kind;
};

struct ModuleEntry {
    std::string file;
    uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module, tls_relocations, exports;
    std::string sha256;
};

class ImageBuilder {
public:
    void MapEboot(const Module& main) {
        std::vector<const ProgramHeader*> loads;
        uint64_t size = 0;
        for (const ProgramHeader& p : main.ph) {
            if (IsLoadable(p)) {
                loads.push_back(&p);
                size = std::max(size, p.vaddr + p.memsz);
            }
        }
        const auto mapped = [&](uint64_t address, uint64_t length = 8) {
            return std::any_of(loads.begin(), loads.end(), [&](const ProgramHeader* p) {
                return p->vaddr <= address && address + length <= p->vaddr + p->memsz;
            });
        };
        if (size > kMaxImage || !mapped(main.entry, 1)) {
            Fail("invalid memory image");
        }
        image.assign(size, 0);
        for (const ProgramHeader* p : loads) {
            if (p->filesz > p->memsz) {
                Fail("segment file size exceeds memory size");
            }
            CheckSpan(main.elf.size(), p->offset, p->filesz);
            std::memcpy(image.data() + p->vaddr, main.elf.data() + p->offset, p->filesz);
            segments.push_back({p->vaddr, p->memsz, p->flags});
        }
        for (const Relocation& r : main.relocs) {
            if (!mapped(r.target)) {
                Fail("relocation outside mapped segments: " + Hex(r.target));
            }
            if (r.kind == 8) {
                relocs.push_back({r.target, 0, r.addend, 0});
            } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                const Symbol& symbol = SymbolOf(main, r);
                if (symbol.section) {
                    if (symbol.section == 0xfff1) {
                        Put64(image, r.target, symbol.value + r.addend);
                    } else {
                        relocs.push_back({r.target, 0, int64_t(symbol.value + r.addend), 0});
                    }
                } else {
                    if (!symbol.identity) {
                        Fail("unlinkable import '" + symbol.name + "'");
                    }
                    AddImport(symbol, r, r.target, "");
                }
            } else {
                Fail("unsupported relocation " + std::to_string(r.kind));
            }
        }
    }

    void LinkModules(const std::filesystem::path& game, const Module& main) {
        uint64_t base = (image.size() + 65535) & ~uint64_t(65535);
        fs_patched = PatchFsLoads(image, main.ph, 0);
        uint64_t tls_module = 2;
        for (const char* filename : kBundledModules) {
            const std::string file = filename;
            const Module m = ReadModule(game / "sce_module" / file);
            std::vector<const ProgramHeader*> loads;
            uint64_t modsize = 0;
            for (const ProgramHeader& p : m.ph) {
                if (IsLoadable(p)) {
                    loads.push_back(&p);
                    modsize = std::max(modsize, p.vaddr + p.memsz);
                }
            }
            if (base + modsize > kMaxImage) {
                Fail("linked image exceeds the 512 MiB limit");
            }
            if (base + modsize < image.size()) {
                Fail(file + ": overlapping module layout");
            }
            image.resize(base + modsize, 0);
            const auto mapped = [&](uint64_t address, uint64_t length = 8) {
                return std::any_of(loads.begin(), loads.end(), [&](const ProgramHeader* p) {
                    return p->vaddr <= address && address + length <= p->vaddr + p->memsz;
                });
            };
            for (const ProgramHeader* p : loads) {
                if (p->filesz > p->memsz) {
                    Fail(file + ": invalid segment size");
                }
                CheckSpan(m.elf.size(), p->offset, p->filesz);
                std::memcpy(image.data() + base + p->vaddr, m.elf.data() + p->offset, p->filesz);
                segments.push_back({base + p->vaddr, p->memsz, p->flags});
            }
            const ProgramHeader* tls = FindHeader(m, PT_TLS);
            uint64_t module_id = 0;
            if (tls) {
                if (tls->filesz > tls->memsz || !mapped(tls->vaddr, tls->memsz)) {
                    Fail(file + ": unsupported TLS layout");
                }
                module_id = tls_module++;
            }
            uint64_t tls_relocations = 0;
            for (const Relocation& r : m.relocs) {
                if (!mapped(r.target)) {
                    Fail(file + ": relocation outside image " + Hex(r.target));
                }
                const uint64_t target = r.target + base;
                if (r.kind == 8) {
                    relocs.push_back({target, 0, int64_t(base + r.addend), 0});
                } else if (r.kind == 16) {
                    const Symbol& symbol = SymbolOf(m, r);
                    if (!symbol.name.empty() || symbol.type != 3 || r.addend || !module_id) {
                        Fail(file + ": unsupported external TLS relocation");
                    }
                    Put64(image, target, module_id);
                    ++tls_relocations;
                } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                    const Symbol& symbol = SymbolOf(m, r);
                    if (symbol.section) {
                        if (symbol.section == 0xfff1) {
                            Put64(image, target, symbol.value + r.addend);
                        } else {
                            relocs.push_back({target, 0, int64_t(base + symbol.value + r.addend), 0});
                        }
                    } else if (!symbol.identity && symbol.binding == 2) {
                        Put64(image, target, 0); // weak undefined (module_start/stop)
                    } else {
                        if (!symbol.identity) {
                            Fail(file + ": unlinkable import '" + symbol.name + "'");
                        }
                        AddImport(symbol, r, target, file + ": ");
                    }
                } else {
                    Fail(file + ": unsupported relocation type " + std::to_string(r.kind));
                }
            }
            uint64_t count = 0;
            for (const Symbol& s : m.symbols) {
                if (s.section && (s.binding == 1 || s.binding == 2) && s.identity && (s.type == 1 || s.type == 2)) {
                    if (!mapped(s.value, std::max<uint64_t>(1, s.size))) {
                        Fail(file + ": export outside load segments");
                    }
                    const Export value{base + s.value, s.type == 1 ? kKindData : kKindCode};
                    if (!exports.emplace(s.identity->Key(), value).second) {
                        Fail(file + ": ambiguous export");
                    }
                    by_nid[s.identity->nid + '\0' + s.identity->library.name].push_back(value);
                    ++count;
                }
            }
            fs_patched += PatchFsLoads(image, m.ph, base);
            table.push_back({file, base, modsize, base + (m.tags.count(DT_INIT) ? m.tags.at(DT_INIT) : 0),
                             tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0, tls ? tls->filesz : 0, module_id,
                             tls_relocations, count, m.sha256});
            base = (base + modsize + 65535) & ~uint64_t(65535);
        }
    }

    Bytes image;
    std::vector<std::array<uint64_t, 3>> segments;
    std::vector<OutRelocation> relocs;
    std::vector<Identity> imported;
    std::unordered_map<std::string, size_t> import_index;
    std::unordered_map<std::string, Export> exports;
    std::unordered_map<std::string, std::vector<Export>> by_nid;
    std::vector<ModuleEntry> table;
    int fs_patched = 0;

private:
    static const Symbol& SymbolOf(const Module& m, const Relocation& r) {
        if (r.symbol >= m.symbols.size()) {
            Fail("relocation symbol out of range");
        }
        return m.symbols[r.symbol];
    }

    void AddImport(const Symbol& symbol, const Relocation& r, uint64_t target, const std::string& where) {
        const uint64_t kind = symbol.type == 1 ? kKindData : kKindCode;
        if ((kind == kKindCode && r.addend) || r.addend < 0 || r.addend >= 4096) {
            Fail(where + "unsupported import addend");
        }
        const auto [it, added] = import_index.emplace(symbol.identity->Key(), imported.size());
        if (added) {
            imported.push_back(*symbol.identity);
        }
        relocs.push_back({target, kind, int64_t(it->second), r.addend});
    }
};

// Python's json.dumps(indent=2) for the report: the same text, so the file matches prepare_image.py's.
class Json {
public:
    static Json Str(std::string v) { Json j; j.text_ = Quote(v); return j; }
    static Json Int(uint64_t v) { Json j; j.text_ = std::to_string(v); return j; }
    static Json Bool(bool v) { Json j; j.text_ = v ? "true" : "false"; return j; }
    static Json Null() { Json j; j.text_ = "null"; return j; }
    static Json Array() { Json j; j.kind_ = 'a'; return j; }
    static Json Object() { Json j; j.kind_ = 'o'; return j; }
    Json& Add(Json v) { items_.emplace_back("", std::move(v)); return *this; }
    Json& Add(std::string key, Json v) { items_.emplace_back(std::move(key), std::move(v)); return *this; }

    std::string Dump(int depth = 0) const {
        if (!kind_) {
            return text_;
        }
        const char open = kind_ == 'a' ? '[' : '{', close = kind_ == 'a' ? ']' : '}';
        if (items_.empty()) {
            return std::string{open, close};
        }
        const std::string inner(size_t(depth + 1) * 2, ' ');
        std::string out(1, open);
        for (size_t i = 0; i < items_.size(); ++i) {
            out += (i ? ",\n" : "\n") + inner;
            if (kind_ == 'o') {
                out += Quote(items_[i].first) + ": ";
            }
            out += items_[i].second.Dump(depth + 1);
        }
        return out + "\n" + std::string(size_t(depth) * 2, ' ') + close;
    }

private:
    static std::string Quote(const std::string& v) {
        std::string out = "\"";
        for (const unsigned char c : v) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c >= 0x80) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out + "\"";
    }

    char kind_ = 0;
    std::string text_;
    std::vector<std::pair<std::string, Json>> items_;
};

// Counter.most_common(): by count, highest first; equal counts keep their first-seen order.
Json CountByLibrary(const std::vector<std::string>& names) {
    std::vector<std::pair<std::string, uint64_t>> counts;
    for (const std::string& name : names) {
        const std::string library = name.substr(name.find('#') + 1);
        const auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == library; });
        if (it == counts.end()) {
            counts.emplace_back(library, 1);
        } else {
            ++it->second;
        }
    }
    std::stable_sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    Json object = Json::Object();
    for (const auto& [library, count] : counts) {
        object.Add(library, Json::Int(count));
    }
    return object;
}

Json StringList(const std::vector<std::string>& values) {
    Json array = Json::Array();
    for (const std::string& value : values) {
        array.Add(Json::Str(value));
    }
    return array;
}

template <typename T> void Append(Bytes& out, const T& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), std::streamsize(size));
    if (!out.flush()) {
        Fail("cannot write " + path.filename().string());
    }
}

void Build(const std::filesystem::path& game, const std::filesystem::path& out) {
    const Module main = ReadModule(game / "eboot.bin");
    const bool init_env_ret = LibcInitEnvIsRet(ReadModule(game / "sce_module" / "libc.prx"));
    ImageBuilder builder;
    builder.MapEboot(main);
    const uint64_t eboot_size = builder.image.size();
    builder.LinkModules(game, main);

    std::vector<std::string> names;
    for (const Identity& identity : builder.imported) {
        names.push_back(identity.Canonical());
    }
    std::vector<std::array<uint64_t, 3>> bindings;
    std::vector<std::string> unresolved;
    for (size_t index = 0; index < builder.imported.size(); ++index) {
        const Identity& identity = builder.imported[index];
        std::optional<Export> found;
        if (const auto it = builder.exports.find(identity.Key()); it != builder.exports.end()) {
            found = it->second;
        } else if (identity.library.name == "libSceLibcInternal") {
            const auto candidates = builder.by_nid.find(identity.nid + '\0' + "libc");
            if (candidates != builder.by_nid.end() && candidates->second.size() == 1) {
                found = candidates->second.front();
            }
        }
        if (found) {
            bindings.push_back({index, found->address, found->kind});
        } else {
            unresolved.push_back(names[index]);
        }
    }

    const ProgramHeader* main_tls = FindHeader(main, PT_TLS);
    const std::array<uint64_t, 4> tls_values =
        main_tls ? std::array<uint64_t, 4>{main_tls->vaddr, main_tls->filesz, main_tls->memsz, main_tls->align}
                 : std::array<uint64_t, 4>{};
    if (main_tls && (main_tls->filesz > main_tls->memsz || main_tls->memsz > 1024 * 1024 ||
                     main_tls->vaddr + main_tls->filesz > eboot_size)) {
        Fail("unsupported eboot TLS layout");
    }
    const ProgramHeader& procparam = RequireHeader(main, PT_SCE_PROCPARAM);

    std::string sfo;
    {
        const Bytes raw = ReadFile(game / "sce_sys" / "param.sfo");
        sfo.assign(raw.begin(), raw.end());
    }
    uint32_t params[4];
    for (int i = 0; i < 4; ++i) {
        const std::optional<uint32_t> value =
            SfoInteger(sfo, ("USER_DEFINED_PARAM_" + std::to_string(i + 1)).c_str(), 0);
        if (!value) {
            Fail("invalid user-defined parameter");
        }
        params[i] = *value;
    }

    Bytes boot;
    boot.insert(boot.end(), {'D', 'C', 'P', 'B', 'O', 'O', 'T', '1'});
    for (const uint64_t value : {uint64_t(builder.image.size()), main.entry, uint64_t(builder.segments.size()),
                                 uint64_t(builder.relocs.size()), uint64_t(names.size()), uint64_t(init_env_ret),
                                 procparam.vaddr, tls_values[0], tls_values[1], tls_values[2], tls_values[3],
                                 uint64_t(builder.table.size())}) {
        Append(boot, value);
    }
    for (const ModuleEntry& t : builder.table) {
        for (const uint64_t value : {t.base, t.size, t.init, t.tls_address, t.tls_memsz, t.tls_filesz, t.tls_module}) {
            Append(boot, value);
        }
    }
    Append(boot, uint64_t(bindings.size()));
    for (const auto& binding : bindings) {
        Append(boot, binding);
    }
    for (const auto& segment : builder.segments) {
        Append(boot, segment);
    }
    for (const std::string& name : names) {
        if (name.size() >= 128) {
            Fail("import name too long");
        }
        std::array<char, 128> field{};
        std::memcpy(field.data(), name.data(), name.size());
        Append(boot, field);
    }
    for (const OutRelocation& r : builder.relocs) {
        Append(boot, r);
    }
    boot.insert(boot.end(), builder.image.begin(), builder.image.end());

    const ProgramHeader* sdk = FindHeader(main, PT_SCE_PROCPARAM);
    Json modules = Json::Array();
    for (const ModuleEntry& t : builder.table) {
        modules.Add(Json::Object()
                        .Add("file", Json::Str(t.file))
                        .Add("base", Json::Str(Hex(t.base)))
                        .Add("size", Json::Int(t.size))
                        .Add("init", Json::Str(Hex(t.init)))
                        .Add("tls_address", Json::Str(Hex(t.tls_address)))
                        .Add("tls_memsz", Json::Int(t.tls_memsz))
                        .Add("tls_filesz", Json::Int(t.tls_filesz))
                        .Add("tls_module", Json::Int(t.tls_module))
                        .Add("tls_relocations", Json::Int(t.tls_relocations))
                        .Add("exports", Json::Int(t.exports))
                        .Add("sha256", Json::Str(t.sha256)));
    }
    const std::string title_id = SfoString(sfo, "TITLE_ID"), app_ver = SfoString(sfo, "APP_VER");
    Json report = Json::Object();
    report.Add("title_id", title_id.empty() ? Json::Null() : Json::Str(title_id))
        .Add("app_ver", app_ver.empty() ? Json::Null() : Json::Str(app_ver))
        .Add("eboot_sha256", Json::Str(main.sha256))
        .Add("entry", Json::Str(Hex(main.entry)))
        .Add("image_bytes", Json::Int(builder.image.size()))
        .Add("eboot_image_bytes", Json::Int(eboot_size))
        .Add("sdk_version", Json::Str(Hex(sdk ? Read<uint64_t>(main.elf, sdk->offset + 16) : 0)))
        .Add("needed_modules", StringList(main.needed))
        .Add("init_env_is_ret", Json::Bool(init_env_ret))
        .Add("fs_loads_patched", Json::Int(uint64_t(builder.fs_patched)))
        .Add("modules", std::move(modules))
        .Add("main_tls", Json::Object()
                             .Add("vaddr", Json::Int(tls_values[0]))
                             .Add("filesz", Json::Int(tls_values[1]))
                             .Add("memsz", Json::Int(tls_values[2]))
                             .Add("align", Json::Int(tls_values[3])))
        .Add("imports", Json::Int(names.size()))
        .Add("native_bindings", Json::Int(bindings.size()))
        .Add("unresolved", Json::Int(unresolved.size()))
        .Add("imports_by_library", CountByLibrary(names))
        .Add("unresolved_by_library", CountByLibrary(unresolved))
        .Add("unresolved_imports", StringList(unresolved))
        .Add("import_list", StringList(names));
    std::string json = report.Dump() + "\n";
    for (size_t at = json.find('\n'); at != std::string::npos; at = json.find('\n', at + 2)) {
        json.insert(at, 1, '\r'); // prepare_image.py writes the report in Windows text mode
    }

    Bytes content;
    content.insert(content.end(), {'D', 'C', 'P', 'C', 'O', 'N', 'T', '1'});
    Append(content, uint32_t(3)); // the full game SKU
    for (const uint32_t param : params) {
        Append(content, param);
    }

    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    WriteFile(out / "boot.bin", boot.data(), boot.size());
    WriteFile(out / "image.json", json.data(), json.size());
    WriteFile(out / "content.bin", content.data(), content.size());
    if (const std::string atlas = BuildAtlasSignatures(game, out); !atlas.empty()) {
        std::fprintf(stderr, "Atlas signatures not written: %s\n", atlas.c_str());
    }
}

} // namespace

std::string BuildBootImage(const std::filesystem::path& game, const std::filesystem::path& out) {
    try {
        Build(game, out);
        return {};
    } catch (const Failure& failure) {
        return failure.what();
    } catch (const std::exception& error) {
        return std::string("unexpected error: ") + error.what();
    }
}

} // namespace dcp

extern "C" int dcp_build_boot_image(const char* app0, const char* out, char* error, size_t error_size) {
    const std::string failure = dcp::BuildBootImage(dcp::PathFromUtf8(app0), dcp::PathFromUtf8(out));
    if (error && error_size) {
        std::snprintf(error, error_size, "%s", failure.c_str());
    }
    return failure.empty() ? 0 : 1;
}
