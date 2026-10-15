// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "common/types.h"

namespace Core::PkgInstaller {

enum class PkgKind {
    Game,
    Patch,
    Dlc,
};

struct InstallPlan {
    std::filesystem::path pkg_path;
    std::filesystem::path target_path;
    std::string title;
    std::string title_id;
    std::string content_id;
    std::string category;
    std::string flags;
    std::string pkg_version;
    std::string installed_version;
    PkgKind kind = PkgKind::Game;
    u64 pkg_size = 0;

    // Set when the PKG can't be installed at all; the plan is unusable.
    std::string error;
    // Set when installing would overwrite something; ask before running.
    std::string overwrite_warning;

    bool CanInstall() const {
        return error.empty();
    }
};

struct Progress {
    std::atomic<u32> files_done{0};
    std::atomic<u32> files_total{0};
    std::atomic<bool> cancel{false};
};

struct InstallResult {
    bool success = false;
    bool cancelled = false;
    std::string message;
    u32 failed_files = 0;
};

std::string_view KindName(PkgKind kind);

InstallPlan PlanInstall(const std::filesystem::path& pkg_path,
                        const std::filesystem::path& install_dir);

InstallResult RunInstall(const InstallPlan& plan, Progress& progress,
                         bool delete_pkg_after = false);

} // namespace Core::PkgInstaller
