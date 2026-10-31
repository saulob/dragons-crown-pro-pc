// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from shadPS4 d339b3f7 (the parent of be22674f, which removed it); see pkg_extract.h.
#include "pkg_extract.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

#include "crypto.h"
#include "utf8.h"

namespace dcp {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kPkgMagic = 0x7F434E54;
constexpr uint32_t kPfscMagic = 0x43534650; // "PFSC"
constexpr uint32_t kEntryKeys = 0x10, kImageKey = 0x20, kParamSfo = 0x1000;
// Lower ids are the package's own digests and keys, not game files.
constexpr uint32_t kFirstMetadataEntry = 0x400;
constexpr uint32_t kEntryEncrypted = 0x80000000;
constexpr uint32_t kFakeImageKeyIndex = 3;
constexpr size_t kMaxEntries = 4096;
constexpr uint64_t kXtsSector = 0x1000;
constexpr uint64_t kBlockSize = 0x10000;
constexpr size_t kInodeSize = 0xA8;
constexpr uint64_t kChunkBlocks = 256;
constexpr int32_t kPfsFile = 2, kPfsDir = 3;

// The public fake-package RSA keysets of shadPS4's keys.h.
constexpr uint8_t kFakeKeysetModulus[] = {
    0xC6, 0xCF, 0x71, 0xE7, 0xE5, 0x9A, 0xF0, 0xD1, 0x2A, 0x2C, 0x45, 0x8B, 0xF9, 0x2A, 0x0E, 0xC1,
    0x43, 0x05, 0x8B, 0xC3, 0x71, 0x17, 0x80, 0x1D, 0xCD, 0x49, 0x7D, 0xDE, 0x35, 0x9D, 0x25, 0x9B,
    0xA0, 0xD7, 0xA0, 0xF2, 0x7D, 0x6C, 0x08, 0x7E, 0xAA, 0x55, 0x02, 0x68, 0x2B, 0x23, 0xC6, 0x44,
    0xB8, 0x44, 0x18, 0xEB, 0x56, 0xCF, 0x16, 0xA2, 0x48, 0x03, 0xC9, 0xE7, 0x4F, 0x87, 0xEB, 0x3D,
    0x30, 0xC3, 0x15, 0x88, 0xBF, 0x20, 0xE7, 0x9D, 0xFF, 0x77, 0x0C, 0xDE, 0x1D, 0x24, 0x1E, 0x63,
    0xA9, 0x4F, 0x8A, 0xBF, 0x5B, 0xBE, 0x60, 0x19, 0x68, 0x33, 0x3B, 0xFC, 0xED, 0x9F, 0x47, 0x4E,
    0x5F, 0xF8, 0xEA, 0xCB, 0x3D, 0x00, 0xBD, 0x67, 0x01, 0xF9, 0x2C, 0x6D, 0xC6, 0xAC, 0x13, 0x64,
    0xE7, 0x67, 0x14, 0xF3, 0xDC, 0x52, 0x69, 0x6A, 0xB9, 0x83, 0x2C, 0x42, 0x30, 0x13, 0x1B, 0xB2,
    0xD8, 0xA5, 0x02, 0x0D, 0x79, 0xED, 0x96, 0xB1, 0x0D, 0xF8, 0xCC, 0x0C, 0xDF, 0x81, 0x95, 0x4F,
    0x03, 0x58, 0x09, 0x57, 0x0E, 0x80, 0x69, 0x2E, 0xFE, 0xFF, 0x52, 0x77, 0xEA, 0x75, 0x28, 0xA8,
    0xFB, 0xC9, 0xBE, 0xBF, 0x9F, 0xBB, 0xB7, 0x79, 0x8E, 0x18, 0x05, 0xE1, 0x80, 0xBD, 0x50, 0x34,
    0x94, 0x81, 0xD3, 0x53, 0xC2, 0x69, 0xA2, 0xD2, 0x4C, 0xCF, 0x6C, 0xF4, 0x57, 0x2C, 0x10, 0x4A,
    0x3F, 0xFB, 0x22, 0xFD, 0x8B, 0x97, 0xE2, 0xC9, 0x5B, 0xA6, 0x2B, 0xCD, 0xD6, 0x1B, 0x6B, 0xDB,
    0x68, 0x7F, 0x4B, 0xC2, 0xA0, 0x50, 0x34, 0xC0, 0x05, 0xE5, 0x8D, 0xEF, 0x24, 0x67, 0xFF, 0x93,
    0x40, 0xCF, 0x2D, 0x62, 0xA2, 0xA0, 0x50, 0xB1, 0xF1, 0x3A, 0xA8, 0x3D, 0xFD, 0x80, 0xD1, 0xF9,
    0xB8, 0x05, 0x22, 0xAF, 0xC8, 0x35, 0x45, 0x90, 0x58, 0x8E, 0xE3, 0x3A, 0x7C, 0xBD, 0x3E, 0x27,
};
constexpr uint8_t kFakeKeysetPrime1[] = {
    0xFE, 0xF6, 0xBF, 0x1D, 0x69, 0xAB, 0x16, 0x25, 0x08, 0x47, 0x55, 0x6B, 0x86, 0xE4, 0x35, 0x88,
    0x72, 0x2A, 0xB1, 0x3D, 0xF8, 0xB6, 0x44, 0xCA, 0xB3, 0xAB, 0x19, 0xD1, 0x04, 0x24, 0x28, 0x0A,
    0x74, 0x55, 0xB8, 0x15, 0x45, 0x09, 0xCC, 0x13, 0x1C, 0xF2, 0xBA, 0x37, 0xA9, 0x03, 0x90, 0x8F,
    0x02, 0x10, 0xFF, 0x25, 0x79, 0x86, 0xCC, 0x18, 0x50, 0x9A, 0x10, 0x5F, 0x5B, 0x4C, 0x1C, 0x4E,
    0xB0, 0xA7, 0xE3, 0x59, 0xB1, 0x2D, 0xA0, 0xC6, 0xB0, 0x20, 0x2C, 0x21, 0x33, 0x12, 0xB3, 0xAF,
    0x72, 0x34, 0x83, 0xCD, 0x52, 0x2F, 0xAF, 0x0F, 0x20, 0x5A, 0x1B, 0xC0, 0xE2, 0xA3, 0x76, 0x34,
    0x0F, 0xD7, 0xFC, 0xC1, 0x41, 0xC9, 0xF9, 0x79, 0x40, 0x17, 0x42, 0x21, 0x3E, 0x9D, 0xFD, 0xC7,
    0xC1, 0x50, 0xDE, 0x44, 0x5A, 0xC9, 0x31, 0x89, 0x6A, 0x78, 0x05, 0xBE, 0x65, 0xB4, 0xE8, 0x2D,
};
constexpr uint8_t kFakeKeysetPrime2[] = {
    0xC7, 0x9E, 0x47, 0x58, 0x00, 0x7D, 0x62, 0x82, 0xB0, 0xD2, 0x22, 0x81, 0xD4, 0xA8, 0x97, 0x1B,
    0x79, 0x0C, 0x3A, 0xB0, 0xD7, 0xC9, 0x30, 0xE3, 0xC3, 0x53, 0x8E, 0x57, 0xEF, 0xF0, 0x9B, 0x9F,
    0xB3, 0x90, 0x52, 0xC6, 0x94, 0x22, 0x36, 0xAA, 0xE6, 0x4A, 0x5F, 0x72, 0x1D, 0x70, 0xE8, 0x76,
    0x58, 0xC8, 0xB2, 0x91, 0xCE, 0x9C, 0xC3, 0xE9, 0x09, 0x7F, 0x2E, 0x47, 0x97, 0xCC, 0x90, 0x39,
    0x15, 0x35, 0x31, 0xDE, 0x1F, 0x0C, 0x8C, 0x0D, 0xC1, 0xC2, 0x92, 0xBE, 0x97, 0xBF, 0x2F, 0x91,
    0xA1, 0x8C, 0x7D, 0x50, 0xA8, 0x21, 0x2F, 0xD7, 0xA2, 0x9A, 0x7E, 0xB5, 0xA7, 0x2A, 0x90, 0x02,
    0xD9, 0xF3, 0x3D, 0xD1, 0xEB, 0xB8, 0xE0, 0x5A, 0x79, 0x9E, 0x7D, 0x8D, 0xCA, 0x18, 0x6D, 0xBD,
    0x9E, 0xA1, 0x80, 0x28, 0x6B, 0x2A, 0xFE, 0x51, 0x24, 0x9B, 0x6F, 0x4D, 0x84, 0x77, 0x80, 0x23,
};
constexpr uint8_t kPkgDerivedKey3KeysetModulus[] = {
    0xD2, 0x12, 0xFC, 0x33, 0x5F, 0x6D, 0xDB, 0x83, 0x16, 0x09, 0x62, 0x8B, 0x03, 0x56, 0x27, 0x37,
    0x82, 0xD4, 0x77, 0x85, 0x35, 0x29, 0x39, 0x2D, 0x52, 0x6B, 0x8C, 0x4C, 0x8C, 0xFB, 0x06, 0xC1,
    0x84, 0x5B, 0xE7, 0xD4, 0xF7, 0xBC, 0xD2, 0x4E, 0x62, 0x45, 0xCD, 0x2A, 0xBB, 0xD7, 0x77, 0x76,
    0x45, 0x36, 0x55, 0x27, 0x3F, 0xB3, 0xF5, 0xF9, 0x8E, 0xDA, 0x4B, 0xEF, 0xAA, 0x59, 0xAE, 0xB3,
    0x9B, 0xEA, 0x54, 0x98, 0xD2, 0x06, 0x32, 0x6A, 0x58, 0x31, 0x2A, 0xE0, 0xD4, 0x4F, 0x90, 0xB5,
    0x0A, 0x7D, 0xEC, 0xF4, 0x3A, 0x9C, 0x52, 0x67, 0x2D, 0x99, 0x31, 0x8E, 0x0C, 0x43, 0xE6, 0x82,
    0xFE, 0x07, 0x46, 0xE1, 0x2E, 0x50, 0xD4, 0x1F, 0x2D, 0x2F, 0x7E, 0xD9, 0x08, 0xBA, 0x06, 0xB3,
    0xBF, 0x2E, 0x20, 0x3F, 0x4E, 0x3F, 0xFE, 0x44, 0xFF, 0xAA, 0x50, 0x43, 0x57, 0x91, 0x69, 0x94,
    0x49, 0x15, 0x82, 0x82, 0xE4, 0x0F, 0x4C, 0x8D, 0x9D, 0x2C, 0xC9, 0x5B, 0x1D, 0x64, 0xBF, 0x88,
    0x8B, 0xD4, 0xC5, 0x94, 0xE7, 0x65, 0x47, 0x84, 0x1E, 0xE5, 0x79, 0x10, 0xFB, 0x98, 0x93, 0x47,
    0xB9, 0x7D, 0x85, 0x12, 0xA6, 0x40, 0x98, 0x2C, 0xF7, 0x92, 0xBC, 0x95, 0x19, 0x32, 0xED, 0xE8,
    0x90, 0x56, 0x0D, 0x65, 0xC1, 0xAA, 0x78, 0xC6, 0x2E, 0x54, 0xFD, 0x5F, 0x54, 0xA1, 0xF6, 0x7E,
    0xE5, 0xE0, 0x5F, 0x61, 0xC1, 0x20, 0xB4, 0xB9, 0xB4, 0x33, 0x08, 0x70, 0xE4, 0xDF, 0x89, 0x56,
    0xED, 0x01, 0x29, 0x46, 0x77, 0x5F, 0x8C, 0xB8, 0xA9, 0xF5, 0x1E, 0x2E, 0xB3, 0xB9, 0xBF, 0xE0,
    0x09, 0xB7, 0x8D, 0x28, 0xD4, 0xA6, 0xC3, 0xB8, 0x1E, 0x1F, 0x07, 0xEB, 0xB4, 0x12, 0x0B, 0x95,
    0xB8, 0x85, 0x30, 0xFD, 0xDC, 0x39, 0x13, 0xD0, 0x7C, 0xDC, 0x8F, 0xED, 0xF9, 0xC9, 0xA3, 0xC1,
};
constexpr uint8_t kPkgDerivedKey3KeysetPrime1[] = {
    0xF9, 0x67, 0xAD, 0x99, 0x12, 0x31, 0x0C, 0x56, 0xA2, 0x2E, 0x16, 0x1C, 0x46, 0xB3, 0x4D, 0x5B,
    0x43, 0xBE, 0x42, 0xA2, 0xF6, 0x86, 0x96, 0x80, 0x42, 0xC3, 0xC7, 0x3F, 0xC3, 0x42, 0xF5, 0x87,
    0x49, 0x33, 0x9F, 0x07, 0x5D, 0x6E, 0x2C, 0x04, 0xFD, 0xE3, 0xE1, 0xB2, 0xAE, 0x0A, 0x0C, 0xF0,
    0xC7, 0xA6, 0x1C, 0xA1, 0x63, 0x50, 0xC8, 0x09, 0x9C, 0x51, 0x24, 0x52, 0x6C, 0x5E, 0x5E, 0xBD,
    0x1E, 0x27, 0x06, 0xBB, 0xBC, 0x9E, 0x94, 0xE1, 0x35, 0xD4, 0x6D, 0xB3, 0xCB, 0x3C, 0x68, 0xDD,
    0x68, 0xB3, 0xFE, 0x6C, 0xCB, 0x8D, 0x82, 0x20, 0x76, 0x23, 0x63, 0xB7, 0xE9, 0x68, 0x10, 0x01,
    0x4E, 0xDC, 0xBA, 0x27, 0x5D, 0x01, 0xC1, 0x2D, 0x80, 0x5E, 0x2B, 0xAF, 0x82, 0x6B, 0xD8, 0x84,
    0xB6, 0x10, 0x52, 0x86, 0xA7, 0x89, 0x8E, 0xAE, 0x9A, 0xE2, 0x89, 0xC6, 0xF7, 0xD5, 0x87, 0xFB,
};
constexpr uint8_t kPkgDerivedKey3KeysetPrime2[] = {
    0xD7, 0xA1, 0x0F, 0x9A, 0x8B, 0xF2, 0xC9, 0x11, 0x95, 0x32, 0x9A, 0x8C, 0xF0, 0xD9, 0x40, 0x47,
    0xF5, 0x68, 0xA0, 0x0D, 0xBD, 0xC1, 0xFC, 0x43, 0x2F, 0x65, 0xF9, 0xC3, 0x61, 0x0F, 0x25, 0x77,
    0x54, 0xAD, 0xD7, 0x58, 0xAC, 0x84, 0x40, 0x60, 0x8D, 0x3F, 0xF3, 0x65, 0x89, 0x75, 0xB5, 0xC6,
    0x2C, 0x51, 0x1A, 0x2F, 0x1F, 0x22, 0xE4, 0x43, 0x11, 0x54, 0xBE, 0xC9, 0xB4, 0xC7, 0xB5, 0x1B,
    0x05, 0x0B, 0xBC, 0x56, 0x9A, 0xCD, 0x4A, 0xD9, 0x73, 0x68, 0x5E, 0x5C, 0xFB, 0x92, 0xB7, 0x8B,
    0x0D, 0xFF, 0xF5, 0x07, 0xCA, 0xB4, 0xC8, 0x9B, 0x96, 0x3C, 0x07, 0x9E, 0x3E, 0x6B, 0x2A, 0x11,
    0xF2, 0x8A, 0xB1, 0x8A, 0xD7, 0x2E, 0x1B, 0xA5, 0x53, 0x24, 0x06, 0xED, 0x50, 0xB8, 0x90, 0x67,
    0xB1, 0xE2, 0x41, 0xC6, 0x92, 0x01, 0xEE, 0x10, 0xF0, 0x61, 0xBB, 0xFB, 0xB2, 0x7D, 0x4A, 0x73,
};

struct RsaKeyset {
    const uint8_t* modulus;
    const uint8_t* prime1;
    const uint8_t* prime2;
};
constexpr RsaKeyset kFakeKeyset{kFakeKeysetModulus, kFakeKeysetPrime1, kFakeKeysetPrime2};
constexpr RsaKeyset kPkgDerivedKey3Keyset{kPkgDerivedKey3KeysetModulus, kPkgDerivedKey3KeysetPrime1,
                                          kPkgDerivedKey3KeysetPrime2};

// pkg_type.cpp's names for the metadata entries a game package carries; others are written under their number.
constexpr struct {
    uint32_t id;
    const char* name;
} kEntryNames[] = {
    {0x0400, "license.dat"},
    {0x0401, "license.info"},
    {0x0402, "nptitle.dat"},
    {0x0403, "npbind.dat"},
    {0x0404, "selfinfo.dat"},
    {0x0406, "imageinfo.dat"},
    {0x0407, "target-deltainfo.dat"},
    {0x0408, "origin-deltainfo.dat"},
    {0x0409, "psreserved.dat"},
    {0x1000, "param.sfo"},
    {0x1001, "playgo-chunk.dat"},
    {0x1002, "playgo-chunk.sha"},
    {0x1003, "playgo-manifest.xml"},
    {0x1004, "pronunciation.xml"},
    {0x1005, "pronunciation.sig"},
    {0x1006, "pic1.png"},
    {0x1007, "pubtoolinfo.dat"},
    {0x1008, "app/playgo-chunk.dat"},
    {0x1009, "app/playgo-chunk.sha"},
    {0x100A, "app/playgo-manifest.xml"},
    {0x100B, "shareparam.json"},
    {0x100C, "shareoverlayimage.png"},
    {0x100D, "save_data.png"},
    {0x100E, "shareprivacyguardimage.png"},
    {0x1200, "icon0.png"},
    {0x1220, "pic0.png"},
    {0x1240, "snd0.at9"},
    {0x1280, "icon0.dds"},
    {0x12A0, "pic0.dds"},
    {0x12C0, "pic1.dds"},
    {0x1400, "trophy/trophy00.trp"},
};

// pfs.h
constexpr uint16_t kPfsSigned = 0x1, kPfsIs64Bit = 0x2;
constexpr uint32_t kInodeCompressed = 0x1;

struct PfsHeader {
    int64_t version, magic, id;
    uint8_t fmode, clean, read_only, rsv;
    uint16_t mode;
    int16_t unk1;
    int32_t block_size, n_backup;
    int64_t n_block, dinode_count, nd_block, dinode_block_count, superroot_ino;
};
static_assert(sizeof(PfsHeader) == 0x50);

struct PfscHeader {
    int32_t magic, unk4, unk8, block_sz;
    int64_t block_sz2, block_offsets;
    uint64_t data_start;
    int64_t data_length;
};
static_assert(sizeof(PfscHeader) == 0x30);

struct Inode {
    uint16_t mode, nlink;
    uint32_t flags;
    int64_t size, size_compressed;
    int64_t time_sec[4];
    uint32_t time_nsec[4];
    uint32_t uid, gid;
    uint64_t unk1, unk2;
    uint32_t blocks, loc;
};
static_assert(sizeof(Inode) == 0x68);

struct DirentHeader {
    int32_t ino, type, namelen, entsize;
};

struct Entry {
    uint32_t id, flags1, flags2, offset, size;
    uint8_t raw[32];
};

struct Item {
    std::string path;
    uint32_t inode;
    int32_t type;
};

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t Be64(const uint8_t* p) {
    return uint64_t(Be32(p)) << 32 | Be32(p + 4);
}

PackageResult Fail(PackageResult::Code code, std::string detail) {
    return {code, std::move(detail)};
}

std::string EntryName(uint32_t id) {
    for (const auto& entry : kEntryNames) {
        if (entry.id == id) {
            return entry.name;
        }
    }
    return std::to_string(id);
}

bool IsSafeName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    return std::none_of(name.begin(), name.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\' || c == ':';
    });
}

