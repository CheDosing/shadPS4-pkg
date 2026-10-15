// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Downloads::CommunityPatches {

struct Repo {
    std::string name;
    std::string owner;
    std::string repo;
    std::string branch;
    std::string dir;
};

std::span<const Repo> Repos();

struct SyncProgress {
    std::atomic<u32> files_done{0};
    std::atomic<u32> files_total{0};
};

struct SyncResult {
    bool success = false;
    u32 files = 0;
    u32 failed = 0;
    std::string message;
};

// Downloads every patch XML from a repo into user/patches/<repo name>/ and rebuilds its
// files.json index. Patches that were enabled before stay enabled.
SyncResult SyncRepo(const Repo& repo, SyncProgress& progress, const std::atomic<bool>* cancel);

struct PatchEntry {
    std::string repo;
    std::filesystem::path file;
    u32 index = 0;
    std::string name;
    std::string author;
    std::string note;
    std::string app_ver;
    std::string app_elf;
    std::string patch_ver;
    bool enabled = false;
};

std::filesystem::path RepoFolder(const Repo& repo);
bool HasDownloadedPatches();

std::vector<PatchEntry> ListPatchesFor(const std::string& serial);

bool SaveEnabled(std::span<const PatchEntry> entries, std::string& error);

} // namespace Core::Downloads::CommunityPatches
