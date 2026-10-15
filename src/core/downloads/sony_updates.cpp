// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <pugixml.hpp>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/crypto/crypto.h"
#include "core/downloads/http_client.h"
#include "core/downloads/sony_updates.h"

namespace Core::Downloads::SonyUpdates {

namespace {

// Published HMAC key the PS4 uses to build update URLs (see PS4-Updates-Python, MIT).
constexpr std::array<u8, 32> UpdateUrlKey = {
    0xAD, 0x62, 0xE3, 0x7F, 0x90, 0x5E, 0x06, 0xBC, 0x19, 0x59, 0x31, 0x42, 0x28, 0x1C, 0x11, 0x2C,
    0xEC, 0x0E, 0x7E, 0xC3, 0xE9, 0x7E, 0xFD, 0xCA, 0xEF, 0xCD, 0xBA, 0xAF, 0xA6, 0x37, 0x8D, 0x84,
};

std::string UpdateBase() {
    const char* value = std::getenv("SHADPS4_PKG_SONY_UPDATE_BASE");
    return (value && *value) ? std::string{value}
                             : std::string{"http://gs-sec.ww.np.dl.playstation.net/plo/np"};
}

struct Piece {
    std::string url;
    u64 offset = 0;
    u64 size = 0;
    std::string hash;
};

struct EvpDeleter {
    void operator()(EVP_MD_CTX* ctx) const {
        EVP_MD_CTX_free(ctx);
    }
};

class Hasher {
public:
    explicit Hasher(size_t hex_length)
        : ctx(EVP_MD_CTX_new()), md(hex_length == 64 ? EVP_sha256() : EVP_sha1()) {
        EVP_DigestInit_ex(ctx.get(), md, nullptr);
    }

    void Update(const void* data, size_t size) {
        EVP_DigestUpdate(ctx.get(), data, size);
    }

    std::string FinishHex() {
        std::array<u8, EVP_MAX_MD_SIZE> digest{};
        unsigned int length = 0;
        EVP_DigestFinal_ex(ctx.get(), digest.data(), &length);
        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < length; i++) {
            out.push_back(hex[digest[i] >> 4]);
            out.push_back(hex[digest[i] & 0xF]);
        }
        return out;
    }

private:
    std::unique_ptr<EVP_MD_CTX, EvpDeleter> ctx;
    const EVP_MD* md;
};

std::string ToLower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool HashMatches(const std::string& expected, const std::string& actual) {
    return expected.empty() || ToLower(expected) == actual;
}

std::string HashExistingRange(Common::FS::IOFile& file, const Piece& piece,
                              DownloadProgress& progress, const std::atomic<bool>* cancel) {
    Hasher hasher(piece.hash.size());
    std::vector<u8> buffer(4 * 1024 * 1024);
    file.Seek(static_cast<s64>(piece.offset));
    u64 remaining = piece.size;
    while (remaining > 0) {
        if (cancel && *cancel) {
            return {};
        }
        const size_t chunk = static_cast<size_t>(std::min<u64>(remaining, buffer.size()));
        const size_t read = file.ReadRaw<u8>(buffer.data(), chunk);
        if (read != chunk) {
            return {};
        }
        hasher.Update(buffer.data(), chunk);
        remaining -= chunk;
        progress.bytes_done += chunk;
    }
    return hasher.FinishHex();
}

std::string OutputName(const UpdateInfo& info) {
    auto name = info.manifest_url.substr(info.manifest_url.rfind('/') + 1);
    if (name.ends_with(".json")) {
        name.resize(name.size() - 5);
    }
    for (auto& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    if (name.empty()) {
        name = info.content_id.empty() ? info.title_id : info.content_id;
    }
    return name + ".pkg";
}

} // namespace

bool IsValidTitleId(const std::string& title_id) {
    if (title_id.size() != 9) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        if (!std::isupper(static_cast<unsigned char>(title_id[i]))) {
            return false;
        }
    }
    for (size_t i = 4; i < 9; i++) {
        if (!std::isdigit(static_cast<unsigned char>(title_id[i]))) {
            return false;
        }
    }
    return true;
}