using crypto::AesKey;
using crypto::AesMode;

// RSAES-PKCS1-v1_5 with a 2048-bit private key; the message starts with the 32-byte key it carries.
bool RsaDecrypt(const RsaKeyset& keyset, const uint8_t* cipher, std::array<uint8_t, 32>& out) {
    Bytes plain;
    if (!crypto::RsaDecryptPkcs1(keyset.modulus, keyset.prime1, keyset.prime2, cipher, plain) || plain.size() < out.size()) {
        return false;
    }
    std::copy_n(plain.begin(), out.size(), out.begin());
    return true;
}

// AES-128-CBC with the IV and key from SHA-256(entry || dk3), in place. A partial last block stays as it is.
bool DecryptWithIvKey(const uint8_t (&ivkey)[32], uint8_t* data, size_t size) {
    const size_t whole = size & ~size_t(15);
    if (whole == 0) {
        return true;
    }
    AesKey key;
    if (!key.Init(AesMode::kCbc, ivkey + 16)) {
        return false;
    }
    uint8_t iv[16];
    std::memcpy(iv, ivkey, sizeof(iv));
    return key.Decrypt(data, whole, iv);
}

bool WriteWholeFile(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    return file && file.write(reinterpret_cast<const char*>(data), std::streamsize(size)).flush();
}

} // namespace

