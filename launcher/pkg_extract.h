// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Unpacks a PS4 fake package (fPKG) into a game folder: shadPS4's PKG extractor (src/core/file_format/pkg.cpp,
// pfs.h, pkg_type.cpp and src/core/crypto, removed upstream in "code: Remove fpkg code"), with crypto.h (CNG or OpenSSL) in place of
// Crypto++. scripts/pkg_extract.py follows the same code and writes the same files.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dcp {

struct PackageResult {
    enum Code { kOk, kNotPackage, kUnsupported, kIncomplete, kReadFailed, kWriteFailed, kCancelled };
    Code code = kOk;
    std::string detail; // for the log, not the UI
    bool ok() const { return code == kOk; }
};

struct PackageFile {
    std::string path;
    uint64_t size = 0;
};

class Package {
public:
    Package();
    ~Package();
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;

    // The header, the entry table and param.sfo.
    PackageResult Open(const std::filesystem::path& path);
    // The keys and the inner file system's file list. Packages encrypted for a console are kUnsupported.
    PackageResult LoadFiles();
    // The metadata entries into sce_sys, then every file of the package, into `out`. `progress` returns false to
    // cancel.
    PackageResult Extract(const std::filesystem::path& out,
                          const std::function<bool(uint64_t done, uint64_t total)>& progress);

    // The files of the inner file system, after LoadFiles.
    std::vector<PackageFile> Files() const;
    // One file's bytes, read without writing anything; kIncomplete when it is absent or larger than `max_bytes`.
    PackageResult ReadFile(const std::string& path, uint64_t max_bytes, std::vector<uint8_t>& out);

    // `length` bytes of one file from `offset`, read without writing anything.
    PackageResult ReadFileRange(const std::string& path, uint64_t offset, size_t length, std::vector<uint8_t>& out);

    const std::string& content_id() const;
    const std::string& sfo() const;
    uint64_t total_bytes() const;
    size_t file_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dcp
