// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <zlib.h>
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/file_format/pkg.h"
#include "core/file_format/pkg_type.h"

namespace {

constexpr u32 PkgMagic = 0x7F434E54;
constexpr u32 PfscMagic = 0x43534650;
constexpr u64 PfsBlockSize = 0x10000;

bool DecompressPFSC(std::span<char> compressed_data, std::span<char> decompressed_data) {
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return false;
    }
    stream.avail_in = static_cast<uInt>(compressed_data.size());
    stream.next_in = reinterpret_cast<unsigned char*>(compressed_data.data());
    stream.avail_out = static_cast<uInt>(decompressed_data.size());
    stream.next_out = reinterpret_cast<unsigned char*>(decompressed_data.data());

    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    return result == Z_STREAM_END || result == Z_OK || result == Z_BUF_ERROR;
}

bool ReadBlock(std::span<char> compressed, std::vector<char>& decompressed) {
    if (compressed.size() == PfsBlockSize) {
        std::memcpy(decompressed.data(), compressed.data(), PfsBlockSize);
        return true;
    }
    if (compressed.size() < PfsBlockSize) {
        return DecompressPFSC(compressed, decompressed);
    }
    return false;
}

constexpr u64 DirentHeaderSize = offsetof(Dirent, name);

Dirent ReadDirent(const std::vector<char>& block, u64 offset) {
    Dirent dirent{};
    std::memcpy(&dirent, &block[offset], std::min<u64>(sizeof(Dirent), block.size() - offset));
    return dirent;
}

std::filesystem::path Utf8Path(std::string_view name) {
    return std::filesystem::path(std::u8string(name.begin(), name.end()));
}

u64 GetPFSCOffset(std::span<const u8> pfs_image) {
    u32 value;
    for (u64 i = 0x20000; i + sizeof(u32) <= pfs_image.size(); i += 0x10000) {
        std::memcpy(&value, &pfs_image[i], sizeof(u32));
        if (value == PfscMagic) {
            return i;
        }
    }
    return std::numeric_limits<u64>::max();
}

} // namespace

PKG::PKG() = default;

PKG::~PKG() = default;

bool PKG::Open(const std::filesystem::path& filepath, std::string& failreason) {
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        failreason = "Could not open the PKG file";
        return false;
    }
    pkgSize = file.GetSize();
    if (pkgSize < sizeof(PKGHeader)) {
        failreason = "File is too small to be a PKG";
        return false;
    }

    file.Read(pkgheader);
    if (pkgheader.magic != PkgMagic) {
        failreason = "File is not a PS4 PKG (bad magic)";
        return false;
    }

    pkgFlags.clear();
    for (const auto& flag : flagNames) {
        if (isFlagSet(pkgheader.pkg_content_flags, flag.first)) {
            if (!pkgFlags.empty())
                pkgFlags += (", ");
            pkgFlags += (flag.second);
        }
    }

    // The title ID starts 7 characters into the content ID at offset 0x40.
    file.Seek(0x47);
    file.Read(pkgTitleID);

    const u32 offset = pkgheader.pkg_table_entry_offset;
    const u32 n_files = pkgheader.pkg_table_entry_count;

    if (offset >= pkgSize || !file.Seek(offset)) {
        failreason = "Failed to seek to PKG table entry offset";
        return false;
    }

    for (u32 i = 0; i < n_files; i++) {
        PKGEntry entry{};
        file.Read(entry.id);
        file.Read(entry.filename_offset);
        file.Read(entry.flags1);
        file.Read(entry.flags2);
        file.Read(entry.offset);
        file.Read(entry.size);
        file.Seek(8, Common::FS::SeekOrigin::CurrentPosition);

        if (GetEntryNameByType(entry.id) == "param.sfo") {
            if (u64{entry.offset} + entry.size > pkgSize) {
                failreason = "param.sfo lies outside the PKG";
                return false;
            }
            const auto current = file.Tell();
            sfo.resize(entry.size);
            file.Seek(entry.offset);
            file.ReadRaw<u8>(sfo.data(), entry.size);
            file.Seek(current);
        }
    }

    if (sfo.empty()) {
        failreason = "PKG has no param.sfo";
        return false;
    }
    return true;
}