struct Package::Impl {
    std::ifstream file;
    uint64_t file_size = 0;
    std::string content_id, sfo;
    std::vector<Entry> entries;
    uint64_t pfs_offset = 0, pfs_size = 0, pfs_cache_size = 0;
    std::array<uint8_t, 32> dk3{}, ekpfs{};
    AesKey data_key, tweak_key;
    uint64_t pfsc_offset = 0, num_blocks = 0;
    std::vector<uint64_t> sector_map;
    std::vector<Inode> inodes;
    std::vector<Item> items;
    uint64_t total = 0;
    size_t files = 0;
    Bytes buffer, mask, tweaks;

    const Entry* Find(uint32_t id) const {
        for (const Entry& entry : entries) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    }

    bool ReadAt(uint64_t offset, void* data, size_t size) {
        if (offset > file_size || size > file_size - offset) {
            return false;
        }
        file.clear();
        file.seekg(std::streamoff(offset));
        return file.read(static_cast<char*>(data), std::streamsize(size)).gcount() == std::streamsize(size);
    }

    bool IvKey(const Entry& entry, uint8_t (&ivkey)[32]) const {
        uint8_t input[64];
        std::memcpy(input, entry.raw, 32);
        std::memcpy(input + 32, dk3.data(), 32);
        return crypto::Sha256(input, sizeof(input), ivkey);
    }

