#include "file_browser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>

#include "utf8.h"

namespace fs = std::filesystem;

namespace dcp {
namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

bool HasExtension(const fs::path& name, std::string_view extension) {
    return extension.empty() || Lower(Utf8(name.extension())) == Lower(std::string(extension));
}

fs::path HomeFolder() {
    for (const char* variable : {"HOME", "USERPROFILE"}) {
        if (const char* value = std::getenv(variable); value && *value) {
            return PathFromUtf8(value);
        }
    }
    return {};
}

void AddIfFolder(std::vector<Place>& places, std::string label, const fs::path& path) {
    std::error_code ec;
    if (path.empty() || !fs::is_directory(path, ec)) {
        return;
    }
    for (const Place& place : places) {
        if (place.path == path) {
            return;
        }
    }
    places.push_back({std::move(label), path});
}

#ifndef _WIN32
void AddChildren(std::vector<Place>& places, const fs::path& parent) {
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_directory(type_ec) && !Utf8(it->path().filename()).starts_with('.')) {
            children.push_back(it->path());
        }
    }
    std::sort(children.begin(), children.end());
    for (const fs::path& child : children) {
        AddIfFolder(places, Utf8(child.filename()), child);
    }
}
#endif

} // namespace

FolderListing ListFolder(const fs::path& folder, std::string_view extension, bool show_hidden) {
    FolderListing listing;
    listing.folder = folder;
    std::error_code ec;
    fs::directory_iterator it(folder, ec), end;
    if (ec) {
        return listing;
    }
    listing.readable = true;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code type_ec;
        const fs::path name = it->path().filename();
        FolderEntry entry;
        entry.name = Utf8(name);
        if (entry.name.empty() || (!show_hidden && entry.name.front() == '.')) {
            continue;
        }
        entry.directory = it->is_directory(type_ec);
        if (!entry.directory) {
            if (!it->is_regular_file(type_ec) || !HasExtension(name, extension)) {
                continue;
            }
            const uintmax_t size = it->file_size(type_ec);
            entry.size = type_ec ? 0 : uint64_t(size);
        }
        listing.entries.push_back(std::move(entry));
    }
    std::sort(listing.entries.begin(), listing.entries.end(), [](const FolderEntry& a, const FolderEntry& b) {
        if (a.directory != b.directory) {
            return a.directory;
        }
        const std::string left = Lower(a.name), right = Lower(b.name);
        return left != right ? left < right : a.name < b.name;
    });
    return listing;
}

std::vector<Place> StandardPlaces(const fs::path& program_folder) {
    std::vector<Place> places;
    AddIfFolder(places, "Program folder", program_folder);
    const fs::path home = HomeFolder();
    if (!home.empty()) {
        AddIfFolder(places, "Home folder", home);
        for (const char* name : {"Downloads", "Desktop", "Documents"}) {
            AddIfFolder(places, name, home / name);
        }
    }
#ifndef _WIN32
    // SteamOS mounts the SD card at /run/media/<device>, other desktops at /run/media/<user>/<name> or /media/<user>.
    AddChildren(places, "/run/media");
    if (const char* user = std::getenv("USER"); user && *user) {
        AddChildren(places, fs::path("/run/media") / user);
        AddChildren(places, fs::path("/media") / user);
    }
    AddChildren(places, "/media");
    AddChildren(places, "/mnt");
    AddIfFolder(places, "Computer", "/");
#endif
    return places;
}

fs::path StartingFolder(const std::vector<Place>& places, std::string_view extension) {
    for (const Place& place : places) {
        const FolderListing listing = ListFolder(place.path, extension, false);
        for (const FolderEntry& entry : listing.entries) {
            if (!entry.directory) {
                return place.path;
            }
        }
    }
    return places.empty() ? fs::current_path() : places.front().path;
}

std::string FormatSize(uint64_t bytes) {
    char text[32];
    constexpr double kKilo = 1024.0;
    if (bytes >= (1ull << 30)) {
        std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / (kKilo * kKilo * kKilo));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (kKilo * kKilo));
    } else {
        std::snprintf(text, sizeof(text), "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    }
    return text;
}

std::string_view FileChooserPreference() {
    const char* value = std::getenv("DCP_FILE_CHOOSER");
    const std::string text = Lower(value ? value : "");
    return text == "builtin" ? "builtin" : text == "native" ? "native" : "";
}

} // namespace dcp