std::string UpdateXmlUrl(const std::string& title_id) {
    const std::string message = "np_" + title_id;
    const auto hash = Crypto::HmacSha256Hex(
        UpdateUrlKey,
        std::span<const u8>(reinterpret_cast<const u8*>(message.data()), message.size()));
    return fmt::format("{}/{}/{}/{}-ver.xml", UpdateBase(), title_id, hash, title_id);
}

UpdateInfo Lookup(const std::string& raw_title_id) {
    UpdateInfo info;
    std::string title_id;
    for (const char c : raw_title_id) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            title_id.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    info.title_id = title_id;

    if (!IsValidTitleId(title_id)) {
        info.error = "Enter a title ID like CUSA00900 (4 letters, 5 digits).";
        return info;
    }

    const auto response = Get(UpdateXmlUrl(title_id));
    if (response.status == 404 || (response.Ok() && response.body.empty())) {
        info.error = fmt::format("Sony has no update published for {}.", title_id);
        return info;
    }
    if (!response.Ok()) {
        info.error = fmt::format("Could not reach Sony's update server: {}", response.error);
        return info;
    }

    pugi::xml_document doc;
    if (!doc.load_buffer(response.body.data(), response.body.size())) {
        info.error = "Sony's update server sent a response that isn't valid XML.";
        return info;
    }

    pugi::xml_node best;
    double best_version = -1.0;
    for (const auto& tag : doc.child("titlepatch").children("tag")) {
        for (const auto& package : tag.children("package")) {
            double version = 0.0;
            try {
                version = std::stod(package.attribute("version").value());
            } catch (...) {
            }
            if (version > best_version) {
                best_version = version;
                best = package;
            }
        }
    }
    if (!best) {
        info.error = fmt::format("Sony has no update package listed for {}.", title_id);
        return info;
    }

    info.found = true;
    info.version = best.attribute("version").value();
    info.content_id = best.attribute("content_id").value();
    info.manifest_url = best.attribute("manifest_url").value();
    info.digest = best.attribute("digest").value();
    info.type = best.attribute("type").value();
    info.size = best.attribute("size").as_ullong();
    info.system_ver = best.attribute("system_ver").as_uint();
    info.title = best.child("paramsfo").child("title").text().get();
    if (info.title.empty()) {
        info.title = title_id;
    }
    if (info.manifest_url.empty()) {
        info.found = false;
        info.error = "The update entry has no download manifest.";
    }
    return info;
}