    // The outer PFS image: AES-XTS over 0x1000-byte sectors numbered from the start of the image (decryptPFS), with
    // the tweaks of a whole read computed first so CNG decrypts the read in one call.
    const uint8_t* ReadPfs(uint64_t offset, uint64_t size) {
        const uint64_t start = offset & ~(kXtsSector - 1);
        const uint64_t end = (offset + size + kXtsSector - 1) & ~(kXtsSector - 1);
        if (end > pfs_size || end <= start) {
            return nullptr;
        }
        const size_t length = size_t(end - start), sectors = length / kXtsSector;
        buffer.resize(length);
        mask.resize(length);
        tweaks.assign(sectors * 16, 0);
        if (!ReadAt(pfs_offset + start, buffer.data(), length)) {
            return nullptr;
        }
        for (size_t s = 0; s < sectors; ++s) {
            const uint64_t sector = start / kXtsSector + s;
            std::memcpy(&tweaks[s * 16], &sector, sizeof(sector));
        }
        if (!tweak_key.Encrypt(tweaks.data(), tweaks.size())) {
            return nullptr;
        }
        for (size_t s = 0; s < sectors; ++s) {
            uint64_t low, high;
            std::memcpy(&low, &tweaks[s * 16], 8);
            std::memcpy(&high, &tweaks[s * 16 + 8], 8);
            for (size_t block = 0; block < kXtsSector; block += 16) {
                std::memcpy(&mask[s * kXtsSector + block], &low, 8);
                std::memcpy(&mask[s * kXtsSector + block + 8], &high, 8);
                // xtsMult: the tweak times x in GF(2^128).
                const uint64_t carry = high >> 63;
                high = high << 1 | low >> 63;
                low = low << 1 ^ (carry ? 0x87 : 0);
            }
        }
        for (size_t i = 0; i < length; ++i) {
            buffer[i] ^= mask[i];
        }
        if (!data_key.Decrypt(buffer.data(), length)) {
            return nullptr;
        }
        for (size_t i = 0; i < length; ++i) {
            buffer[i] ^= mask[i];
        }
        return buffer.data() + (offset - start);
    }

