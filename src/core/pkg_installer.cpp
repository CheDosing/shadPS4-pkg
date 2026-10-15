// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "core/emulator_settings.h"
#include "core/file_format/pkg.h"
#include "core/file_format/psf.h"
#include "core/file_sys/ifile.h"
#include "core/pkg_installer.h"

namespace Core::PkgInstaller {

namespace {

double ParseVersion(const std::string& version) {
    try {
        return std::stod(version);
    } catch (...) {
        return 0.0;
    }
}

std::string ReadAppVersion(const std::filesystem::path& sfo_path) {
    std::error_code ec;
    if (!std::filesystem::exists(sfo_path, ec)) {
        return {};
    }
    PSF psf;
    if (!psf.Open(sfo_path)) {
        return {};
    }
    if (const auto ver = psf.GetString("APP_VER"); ver.has_value()) {
        return std::string{*ver};
    }
    return {};
}

std::filesystem::path GameRootFromBootPath(const std::filesystem::path& boot_path) {
    if (Core::FileSys::IsZArchiveFile(boot_path)) {
        return boot_path;
    }
    return boot_path.parent_path();
}

} // namespace

std::string_view KindName(PkgKind kind) {
    switch (kind) {
    case PkgKind::Game:
        return "Game";
    case PkgKind::Patch:
        return "Update";
    case PkgKind::Dlc:
        return "DLC";
    }
    return "Unknown";
}

InstallPlan PlanInstall(const std::filesystem::path& pkg_path,
                        const std::filesystem::path& install_dir) {
    InstallPlan plan;
    plan.pkg_path = pkg_path;

    PKG pkg;
    std::string failreason;
    if (!pkg.Open(pkg_path, failreason)) {
        plan.error = failreason.empty() ? "Not a valid PKG file" : failreason;
        return plan;
    }

    PSF psf;
    if (!psf.Open(pkg.sfo)) {
        plan.error = "Could not read the PKG's param.sfo";
        return plan;
    }

    plan.title_id = std::string{pkg.GetTitleID()};
    plan.flags = pkg.GetPkgFlags();
    plan.pkg_size = pkg.GetPkgSize();
    plan.title = std::string{psf.GetString("TITLE").value_or(plan.title_id)};
    plan.category = std::string{psf.GetString("CATEGORY").value_or("")};
    plan.content_id = std::string{psf.GetString("CONTENT_ID").value_or("")};
    plan.pkg_version = std::string{psf.GetString("APP_VER").value_or("")};

    if (plan.flags.find("PATCH") != std::string::npos || plan.category.starts_with("gp")) {
        plan.kind = PkgKind::Patch;
    } else if (plan.category == "ac") {
        plan.kind = PkgKind::Dlc;
    } else {
        plan.kind = PkgKind::Game;
    }

    std::error_code ec;
    constexpr int max_depth = 5;

    switch (plan.kind) {
    case PkgKind::Game: {
        if (install_dir.empty()) {
            plan.error = "Choose an install folder first";
            return plan;
        }
        plan.target_path = install_dir / plan.title_id;
        if (std::filesystem::exists(plan.target_path, ec)) {
            plan.installed_version = ReadAppVersion(plan.target_path / "sce_sys" / "param.sfo");
            plan.overwrite_warning =
                fmt::format("{} is already installed at {}. Installing will overwrite it.",
                            plan.title_id, Common::FS::PathToUTF8String(plan.target_path));
        }
        break;
    }
    case PkgKind::Patch: {
        std::optional<std::filesystem::path> boot_path;
        for (const auto& dir : EmulatorSettings.GetGameInstallDirs()) {
            if ((boot_path = Common::FS::FindGameByID(dir, plan.title_id, max_depth))) {
                break;
            }
        }
        if (!boot_path && !install_dir.empty()) {
            boot_path = Common::FS::FindGameByID(install_dir, plan.title_id, max_depth);
        }
        if (!boot_path) {
            plan.error = fmt::format("This is an update for {}. Install the base game first.",
                                     plan.title_id);
            return plan;
        }

        const auto game_root = GameRootFromBootPath(*boot_path);
        auto update_name = Core::FileSys::IsZArchiveFile(game_root)
                               ? Core::FileSys::StripZArchiveExtension(game_root).filename()
                               : game_root.filename();
        update_name += "-UPDATE";
        plan.target_path = game_root.parent_path() / update_name;

        plan.installed_version = ReadAppVersion(plan.target_path / "sce_sys" / "param.sfo");
        if (plan.installed_version.empty() && !Core::FileSys::IsZArchiveFile(game_root)) {
            plan.installed_version = ReadAppVersion(game_root / "sce_sys" / "param.sfo");
        }

        if (!plan.installed_version.empty()) {
            const double installed = ParseVersion(plan.installed_version);
            const double incoming = ParseVersion(plan.pkg_version);
            if (incoming == installed) {
                plan.overwrite_warning =
                    fmt::format("Version {} is already installed. Installing will overwrite it.",
                                plan.pkg_version);
            } else if (incoming < installed) {
                plan.overwrite_warning =
                    fmt::format("This update ({}) is older than the installed version ({}).",
                                plan.pkg_version, plan.installed_version);
            }
        }
        break;
    }
    case PkgKind::Dlc: {
        const auto parts = Common::SplitString(plan.content_id, '-');
        if (parts.size() < 3 || parts[2].empty()) {
            plan.error = "The DLC's content ID is malformed";
            return plan;
        }
        plan.target_path = EmulatorSettings.GetAddonInstallDir() / plan.title_id / parts[2];
        if (std::filesystem::exists(plan.target_path, ec)) {
            plan.overwrite_warning =
                fmt::format("DLC {} is already installed. Installing will overwrite it.", parts[2]);
        }
        break;
    }
    }

    return plan;
}

InstallResult RunInstall(const InstallPlan& plan, Progress& progress, bool delete_pkg_after) {
    InstallResult result;
    if (!plan.CanInstall()) {
        result.message = plan.error;
        return result;
    }

    std::error_code ec;
    const bool target_existed = std::filesystem::exists(plan.target_path, ec);

    auto space_probe = plan.target_path;
    while (!space_probe.empty() && !std::filesystem::exists(space_probe, ec)) {
        space_probe = space_probe.parent_path();
    }
    if (const auto space = std::filesystem::space(space_probe, ec);
        !ec && space.available < plan.pkg_size) {
        result.message = fmt::format(
            "Not enough disk space in {} ({} MB free, about {} MB needed).",
            Common::FS::PathToUTF8String(space_probe), space.available >> 20, plan.pkg_size >> 20);
        return result;
    }
    const auto cleanup_new_target = [&] {
        if (!target_existed) {
            std::filesystem::remove_all(plan.target_path, ec);
        }
    };

    PKG pkg;
    std::string failreason;
    if (!pkg.Open(plan.pkg_path, failreason) ||
        !pkg.Extract(plan.pkg_path, plan.target_path, failreason)) {
        cleanup_new_target();
        result.message = failreason.empty() ? "Extraction failed" : failreason;
        LOG_ERROR(Loader, "PKG install failed: {}", result.message);
        return result;
    }

    const u32 total = pkg.GetNumberOfFiles();
    progress.files_total = total;
    progress.files_done = 0;

    for (u32 i = 0; i < total; i++) {
        if (progress.cancel) {
            result.cancelled = true;
            result.message = "Installation cancelled. The install folder may be incomplete.";
            return result;
        }
        if (!pkg.ExtractFiles(static_cast<int>(i))) {
            result.failed_files++;
        }
        progress.files_done = i + 1;
    }

    if (result.failed_files != 0) {
        result.message =
            fmt::format("{} of {} files could not be extracted.", result.failed_files, total);
        LOG_ERROR(Loader, "PKG install of {} finished with errors: {}", plan.title_id,
                  result.message);
        return result;
    }

    result.success = true;
    result.message = fmt::format("{} {} installed to {}", plan.title_id, KindName(plan.kind),
                                 Common::FS::PathToUTF8String(plan.target_path));
    LOG_INFO(Loader, "{}", result.message);

    if (delete_pkg_after) {
        std::filesystem::remove(plan.pkg_path, ec);
    }
    return result;
}

} // namespace Core::PkgInstaller