bool PKG::Extract(const std::filesystem::path& filepath, const std::filesystem::path& extract,
                  std::string& failreason) {
    extract_path = extract;
    pkgpath = filepath;
    extractPaths.clear();
    fsTable.clear();
    iNodeBuf.clear();
    sectorMap.clear();

    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        failreason = "Could not open the PKG file";
        return false;
    }
    pkgSize = file.GetSize();
    if (pkgSize < sizeof(PKGHeader)) {
        failreason = "File is too small to be a PKG";
        return false;
    }
    file.ReadRaw<u8>(&pkgheader, sizeof(PKGHeader));

    if (pkgheader.magic != PkgMagic) {
        failreason = "File is not a PS4 PKG (bad magic)";
        return false;
    }
    if (pkgheader.pkg_size > pkgSize) {
        failreason = "PKG is truncated (file is smaller than its header says)";
        return false;
    }
    if ((pkgheader.pkg_content_size + pkgheader.pkg_content_offset) > pkgheader.pkg_size) {
        failreason = "Content size is bigger than pkg size";
        return false;
    }

    const u32 offset = pkgheader.pkg_table_entry_offset;
    const u32 n_files = pkgheader.pkg_table_entry_count;

    std::array<u8, 64> concatenated_ivkey_dk3;
    std::array<u8, 32> seed_digest;
    std::array<std::array<u8, 32>, 7> digest1;
    std::array<std::array<u8, 256>, 7> key1;
    std::array<u8, 256> imgkeydata;

    if (offset >= pkgSize || !file.Seek(offset)) {
        failreason = "Failed to seek to PKG table entry offset";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(extract_path / "sce_sys", ec);
    if (ec) {
        failreason = "Could not create the install folder: " + ec.message();
        return false;
    }

    for (u32 i = 0; i < n_files; i++) {
        PKGEntry entry{};
        file.Read(entry.id);
        file.Read(entry.filename_offset);
        file.Read(entry.flags1);
        file.Read(entry.flags2);
        file.Read(entry.offset);
        file.Read(entry.size);
        file.Seek(8, Common::FS::SeekOrigin::CurrentPosition);

        const auto currentPos = file.Tell();

        if (u64{entry.offset} + entry.size > pkgSize) {
            failreason = "A PKG entry lies outside the file";
            return false;
        }

        const auto name = GetEntryNameByType(entry.id);
        const auto out_path = name.empty() ? extract_path / "sce_sys" / std::to_string(entry.id)
                                           : extract_path / "sce_sys" / Utf8Path(name);
        std::filesystem::create_directories(out_path.parent_path(), ec);

        if (entry.id == 0x10) { // ENTRY_KEYS
            file.Seek(entry.offset);
            file.Read(seed_digest);
            for (int k = 0; k < 7; k++) {
                file.Read(digest1[k]);
            }
            for (int k = 0; k < 7; k++) {
                file.Read(key1[k]);
            }
            crypto.RSA2048Decrypt(dk3_, key1[3], true);
        } else if (entry.id == 0x20) { // IMAGE_KEY
            file.Seek(entry.offset);
            file.Read(imgkeydata);

            std::memcpy(concatenated_ivkey_dk3.data(), &entry, sizeof(entry));
            std::memcpy(concatenated_ivkey_dk3.data() + sizeof(entry), dk3_.data(), sizeof(dk3_));

            crypto.ivKeyHASH256(concatenated_ivkey_dk3, ivKey);
            crypto.aesCbcCfb128Decrypt(ivKey, imgkeydata, imgKey);
            crypto.RSA2048Decrypt(ekpfsKey, imgKey, false);
        }

        std::vector<u8> data(entry.size);
        file.Seek(entry.offset);
        file.ReadRaw<u8>(data.data(), entry.size);

        // NP entries are encrypted with the derived DK3; store them decrypted.
        if (!name.empty() &&
            (entry.id == 0x400 || entry.id == 0x401 || entry.id == 0x402 || entry.id == 0x403)) {
            decNp.resize(entry.size);
            std::array<u8, 64> concatenated_ivkey_dk3_;
            std::memcpy(concatenated_ivkey_dk3_.data(), &entry, sizeof(entry));
            std::memcpy(concatenated_ivkey_dk3_.data() + sizeof(entry), dk3_.data(), sizeof(dk3_));
            crypto.ivKeyHASH256(concatenated_ivkey_dk3_, ivKey);
            crypto.aesCbcCfb128DecryptEntry(ivKey, data, decNp);
            data = decNp;
        }

        Common::FS::IOFile out(out_path, Common::FS::FileAccessMode::Create);
        if (!out.IsOpen()) {
            failreason = "Could not write " + Common::FS::PathToUTF8String(out_path);
            return false;
        }
        out.WriteRaw<u8>(data.data(), data.size());
        out.Close();

        file.Seek(currentPos);
    }

    if (pkgheader.pfs_image_offset + 0x380 > pkgSize) {
        failreason = "PFS image lies outside the PKG";
        return false;
    }

    std::array<u8, 16> seed;
    file.Seek(pkgheader.pfs_image_offset + 0x370);
    file.Read(seed);

    crypto.PfsGenCryptoKey(ekpfsKey, seed, dataKey, tweakKey);
    const u64 length = u64{pkgheader.pfs_cache_size} * 0x2;

    u64 num_blocks = 0;
    std::vector<u8> pfsc(length);
    if (length != 0) {
        if (pkgheader.pfs_image_offset + length > pkgSize) {
            failreason = "PFS image lies outside the PKG";
            return false;
        }
        std::vector<u8> pfs_encrypted(length);
        file.Seek(pkgheader.pfs_image_offset);
        file.Read(pfs_encrypted);

        std::vector<u8> pfs_decrypted(length);
        crypto.decryptPFS(dataKey, tweakKey, pfs_encrypted, pfs_decrypted, 0);

        pfsc_offset = GetPFSCOffset(pfs_decrypted);
        if (pfsc_offset == std::numeric_limits<u64>::max()) {
            failreason = "Could not decrypt the PKG's file system. Only fake (fPKG) and homebrew "
                         "PKGs can be installed; retail PKGs need console keys.";
            return false;
        }
        pfsc.resize(length - pfsc_offset);
        std::memcpy(pfsc.data(), pfs_decrypted.data() + pfsc_offset, pfsc.size());
        if (pfsc.size() < sizeof(PFSCHdr)) {
            failreason = "Corrupt PFSC header";
            return false;
        }

        PFSCHdr pfsChdr;
        std::memcpy(&pfsChdr, pfsc.data(), sizeof(pfsChdr));
        if (pfsChdr.block_sz2 <= 0 || pfsChdr.data_length < 0 || pfsChdr.block_offsets < 0) {
            failreason = "Corrupt PFSC header";
            return false;
        }

        num_blocks = static_cast<u64>(pfsChdr.data_length / pfsChdr.block_sz2);
        const u64 block_table = static_cast<u64>(pfsChdr.block_offsets);
        if (block_table + (num_blocks + 1) * 8 > pfsc.size()) {
            failreason = "Corrupt PFSC block table";
            return false;
        }
        sectorMap.resize(num_blocks + 1);
        for (u64 i = 0; i < num_blocks + 1; i++) {
            std::memcpy(&sectorMap[i], pfsc.data() + block_table + i * 8, 8);
        }
    }

    u32 ent_size = 0;
    u32 ndinode = 0;
    u32 ndinode_counter = 0;
    bool dinode_reached = false;
    bool uroot_reached = false;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(PfsBlockSize);

    for (u64 i = 0; i < num_blocks; i++) {
        const u64 sectorOffset = sectorMap[i];
        if (sectorMap[i + 1] < sectorOffset || sectorMap[i + 1] - sectorOffset > PfsBlockSize) {
            failreason = "Corrupt PFSC sector map";
            return false;
        }
        const u64 sectorSize = sectorMap[i + 1] - sectorOffset;

        // Metadata usually sits in the decrypted cache; later blocks are read from the file.
        if (sectorMap[i + 1] <= pfsc.size()) {
            compressedData.assign(pfsc.begin() + sectorOffset, pfsc.begin() + sectorMap[i + 1]);
        } else if (!ReadPfscRange(file, sectorOffset, sectorSize, compressedData)) {
            failreason = "Could not read the PKG's file table";
            return false;
        }
        if (!ReadBlock(compressedData, decompressedData)) {
            failreason = "Could not decompress the PKG's file table";
            return false;
        }

        if (i == 0) {
            std::memcpy(&ndinode, decompressedData.data() + 0x30, 4);
        }

        u64 occupied_blocks = (u64{ndinode} * 0xA8) / PfsBlockSize;
        if (((u64{ndinode} * 0xA8) % PfsBlockSize) != 0)
            occupied_blocks += 1;

        if (i >= 1 && i <= occupied_blocks) {
            for (u64 p = 0; p + sizeof(Inode) <= PfsBlockSize; p += 0xA8) {
                Inode node;
                std::memcpy(&node, &decompressedData[p], sizeof(node));
                if (node.Mode == 0) {
                    break;
                }
                iNodeBuf.push_back(node);
            }
        }

        // Root entries; sometimes there are more than two.
        const std::string_view flat_path_table(&decompressedData[0x10], 15);
        if (flat_path_table == "flat_path_table") {
            uroot_reached = true;
        }

        if (uroot_reached) {
            for (u64 j = 0; j + DirentHeaderSize <= PfsBlockSize; j += ent_size) {
                const Dirent dirent = ReadDirent(decompressedData, j);
                ent_size = static_cast<u32>(dirent.entsize);
                if (dirent.ino != 0) {
                    ndinode_counter++;
                    if (dirent.entsize <= 0) {
                        break;
                    }
                } else {
                    const auto parent_path = extract_path.parent_path();
                    const std::string title_id{GetTitleID()};

                    if (parent_path.filename() != title_id &&
                        !Common::FS::PathToUTF8String(extract_path).ends_with("-UPDATE")) {
                        extractPaths[ndinode_counter] = parent_path / title_id;
                    } else {
                        // DLCs and separate updates use the given folder as their root.
                        extractPaths[ndinode_counter] = extract_path;
                    }
                    uroot_reached = false;
                    break;
                }
            }
        }

        const char dot = decompressedData[0x10];
        const std::string_view dotdot(&decompressedData[0x28], 2);
        if (dot == '.' && dotdot == "..") {
            dinode_reached = true;
        }

        bool end_reached = false;
        if (dinode_reached) {
            for (u64 j = 0; j + DirentHeaderSize <= PfsBlockSize; j += ent_size) {
                const Dirent dirent = ReadDirent(decompressedData, j);

                if (dirent.ino == 0 || dirent.entsize <= 0) {
                    break;
                }

                ent_size = static_cast<u32>(dirent.entsize);
                const u64 max_name =
                    std::min<u64>(sizeof(dirent.name), PfsBlockSize - j - DirentHeaderSize);
                auto& table = fsTable.emplace_back();
                table.name = std::string(dirent.name, std::clamp<u64>(dirent.namelen, 0, max_name));
                table.inode = dirent.ino;
                table.type = dirent.type;

                if (table.type == PFS_CURRENT_DIR) {
                    current_dir = extractPaths[table.inode];
                }
                extractPaths[table.inode] = current_dir / Utf8Path(table.name);

                if (table.type == PFS_FILE || table.type == PFS_DIR) {
                    if (table.type == PFS_DIR) {
                        std::filesystem::create_directories(extractPaths[table.inode], ec);
                    }
                    ndinode_counter++;
                    if ((ndinode_counter + 1) == ndinode) // 1 for the image itself (root).
                        end_reached = true;
                }
            }
            if (end_reached) {
                break;
            }
        }
    }
    return true;
}

