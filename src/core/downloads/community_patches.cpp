// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/downloads/community_patches.h"
#include "core/downloads/http_client.h"

namespace Core::Downloads::CommunityPatches {

namespace {

const std::array<Repo, 2> PatchRepos{{
    {"GoldHEN", "illusion0001", "PS4-PS5-Game-Patch", "main", "patches/xml"},
    {"shadPS4", "shadps4-emu", "ps4_cheats", "main", "PATCHES"},
}};

std::string EnvOr(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string{value} : fallback;
}

std::string ApiBase() {
    return EnvOr("SHADPS4_PKG_GITHUB_API", "https://api.github.com");
}

std::string RawBase() {
    return EnvOr("SHADPS4_PKG_GITHUB_RAW", "https://raw.githubusercontent.com");
}

std::string UrlEncodePath(const std::string& path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::filesystem::path Utf8Path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string EnabledKey(const pugi::xml_node& metadata) {
    return std::string{metadata.attribute("Name").value()} + '\x1f' +
           metadata.attribute("AppVer").value() + '\x1f' + metadata.attribute("AppElf").value();
}

std::set<std::string> ReadEnabled(const std::filesystem::path& file) {
    std::set<std::string> enabled;
    pugi::xml_document doc;
    if (!doc.load_file(file.c_str())) {
        return enabled;
    }
    for (const auto& node : doc.child("Patch").children("Metadata")) {
        if (std::string_view{node.attribute("isEnabled").value()} == "true") {
            enabled.insert(EnabledKey(node));
        }
    }
    return enabled;
}

std::vector<std::string> ReadTitleIds(const pugi::xml_document& doc) {
    std::vector<std::string> ids;
    for (const auto& id : doc.child("Patch").child("TitleID").children("ID")) {
        ids.emplace_back(id.text().get());
    }
    return ids;
}

bool WriteIndex(const std::filesystem::path& folder, std::string& error) {
    nlohmann::json index = nlohmann::json::object();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".xml") {
            continue;
        }
        pugi::xml_document doc;
        if (!doc.load_file(entry.path().c_str())) {
            LOG_WARNING(Loader, "Skipping unreadable patch file {}",
                        Common::FS::PathToUTF8String(entry.path()));
            continue;
        }
        index[Common::FS::PathToUTF8String(entry.path().filename())] = ReadTitleIds(doc);
    }
    if (ec) {
        error = "Could not list " + Common::FS::PathToUTF8String(folder) + ": " + ec.message();
        return false;
    }

    std::ofstream out(folder / "files.json", std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not write files.json";
        return false;
    }
    out << index.dump(2);
    return static_cast<bool>(out);
}

} // namespace

std::span<const Repo> Repos() {
    return PatchRepos;
}

std::filesystem::path RepoFolder(const Repo& repo) {
    return Common::FS::GetUserPath(Common::FS::PathType::PatchesDir) / repo.name;
}

bool HasDownloadedPatches() {
    std::error_code ec;
    return std::ranges::any_of(PatchRepos, [&](const Repo& repo) {
        return std::filesystem::exists(RepoFolder(repo) / "files.json", ec);
    });
}