    // Inner PFS blocks [first, first + count): zlib when shorter than a block, stored otherwise (DecompressPFSC).
    bool ReadBlocks(uint64_t first, uint64_t count, Bytes& out) {
        if (count == 0 || first > num_blocks || count > num_blocks - first) {
            return false;
        }
        const uint64_t low = sector_map[first], high = sector_map[first + count];
        if (high < low) {
            return false;
        }
        const uint8_t* data = ReadPfs(pfsc_offset + low, high - low);
        if (!data) {
            return false;
        }
        out.assign(size_t(count * kBlockSize), 0);
        for (uint64_t b = 0; b < count; ++b) {
            const uint64_t from = sector_map[first + b], to = sector_map[first + b + 1];
            if (to < from || to - from > kBlockSize) {
                return false;
            }
            uint8_t* dest = out.data() + b * kBlockSize;
            if (to - from == kBlockSize) {
                std::memcpy(dest, data + (from - low), kBlockSize);
                continue;
            }
            uLongf length = kBlockSize;
            if (uncompress(dest, &length, data + (from - low), uLong(to - from)) != Z_OK) {
                return false;
            }
        }
        return true;
    }

    bool ReadDir(uint32_t ino, std::vector<Item>& out) {
        if (ino >= inodes.size()) {
            return false;
        }
        Bytes data;
        if (!ReadBlocks(inodes[ino].loc, inodes[ino].blocks, data)) {
            return false;
        }
        for (size_t block = 0; block < data.size(); block += kBlockSize) {
            const size_t end = block + kBlockSize;
            for (size_t at = block; at + sizeof(DirentHeader) <= end;) {
                DirentHeader dirent;
                std::memcpy(&dirent, &data[at], sizeof(dirent));
                if (dirent.ino == 0 || dirent.entsize <= 0) {
                    break;
                }
                if (dirent.namelen < 0 || size_t(dirent.namelen) > end - at - sizeof(dirent)) {
                    return false;
                }
                out.push_back({std::string(reinterpret_cast<const char*>(&data[at + sizeof(dirent)]),
                                           size_t(dirent.namelen)),
                               uint32_t(dirent.ino), dirent.type});
                at += size_t(dirent.entsize);
            }
        }
        return true;
    }
};

Package::Package() : impl_(std::make_unique<Impl>()) {}
Package::~Package() = default;

std::vector<PackageFile> Package::Files() const {
    std::vector<PackageFile> files;
    for (const Item& item : impl_->items) {
        if (item.type == kPfsFile && item.inode < impl_->inodes.size()) {
            files.push_back({item.path, uint64_t(std::max<int64_t>(impl_->inodes[item.inode].size, 0))});
        }
    }
    return files;
}

