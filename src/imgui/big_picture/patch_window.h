// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/downloads/community_patches.h"
#include "core/downloads/sony_updates.h"

namespace BigPictureMode {

struct PatchGame {
    std::string title;
    std::string serial;
    std::string app_version;
};

class PatchWindow {
public:
    ~PatchWindow();

    void Open(std::vector<PatchGame> games);
    void Draw(bool* open, float uiScale);

    bool IsBusy() const;

private:
    void DrawCommunityTab(float uiScale);
    void DrawSonyTab(float uiScale);
    void SelectGame(int index);
    void LoadPatchList();
    void StartSync();
    void StartLookup();
    void StartDownload();

    std::vector<PatchGame> games;
    int game_index = -1;
    std::array<char, 16> manual_serial{};

    // Community patches
    std::vector<Core::Downloads::CommunityPatches::PatchEntry> patches;
    bool patches_dirty = false;
    std::string patch_status;
    bool patch_status_error = false;
    std::atomic<bool> syncing{false};
    std::atomic<u32> sync_repo{0};
    Core::Downloads::CommunityPatches::SyncProgress sync_progress;
    std::atomic<bool> sync_cancel{false};
    bool sync_finished = false;
    std::vector<std::string> sync_messages;

    // Sony updates
    std::array<char, 16> sony_title_id{};
    std::atomic<bool> looking_up{false};
    std::optional<Core::Downloads::SonyUpdates::UpdateInfo> update_info;
    std::filesystem::path download_dir;
    std::atomic<bool> downloading{false};
    Core::Downloads::SonyUpdates::DownloadProgress download_progress;
    std::atomic<bool> download_cancel{false};
    std::optional<Core::Downloads::SonyUpdates::DownloadResult> download_result;

    mutable std::mutex mutex;
    std::jthread sync_worker;
    std::jthread sony_worker;
};

} // namespace BigPictureMode
