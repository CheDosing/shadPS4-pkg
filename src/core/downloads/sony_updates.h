// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "common/types.h"

namespace Core::Downloads::SonyUpdates {

bool IsValidTitleId(const std::string& title_id);

std::string UpdateXmlUrl(const std::string& title_id);

struct UpdateInfo {
    bool found = false;
    std::string error;

    std::string title_id;
    std::string title;
    std::string version;
    std::string content_id;
    std::string manifest_url;
    std::string digest;
    std::string type;
    u64 size = 0;
    u32 system_ver = 0;
};

// Asks Sony's update server for the latest official update of a title.
UpdateInfo Lookup(const std::string& title_id);

struct DownloadProgress {
    std::atomic<u64> bytes_done{0};
    std::atomic<u64> bytes_total{0};
    std::atomic<u32> piece{0};
    std::atomic<u32> pieces{0};
    std::atomic<bool> verifying{false};
};

struct DownloadResult {
    bool success = false;
    std::string message;
    std::filesystem::path file;
};

// Downloads every piece listed in the update's manifest into one PKG in out_dir and checks
// each piece's SHA-1. Pieces already on disk with a matching hash are kept, so an
// interrupted download can be resumed.
DownloadResult Download(const UpdateInfo& info, const std::filesystem::path& out_dir,
                        DownloadProgress& progress, const std::atomic<bool>* cancel);

std::string FormatSize(u64 bytes);

} // namespace Core::Downloads::SonyUpdates