PackageResult Package::ReadFile(const std::string& path, uint64_t max_bytes, std::vector<uint8_t>& out) {
    Impl& p = *impl_;
    out.clear();
    const auto item = std::find_if(p.items.begin(), p.items.end(),
                                   [&](const Item& candidate) { return candidate.type == kPfsFile && candidate.path == path; });
    if (item == p.items.end() || item->inode >= p.inodes.size()) {
        return Fail(PackageResult::kIncomplete, path + " is not in the package");
    }
    const Inode& node = p.inodes[item->inode];
    if (node.size < 0 || uint64_t(node.size) > max_bytes) {
        return Fail(PackageResult::kIncomplete, path + " is too large to read");
    }
    out.reserve(size_t(node.size));
    Bytes chunk;
    uint64_t remaining = uint64_t(node.size);
    for (uint64_t block = 0; remaining > 0; block += kChunkBlocks) {
        if (!p.ReadBlocks(node.loc + block, std::min<uint64_t>(kChunkBlocks, node.blocks - block), chunk)) {
            out.clear();
            return Fail(PackageResult::kReadFailed, "cannot read " + path);
        }
        const uint64_t length = std::min<uint64_t>(remaining, chunk.size());
        out.insert(out.end(), chunk.begin(), chunk.begin() + std::ptrdiff_t(length));
        remaining -= length;
    }
    return {};
}

PackageResult Package::ReadFileRange(const std::string& path, uint64_t offset, size_t length, std::vector<uint8_t>& out) {
    Impl& p = *impl_;
    out.clear();
    const auto item = std::find_if(p.items.begin(), p.items.end(),
                                   [&](const Item& candidate) { return candidate.type == kPfsFile && candidate.path == path; });
    if (item == p.items.end() || item->inode >= p.inodes.size()) {
        return Fail(PackageResult::kIncomplete, path + " is not in the package");
    }
    const Inode& node = p.inodes[item->inode];
    if (node.size < 0 || offset > uint64_t(node.size) || length > uint64_t(node.size) - offset) {
        return Fail(PackageResult::kIncomplete, "range outside " + path);
    }
    out.reserve(length);
    Bytes chunk;
    uint64_t at = offset;
    while (out.size() < length) {
        const uint64_t block = at / kBlockSize;
        const uint64_t count = std::min<uint64_t>(kChunkBlocks, node.blocks - block);
        if (!p.ReadBlocks(node.loc + block, count, chunk)) {
            out.clear();
            return Fail(PackageResult::kReadFailed, "cannot read " + path);
        }
        const uint64_t skip = at - block * kBlockSize;
        const uint64_t take = std::min<uint64_t>(length - out.size(), chunk.size() - skip);
        out.insert(out.end(), chunk.begin() + std::ptrdiff_t(skip), chunk.begin() + std::ptrdiff_t(skip + take));
        at += take;
    }
    return {};
}

const std::string& Package::content_id() const {
    return impl_->content_id;
}
const std::string& Package::sfo() const {
    return impl_->sfo;
}
uint64_t Package::total_bytes() const {
    return impl_->total;
}
size_t Package::file_count() const {
    return impl_->files;
}

PackageResult Package::Open(const fs::path& path) {
    Impl& p = *impl_;
    std::error_code ec;
    p.file_size = fs::file_size(path, ec);
    p.file.open(path, std::ios::binary);
    if (ec || !p.file) {
        return Fail(PackageResult::kReadFailed, "cannot open " + Utf8(path));
    }
    uint8_t header[0x1000];
    if (p.file_size < sizeof(header) || !p.ReadAt(0, header, sizeof(header)) || Be32(header) != kPkgMagic) {
        return Fail(PackageResult::kNotPackage, Utf8(path));
    }
    const char* content_id = reinterpret_cast<const char*>(header + 0x40);
    p.content_id.assign(content_id, strnlen(content_id, 0x24));
    const uint32_t entry_count = Be32(header + 0x10), table_offset = Be32(header + 0x18);
    const uint64_t content_offset = Be64(header + 0x30), content_size = Be64(header + 0x38);
    p.pfs_offset = Be64(header + 0x410);
    p.pfs_size = Be64(header + 0x418);
    const uint64_t pkg_size = Be64(header + 0x430);
    p.pfs_cache_size = Be32(header + 0x43C);

    // A download that stopped early is shorter than the size the header records.
    if (pkg_size > p.file_size) {
        return Fail(PackageResult::kIncomplete,
                    "package has " + std::to_string(p.file_size) + " of " + std::to_string(pkg_size) + " bytes");
    }
    if (content_offset + content_size > pkg_size || p.pfs_offset > p.file_size ||
        p.pfs_size > p.file_size - p.pfs_offset || entry_count > kMaxEntries) {
        return Fail(PackageResult::kReadFailed, "inconsistent package header");
    }

    Bytes table(size_t(entry_count) * 32);
    if (!p.ReadAt(table_offset, table.data(), table.size())) {
        return Fail(PackageResult::kReadFailed, "cannot read the entry table");
    }
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* raw = &table[size_t(i) * 32];
        Entry entry{Be32(raw), Be32(raw + 8), Be32(raw + 12), Be32(raw + 16), Be32(raw + 20), {}};
        std::memcpy(entry.raw, raw, sizeof(entry.raw));
        if (uint64_t(entry.offset) + entry.size > p.file_size) {
            return Fail(PackageResult::kReadFailed, "entry " + std::to_string(entry.id) + " is outside the package");
        }
        p.entries.push_back(entry);
    }
    const Entry* sfo = p.Find(kParamSfo);
    if (!sfo) {
        return Fail(PackageResult::kNotPackage, "package has no param.sfo");
    }
    p.sfo.resize(sfo->size);
    if (!p.ReadAt(sfo->offset, p.sfo.data(), p.sfo.size())) {
        return Fail(PackageResult::kReadFailed, "cannot read param.sfo");
    }
    return {};
}