bool PKG::ExtractFiles(const int index) {
    if (index < 0 || static_cast<size_t>(index) >= fsTable.size()) {
        return false;
    }
    const u32 inode_number = fsTable[index].inode;
    const u32 inode_type = fsTable[index].type;

    if (inode_type != PFS_FILE) {
        return true;
    }
    if (inode_number >= iNodeBuf.size()) {
        return false;
    }

    const u64 sector_loc = iNodeBuf[inode_number].loc;
    const u64 nblocks = iNodeBuf[inode_number].Blocks;
    const u64 bsize = static_cast<u64>(std::max<s64>(iNodeBuf[inode_number].Size, 0));
    if (sector_loc + nblocks + 1 > sectorMap.size()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(extractPaths[inode_number].parent_path(), ec);

    Common::FS::IOFile inflated;
    inflated.Open(extractPaths[inode_number], Common::FS::FileAccessMode::Create);
    if (!inflated.IsOpen()) {
        return false;
    }

    Common::FS::IOFile pkgFile;
    pkgFile.Open(pkgpath, Common::FS::FileAccessMode::Read);
    if (!pkgFile.IsOpen()) {
        return false;
    }

    u64 size_decompressed = 0;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(PfsBlockSize);

    for (u64 j = 0; j < nblocks; j++) {
        const u64 sectorOffset = sectorMap[sector_loc + j];
        if (sectorMap[sector_loc + j + 1] < sectorOffset) {
            return false;
        }
        const u64 sectorSize = sectorMap[sector_loc + j + 1] - sectorOffset;
        if (!ReadPfscRange(pkgFile, sectorOffset, sectorSize, compressedData) ||
            !ReadBlock(compressedData, decompressedData)) {
            return false;
        }

        size_decompressed += PfsBlockSize;

        if (j < nblocks - 1) {
            inflated.WriteRaw<u8>(decompressedData.data(), decompressedData.size());
        } else {
            // Drop the zero padding at the end of the last block.
            const u64 overshoot = size_decompressed > bsize ? size_decompressed - bsize : 0;
            const u64 write_size = overshoot < PfsBlockSize ? PfsBlockSize - overshoot : 0;
            inflated.WriteRaw<u8>(decompressedData.data(), write_size);
        }
    }
    return true;
}

bool PKG::ReadPfscRange(const Common::FS::IOFile& file, u64 offset, u64 size,
                        std::vector<char>& out) {
    if (size > PfsBlockSize) {
        return false;
    }
    const u64 image_offset = pfsc_offset + offset;
    const u64 aligned = image_offset & ~u64{0xFFF};
    const u64 lead = image_offset - aligned;
    const u64 span = (lead + size + 0xFFF) & ~u64{0xFFF};
    if (pkgheader.pfs_image_offset + aligned + span > pkgSize + 0x1000) {
        return false;
    }

    std::vector<u8> encrypted(span);
    std::vector<u8> decrypted(span);
    file.Seek(static_cast<s64>(pkgheader.pfs_image_offset + aligned));
    file.ReadRaw<u8>(encrypted.data(), encrypted.size());
    crypto.decryptPFS(dataKey, tweakKey, encrypted, decrypted, aligned / 0x1000);

    out.assign(reinterpret_cast<const char*>(decrypted.data()) + lead,
               reinterpret_cast<const char*>(decrypted.data()) + lead + size);
    return true;
}
