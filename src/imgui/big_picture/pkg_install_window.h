// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/pkg_installer.h"

namespace BigPictureMode {

class PkgInstallWindow {
public:
    ~PkgInstallWindow();

    // Opens the window, optionally with a PKG already chosen (e.g. one dropped on the window).
    void Open(const std::filesystem::path& pkg = {});

    // Draws the window while it is open. onInstalled runs after a successful install so the
    // caller can rescan its game list.
    void Draw(bool* open, float uiScale, const std::function<void()>& onInstalled);

    bool IsBusy() const;

private:
    enum class State {
        ChoosePkg,
        Ready,
        Installing,
        Finished,
    };

    void SelectPkg(const std::filesystem::path& pkg);
    void RefreshInstallDirs();
    void Replan();
    void StartInstall();
    void DrawDetails(float uiScale);

    State state = State::ChoosePkg;
    std::filesystem::path pkg_path;
    std::vector<std::filesystem::path> install_dirs;
    int install_dir_index = 0;
    std::optional<Core::PkgInstaller::InstallPlan> plan;
    bool delete_after_install = false;

    Core::PkgInstaller::Progress progress;
    mutable std::mutex result_mutex;
    std::optional<Core::PkgInstaller::InstallResult> result;
    bool notified_install = false;
    std::jthread worker;
};

} // namespace BigPictureMode