PackageResult Package::LoadFiles() {
    Impl& p = *impl_;
    const Entry* keys = p.Find(kEntryKeys);
    const Entry* image_key = p.Find(kImageKey);
    if (!keys || !image_key || keys->size < 32 + 7 * 32 + 4 * 256 || image_key->size < 256) {
        return Fail(PackageResult::kUnsupported, "package has no key entries");
    }
    const uint32_t key_index = (image_key->flags2 >> 12) & 0xF;
    if (key_index != kFakeImageKeyIndex) {
        return Fail(PackageResult::kUnsupported, "image key index " + std::to_string(key_index) + ", not a fake package");
    }

    // ENTRY_KEYS: a seed digest, 7 digests and 7 RSA-encrypted keys, of which the fourth is dk3.
    uint8_t key3[256];
    if (!p.ReadAt(keys->offset + 32 + 7 * 32 + 3 * 256, key3, sizeof(key3)) ||
        !RsaDecrypt(kPkgDerivedKey3Keyset, key3, p.dk3)) {
        return Fail(PackageResult::kUnsupported, "cannot decrypt dk3");
    }
    uint8_t image_key_data[256], ivkey[32];
    if (!p.ReadAt(image_key->offset, image_key_data, sizeof(image_key_data)) || !p.IvKey(*image_key, ivkey) ||
        !DecryptWithIvKey(ivkey, image_key_data, sizeof(image_key_data)) ||
        !RsaDecrypt(kFakeKeyset, image_key_data, p.ekpfs)) {
        return Fail(PackageResult::kUnsupported, "cannot decrypt the image key");
    }

    // PfsGenCryptoKey: HMAC-SHA256(ekpfs, 1 || seed) is the tweak key, then the data key.
    uint8_t input[20], digest[32];
    const uint32_t index = 1;
    std::memcpy(input, &index, sizeof(index));
    if (!p.ReadAt(p.pfs_offset + 0x370, input + 4, 16)) {
        return Fail(PackageResult::kReadFailed, "cannot read the PFS seed");
    }
    if (!crypto::HmacSha256(p.ekpfs.data(), p.ekpfs.size(), input, sizeof(input), digest) ||
        !p.tweak_key.Init(AesMode::kEcb, digest) || !p.data_key.Init(AesMode::kEcb, digest + 16)) {
        return Fail(PackageResult::kUnsupported, "cannot create the PFS keys");
    }

    // GetPFSCOffset: the compressed image starts on a 0x10000 boundary after the outer header.
    const uint64_t limit = std::min(p.pfs_size, p.pfs_cache_size * 2);
    for (uint64_t at = 0x20000; at + 4 <= limit && !p.pfsc_offset; at += 0x10000) {
        const uint8_t* magic = p.ReadPfs(at, 4);
        if (!magic) {
            return Fail(PackageResult::kReadFailed, "cannot read the PFS image");
        }
        if (uint32_t value; std::memcpy(&value, magic, 4), value == kPfscMagic) {
            p.pfsc_offset = at;
        }
    }
    if (!p.pfsc_offset) {
        return Fail(PackageResult::kUnsupported, "no PFSC image (wrong keys?)");
    }
    PfscHeader pfsc;
    const uint8_t* pfsc_data = p.ReadPfs(p.pfsc_offset, sizeof(pfsc));
    if (!pfsc_data) {
        return Fail(PackageResult::kReadFailed, "cannot read the PFSC header");
    }
    std::memcpy(&pfsc, pfsc_data, sizeof(pfsc));
    if (pfsc.block_sz2 != int64_t(kBlockSize) || pfsc.data_length <= 0 || pfsc.block_offsets < 0) {
        return Fail(PackageResult::kUnsupported, "unexpected PFSC header");
    }
    p.num_blocks = uint64_t(pfsc.data_length) / kBlockSize;
    const uint8_t* map = p.ReadPfs(p.pfsc_offset + uint64_t(pfsc.block_offsets), (p.num_blocks + 1) * 8);
    if (!map) {
        return Fail(PackageResult::kReadFailed, "cannot read the PFSC block map");
    }
    p.sector_map.resize(p.num_blocks + 1);
    std::memcpy(p.sector_map.data(), map, p.sector_map.size() * 8);

    // The inner PFS: its header, the 0xA8-byte inodes that follow, then the directories from the super-root.
    Bytes block;
    if (!p.ReadBlocks(0, 1, block)) {
        return Fail(PackageResult::kReadFailed, "cannot read the PFS header");
    }
    PfsHeader header;
    std::memcpy(&header, block.data(), sizeof(header));
    if (header.block_size != int32_t(kBlockSize) || (header.mode & (kPfsSigned | kPfsIs64Bit)) ||
        header.dinode_count <= 0 || header.dinode_block_count <= 0 || header.superroot_ino < 0) {
        return Fail(PackageResult::kUnsupported, "unexpected PFS header");
    }
    if (!p.ReadBlocks(1, uint64_t(header.dinode_block_count), block)) {
        return Fail(PackageResult::kReadFailed, "cannot read the inodes");
    }
    for (size_t at = 0; at < block.size(); at += kBlockSize) {
        for (size_t offset = 0; offset + kInodeSize <= kBlockSize; offset += kInodeSize) {
            Inode node;
            std::memcpy(&node, &block[at + offset], sizeof(node));
            if (node.mode == 0) {
                break;
            }
            p.inodes.push_back(node);
        }
    }
    if (p.inodes.size() < uint64_t(header.dinode_count)) {
        return Fail(PackageResult::kReadFailed, "read " + std::to_string(p.inodes.size()) + " of " +
                                                    std::to_string(header.dinode_count) + " inodes");
    }

    std::vector<Item> root;
    if (!p.ReadDir(uint32_t(header.superroot_ino), root)) {
        return Fail(PackageResult::kReadFailed, "cannot read the super-root");
    }
    const auto uroot = std::find_if(root.begin(), root.end(), [](const Item& item) { return item.path == "uroot"; });
    if (uroot == root.end()) {
        return Fail(PackageResult::kReadFailed, "no uroot in the PFS");
    }
    std::vector<bool> visited(p.inodes.size());
    std::vector<std::pair<std::string, uint32_t>> pending{{std::string(), uroot->inode}};
    while (!pending.empty()) {
        const auto [prefix, ino] = pending.back();
        pending.pop_back();
        if (ino >= visited.size() || visited[ino]) {
            return Fail(PackageResult::kReadFailed, "PFS directory loop");
        }
        visited[ino] = true;
        std::vector<Item> children;
        if (!p.ReadDir(ino, children)) {
            return Fail(PackageResult::kReadFailed, "cannot read the directory " + prefix);
        }
        for (Item& child : children) {
            if (child.type != kPfsFile && child.type != kPfsDir) {
                continue;
            }
            if (!IsSafeName(child.path) || child.inode >= p.inodes.size()) {
                return Fail(PackageResult::kReadFailed, "invalid PFS entry in " + prefix);
            }
            child.path = prefix.empty() ? child.path : prefix + "/" + child.path;
            if (child.type == kPfsDir) {
                pending.emplace_back(child.path, child.inode);
            } else {
                const Inode& node = p.inodes[child.inode];
                if (node.flags & kInodeCompressed) {
                    return Fail(PackageResult::kUnsupported, child.path + " is PFS-compressed");
                }
                if (node.size < 0 || uint64_t(node.blocks) * kBlockSize < uint64_t(node.size) ||
                    uint64_t(node.loc) + node.blocks > p.num_blocks) {
                    return Fail(PackageResult::kReadFailed, child.path + " lies outside the PFS");
                }
                p.total += uint64_t(node.size);
                ++p.files;
            }
            p.items.push_back(std::move(child));
        }
    }
    std::sort(p.items.begin(), p.items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
    return {};
}

PackageResult Package::Extract(const fs::path& out, const std::function<bool(uint64_t, uint64_t)>& progress) {
    Impl& p = *impl_;
    // The metadata entries go to sce_sys first, as shadPS4 writes them; the file system's own copies replace them.
    for (const Entry& entry : p.entries) {
        if (entry.id < kFirstMetadataEntry) {
            continue;
        }
        Bytes data(entry.size);
        if (!p.ReadAt(entry.offset, data.data(), data.size())) {
            return Fail(PackageResult::kReadFailed, "cannot read entry " + std::to_string(entry.id));
        }
        uint8_t ivkey[32];
        if ((entry.flags1 & kEntryEncrypted) &&
            (!p.IvKey(entry, ivkey) || !DecryptWithIvKey(ivkey, data.data(), data.size()))) {
            return Fail(PackageResult::kUnsupported, "cannot decrypt entry " + std::to_string(entry.id));
        }
        const fs::path target = out / "sce_sys" / PathFromUtf8(EntryName(entry.id));
        if (!WriteWholeFile(target, data.data(), data.size())) {
            return Fail(PackageResult::kWriteFailed, "cannot write " + Utf8(target));
        }
    }

    std::error_code ec;
    uint64_t done = 0;
    Bytes chunk;
    for (const Item& item : p.items) {
        const fs::path target = out / PathFromUtf8(item.path);
        fs::create_directories(item.type == kPfsDir ? target : target.parent_path(), ec);
        if (ec) {
            return Fail(PackageResult::kWriteFailed, "cannot create " + Utf8(target) + ": " + ec.message());
        }
        if (item.type == kPfsDir) {
            continue;
        }
        const Inode& node = p.inodes[item.inode];
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Fail(PackageResult::kWriteFailed, "cannot create " + Utf8(target));
        }
        uint64_t remaining = uint64_t(node.size);
        for (uint64_t block = 0; remaining > 0; block += kChunkBlocks) {
            if (!p.ReadBlocks(node.loc + block, std::min<uint64_t>(kChunkBlocks, node.blocks - block), chunk)) {
                return Fail(PackageResult::kReadFailed, "cannot read " + item.path);
            }
            const uint64_t length = std::min<uint64_t>(remaining, chunk.size());
            if (!file.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(length))) {
                return Fail(PackageResult::kWriteFailed, "cannot write " + Utf8(target));
            }
            remaining -= length;
            done += length;
            if (progress && !progress(done, p.total)) {
                return Fail(PackageResult::kCancelled, {});
            }
        }
        if (!file.flush()) {
            return Fail(PackageResult::kWriteFailed, "cannot write " + Utf8(target));
        }
    }
    return {};
}

} // namespace dcp