SyncResult SyncRepo(const Repo& repo, SyncProgress& progress, const std::atomic<bool>* cancel) {
    SyncResult result;
    progress.files_done = 0;
    progress.files_total = 0;

    const auto tree_url = fmt::format("{}/repos/{}/{}/git/trees/{}?recursive=1", ApiBase(),
                                      repo.owner, repo.repo, repo.branch);
    const auto listing = Get(tree_url);
    if (!listing.Ok()) {
        result.message =
            listing.status == 403
                ? "GitHub refused the request (rate limit). Try again in an hour."
                : fmt::format("Could not list {} patches: {}", repo.name, listing.error);
        return result;
    }

    std::vector<std::string> files;
    try {
        const auto json = nlohmann::json::parse(listing.body);
        const std::string prefix = repo.dir + "/";
        for (const auto& item : json.at("tree")) {
            const auto path = item.value("path", std::string{});
            if (item.value("type", std::string{}) != "blob" || !path.starts_with(prefix) ||
                !path.ends_with(".xml") || path.find('/', prefix.size()) != std::string::npos) {
                continue;
            }
            files.push_back(path);
        }
    } catch (const std::exception& e) {
        result.message = fmt::format("GitHub sent an unexpected file list: {}", e.what());
        return result;
    }
    if (files.empty()) {
        result.message = fmt::format("No patch files were found in {}", repo.name);
        return result;
    }

    const auto folder = RepoFolder(repo);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        result.message = "Could not create " + Common::FS::PathToUTF8String(folder);
        return result;
    }

    progress.files_total = static_cast<u32>(files.size());
    Session raw(RawBase());

    for (const auto& remote_path : files) {
        if (cancel && *cancel) {
            result.message = "Cancelled";
            return result;
        }

        const auto file_name = remote_path.substr(remote_path.rfind('/') + 1);
        const auto local = folder / Utf8Path(file_name);
        const auto keep_enabled = ReadEnabled(local);

        const auto response = raw.Get(UrlEncodePath(
            fmt::format("/{}/{}/{}/{}", repo.owner, repo.repo, repo.branch, remote_path)));
        pugi::xml_document doc;
        if (!response.Ok() || !doc.load_buffer(response.body.data(), response.body.size())) {
            LOG_WARNING(Loader, "Failed to download patch {}: {}", remote_path,
                        response.error.empty() ? "invalid XML" : response.error);
            result.failed++;
            progress.files_done++;
            continue;
        }

        for (auto node : doc.child("Patch").children("Metadata")) {
            const bool enabled = keep_enabled.contains(EnabledKey(node));
            auto attr = node.attribute("isEnabled");
            if (!attr) {
                attr = node.append_attribute("isEnabled");
            }
            attr.set_value(enabled ? "true" : "false");
        }

        auto temp = local;
        temp += ".tmp";
        if (!doc.save_file(temp.c_str(), "    ")) {
            result.failed++;
            progress.files_done++;
            continue;
        }
        std::filesystem::rename(temp, local, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            result.failed++;
        } else {
            result.files++;
        }
        progress.files_done++;
    }

    std::string index_error;
    if (!WriteIndex(folder, index_error)) {
        result.message = index_error;
        return result;
    }

    result.success = result.files > 0;
    result.message =
        result.failed == 0
            ? fmt::format("{}: {} patch files up to date", repo.name, result.files)
            : fmt::format("{}: {} downloaded, {} failed", repo.name, result.files, result.failed);
    LOG_INFO(Loader, "{}", result.message);
    return result;
}

std::vector<PatchEntry> ListPatchesFor(const std::string& serial) {
    std::vector<PatchEntry> entries;
    std::error_code ec;

    for (const auto& repo : PatchRepos) {
        const auto folder = RepoFolder(repo);
        std::ifstream index_file(folder / "files.json", std::ios::binary);
        if (!index_file) {
            continue;
        }
        nlohmann::json index;
        try {
            index = nlohmann::json::parse(index_file);
        } catch (...) {
            continue;
        }

        for (const auto& [file_name, serials] : index.items()) {
            if (!serials.is_array() ||
                std::find(serials.begin(), serials.end(), serial) == serials.end()) {
                continue;
            }
            const auto path = folder / Utf8Path(file_name);
            pugi::xml_document doc;
            if (!doc.load_file(path.c_str())) {
                continue;
            }
            u32 index_in_file = 0;
            for (const auto& node : doc.child("Patch").children("Metadata")) {
                PatchEntry entry;
                entry.repo = repo.name;
                entry.file = path;
                entry.index = index_in_file++;
                entry.name = node.attribute("Name").value();
                entry.author = node.attribute("Author").value();
                entry.note = node.attribute("Note").value();
                entry.app_ver = node.attribute("AppVer").value();
                entry.app_elf = node.attribute("AppElf").value();
                entry.patch_ver = node.attribute("PatchVer").value();
                entry.enabled = std::string_view{node.attribute("isEnabled").value()} == "true";
                entries.push_back(std::move(entry));
            }
        }
    }
    return entries;
}

bool SaveEnabled(std::span<const PatchEntry> entries, std::string& error) {
    std::map<std::filesystem::path, std::vector<const PatchEntry*>> by_file;
    for (const auto& entry : entries) {
        by_file[entry.file].push_back(&entry);
    }

    for (const auto& [file, file_entries] : by_file) {
        pugi::xml_document doc;
        if (!doc.load_file(file.c_str())) {
            error = "Could not read " + Common::FS::PathToUTF8String(file.filename());
            return false;
        }
        std::vector<pugi::xml_node> nodes;
        for (auto node : doc.child("Patch").children("Metadata")) {
            nodes.push_back(node);
        }
        for (const auto* entry : file_entries) {
            if (entry->index >= nodes.size()) {
                continue;
            }
            auto attr = nodes[entry->index].attribute("isEnabled");
            if (!attr) {
                attr = nodes[entry->index].append_attribute("isEnabled");
            }
            attr.set_value(entry->enabled ? "true" : "false");
        }
        if (!doc.save_file(file.c_str(), "    ")) {
            error = "Could not save " + Common::FS::PathToUTF8String(file.filename());
            return false;
        }
    }
    return true;
}

} // namespace Core::Downloads::CommunityPatches