DownloadResult Download(const UpdateInfo& info, const std::filesystem::path& out_dir,
                        DownloadProgress& progress, const std::atomic<bool>* cancel) {
    DownloadResult result;
    progress.bytes_done = 0;
    progress.bytes_total = 0;
    progress.piece = 0;
    progress.pieces = 0;
    progress.verifying = false;

    if (!info.found) {
        result.message = "Look up an update first.";
        return result;
    }

    const auto manifest = Get(info.manifest_url);
    if (!manifest.Ok()) {
        result.message = fmt::format("Could not get the download manifest: {}", manifest.error);
        return result;
    }

    std::vector<Piece> pieces;
    u64 total = 0;
    try {
        const auto json = nlohmann::json::parse(manifest.body);
        for (const auto& item : json.at("pieces")) {
            Piece piece;
            piece.url = item.at("url").get<std::string>();
            piece.offset = item.at("fileOffset").get<u64>();
            piece.size = item.at("fileSize").get<u64>();
            piece.hash = item.value("hashValue", std::string{});
            pieces.push_back(std::move(piece));
        }
        total = json.value("originalFileSize", u64{0});
    } catch (const std::exception& e) {
        result.message = fmt::format("The download manifest is malformed: {}", e.what());
        return result;
    }
    if (pieces.empty()) {
        result.message = "The download manifest lists no files.";
        return result;
    }
    if (total == 0) {
        for (const auto& piece : pieces) {
            total = std::max(total, piece.offset + piece.size);
        }
    }
    progress.bytes_total = total;
    progress.pieces = static_cast<u32>(pieces.size());

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        result.message = "Could not create " + Common::FS::PathToUTF8String(out_dir);
        return result;
    }
    result.file = out_dir / OutputName(info);

    const u64 existing =
        std::filesystem::exists(result.file, ec) ? std::filesystem::file_size(result.file, ec) : 0;
    const auto space = std::filesystem::space(out_dir, ec);
    if (!ec && total > existing && space.available < total - existing) {
        result.message = fmt::format("Not enough disk space: {} needed, {} free.",
                                     FormatSize(total - existing), FormatSize(space.available));
        return result;
    }

    if (!std::filesystem::exists(result.file, ec)) {
        Common::FS::IOFile create(result.file, Common::FS::FileAccessMode::Create);
        if (!create.IsOpen()) {
            result.message = "Could not create " + Common::FS::PathToUTF8String(result.file);
            return result;
        }
    }
    Common::FS::IOFile file(result.file, Common::FS::FileAccessMode::ReadWrite);
    if (!file.IsOpen()) {
        result.message = "Could not open " + Common::FS::PathToUTF8String(result.file);
        return result;
    }

    for (size_t i = 0; i < pieces.size(); i++) {
        const auto& piece = pieces[i];
        progress.piece = static_cast<u32>(i + 1);

        // Resume: keep a piece that's already fully on disk with the right hash.
        if (!piece.hash.empty() && file.GetSize() >= piece.offset + piece.size) {
            progress.verifying = true;
            const u64 before = progress.bytes_done;
            const auto existing_hash = HashExistingRange(file, piece, progress, cancel);
            progress.verifying = false;
            if (cancel && *cancel) {
                result.message = "Cancelled. Download again to resume.";
                return result;
            }
            if (HashMatches(piece.hash, existing_hash)) {
                continue;
            }
            progress.bytes_done = before;
        }

        Hasher hasher(piece.hash.size());
        u64 written = 0;
        bool write_failed = false;
        file.Seek(static_cast<s64>(piece.offset));
        const u64 piece_start = progress.bytes_done;

        const auto response = Stream(
            piece.url,
            [&](std::span<const char> chunk) {
                if (written + chunk.size() > piece.size) {
                    write_failed = true;
                    return false;
                }
                if (file.WriteRaw<char>(chunk.data(), chunk.size()) != chunk.size()) {
                    write_failed = true;
                    return false;
                }
                hasher.Update(chunk.data(), chunk.size());
                written += chunk.size();
                progress.bytes_done = piece_start + written;
                return true;
            },
            nullptr, cancel);

        if (cancel && *cancel) {
            result.message = "Cancelled. Download again to resume.";
            return result;
        }
        if (!response.Ok() || write_failed) {
            result.message = write_failed ? "Could not write the PKG (disk full or size mismatch)."
                                          : fmt::format("Download of piece {} failed: {}", i + 1,
                                                        response.error);
            return result;
        }
        if (written != piece.size) {
            result.message = fmt::format("Piece {} was incomplete ({} of {} bytes).", i + 1,
                                         written, piece.size);
            return result;
        }
        if (!HashMatches(piece.hash, hasher.FinishHex())) {
            result.message = fmt::format("Piece {} failed its checksum. Download again.", i + 1);
            return result;
        }
    }

    file.Flush();
    if (file.GetSize() > total && total != 0) {
        file.SetSize(total);
    }

    result.success = true;
    result.message =
        fmt::format("Saved {} ({})", Common::FS::PathToUTF8String(result.file), FormatSize(total));
    LOG_INFO(Loader, "Sony update download finished: {}", result.message);
    return result;
}

std::string FormatSize(u64 bytes) {
    constexpr std::array units{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        unit++;
    }
    return unit == 0 ? fmt::format("{} {}", bytes, units[unit])
                     : fmt::format("{:.2f} {}", value, units[unit]);
}

} // namespace Core::Downloads::SonyUpdates
