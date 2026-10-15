// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>
#include <CLI/CLI.hpp>
#include <SDL3/SDL_messagebox.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/arch.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/downloads/community_patches.h"
#include "core/downloads/sony_updates.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "core/pkg_installer.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace {

std::filesystem::path Utf8Path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

int InstallPkgFromCli(const std::filesystem::path& pkg,
                      const std::optional<std::filesystem::path>& installDir, bool overwrite,
                      bool deletePkg) {
    std::filesystem::path dir;
    if (installDir) {
        dir = *installDir;
    } else if (const auto dirs = EmulatorSettings.GetGameInstallDirs(); !dirs.empty()) {
        dir = dirs.front();
    }

    const auto plan = Core::PkgInstaller::PlanInstall(pkg, dir);
    if (!plan.CanInstall()) {
        std::cerr << "Cannot install " << Common::FS::PathToUTF8String(pkg) << ": " << plan.error
                  << std::endl;
        return 1;
    }
    std::cout << fmt::format("{} [{}] {} {} -> {}", plan.title, plan.title_id,
                             Core::PkgInstaller::KindName(plan.kind), plan.pkg_version,
                             Common::FS::PathToUTF8String(plan.target_path))
              << std::endl;
    if (!plan.overwrite_warning.empty()) {
        if (!overwrite) {
            std::cerr << plan.overwrite_warning << " Pass --overwrite to continue." << std::endl;
            return 1;
        }
        std::cout << plan.overwrite_warning << std::endl;
    }

    Core::PkgInstaller::Progress progress;
    std::atomic<bool> finished{false};
    std::jthread reporter([&] {
        u32 last = std::numeric_limits<u32>::max();
        while (!finished) {
            const u32 done = progress.files_done;
            if (done != last && progress.files_total != 0) {
                std::cout << "\rExtracting " << done << " / " << progress.files_total << std::flush;
                last = done;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });
    const auto result = Core::PkgInstaller::RunInstall(plan, progress, deletePkg);
    finished = true;
    reporter.join();
    std::cout << "\r" << std::string(40, ' ') << "\r";
    (result.success ? std::cout : std::cerr) << result.message << std::endl;
    return result.success ? 0 : 1;
}

int SyncPatchesFromCli() {
    int failures = 0;
    for (const auto& repo : Core::Downloads::CommunityPatches::Repos()) {
        Core::Downloads::CommunityPatches::SyncProgress progress;
        const auto result = Core::Downloads::CommunityPatches::SyncRepo(repo, progress, nullptr);
        (result.success ? std::cout : std::cerr) << result.message << std::endl;
        failures += result.success ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}

int SonyUpdateFromCli(const std::string& titleId, bool download,
                      const std::optional<std::filesystem::path>& outputDir) {
    namespace Sony = Core::Downloads::SonyUpdates;
    const auto info = Sony::Lookup(titleId);
    if (!info.found) {
        std::cerr << info.error << std::endl;
        return 1;
    }
    std::cout << fmt::format("{} [{}]\n  Latest version: {}\n  Size: {}\n  Type: {}\n"
                             "  Content ID: {}\n  Manifest: {}",
                             info.title, info.title_id, info.version, Sony::FormatSize(info.size),
                             info.type, info.content_id, info.manifest_url)
              << std::endl;
    if (!download) {
        return 0;
    }

    const auto dir = outputDir.value_or(Common::FS::GetUserPath(Common::FS::PathType::DownloadDir));
    Sony::DownloadProgress progress;
    std::atomic<bool> finished{false};
    std::jthread reporter([&] {
        while (!finished) {
            if (progress.bytes_total != 0) {
                std::cout << fmt::format("\r{}{} / {} (part {}/{})   ",
                                         progress.verifying ? "Checking " : "",
                                         Sony::FormatSize(progress.bytes_done),
                                         Sony::FormatSize(progress.bytes_total),
                                         progress.piece.load(), progress.pieces.load())
                          << std::flush;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });
    const auto result = Sony::Download(info, dir, progress, nullptr);
    finished = true;
    reporter.join();
    std::cout << std::endl;
    (result.success ? std::cout : std::cerr) << result.message << std::endl;
    return result.success ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
    // KosmicKrisp only supports Apple Silicon. Check that we are not running on an Intel Mac.
    int sysctl_ret = 0;
    size_t sysctl_size = sizeof(sysctl_ret);
    sysctlbyname("sysctl.proc_translated", &sysctl_ret, &sysctl_size, nullptr, 0);
    if (sysctl_ret != 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "shadPS4 only supports Apple Silicon Macs.", nullptr);
        std::cout << "shadPS4 only supports Apple Silicon Macs." << std::endl;
        return -1;
    }
#endif

    CLI::App app{"shadPS4 Emulator CLI"};

    // ---- CLI state ----
    std::optional<std::string> gamePath;
    std::vector<std::string> gameArgs;
    std::optional<std::filesystem::path> overrideRoot;
    std::optional<int> waitPid;
    bool waitForDebugger = false;
    bool userfaultfd = false;

    std::optional<std::string> fullscreenStr;
    bool ignoreGamePatch = false;
    bool showFps = false;
    bool configClean = false;
    bool configGlobal = false;
    bool bigPicture = false;
    bool sameProcess = false;
    bool append_log{};

    std::optional<std::string> installPkg;
    std::optional<std::filesystem::path> installDir;
    bool installOverwrite = false;
    bool installDeletePkg = false;
    bool syncPatches = false;
    std::optional<std::string> checkUpdate;
    std::optional<std::string> downloadUpdate;
    std::optional<std::filesystem::path> outputDir;

    std::optional<std::filesystem::path> addGameFolder;
    std::optional<std::filesystem::path> setAddonFolder;
    std::optional<std::string> patchFile;

    std::vector<std::pair<std::filesystem::path, std::string>> mounts;
    static std::vector<std::string> env_vars;

    // ---- Options ----
    app.add_option("guest_arg", gamePath, "Game path or ID"); // positional
    app.add_option("-g,--game", gamePath, "Game path or ID");
    app.add_option("-p,--patch", patchFile, "Patch file to apply");
    app.add_flag("-i,--ignore-game-patch", ignoreGamePatch,
                 "Disable automatic loading of game patches");

    app.add_flag("-b,--big-picture", bigPicture, "Start in Big Picture Mode");
    app.add_flag("--same-process", sameProcess,
                 "Launch the game in the same process when using Big Picture Mode");

    app.add_option("-f,--fullscreen", fullscreenStr, "Fullscreen mode (true|false)");

    app.add_option("--override-root", overrideRoot)->check(CLI::ExistingDirectory);

    app.add_flag("--wait-for-debugger", waitForDebugger);
    app.add_option("--wait-for-pid", waitPid);
#ifdef __linux__
    app.add_flag("--userfaultfd", userfaultfd,
                 "Enable userfaultfd for tracking memory (Linux only)");
#endif

    app.add_flag("--show-fps", showFps);
    app.add_flag("--config-clean", configClean);
    app.add_flag("--config-global", configGlobal);
    app.add_flag("--log-append", append_log);

    app.add_option("--install-pkg", installPkg, "Install a game, update or DLC from a .pkg file")
        ->check(CLI::ExistingFile);
    app.add_option("--install-dir", installDir,
                   "Game folder to install into (default: first configured game folder)");
    app.add_flag("--overwrite", installOverwrite, "Allow --install-pkg to overwrite an install");
    app.add_flag("--delete-pkg", installDeletePkg, "Delete the .pkg after a successful install");
    app.add_flag("--sync-patches", syncPatches,
                 "Download or update the community patch repositories");
    app.add_option("--check-update", checkUpdate,
                   "Look up the latest official update for a title ID on Sony's server");
    app.add_option("--download-update", downloadUpdate,
                   "Download the latest official update PKG for a title ID");
    app.add_option("--output-dir", outputDir, "Folder for --download-update");

    app.add_option("--add-game-folder", addGameFolder)->check(CLI::ExistingDirectory);
    app.add_option("--set-addon-folder", setAddonFolder)->check(CLI::ExistingDirectory);
    app.add_option("--mount", mounts, "Mount source to destination");
    app.add_option("-e,--env", env_vars, "Environment variables to pass to the guest");

    // ---- Capture args after `--` verbatim ----
    app.allow_extras();

    try {
        bool double_dash_found = false;
        int double_dash_index;
        for (int i = 0; i < argc; i++) {
            if (double_dash_found) {
                gameArgs.emplace_back(argv[i]);
            }
            if (!double_dash_found && std::string(argv[i]) == "--") {
                double_dash_found = true;
                double_dash_index = i;
            }
        }

        // If the -- arg is present, only parse args before it
        if (double_dash_found) {
            app.parse(double_dash_index, argv);
        } else {
            app.parse(argc, argv);
        }
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // With no arguments, open the launcher (game list, PKG installer, patches).
    if (argc == 1) {
        bigPicture = true;
    }

    if (waitPid)
        Core::Debugger::WaitForPid(*waitPid);

    // Initialize main log with default config
    Common::Log::Setup("shadps4.log");

    LOG_INFO(Debug, "Run: {}", fmt::join(std::span(argv, argc), " "));

    IPC::Instance().Init();

    // Initialize key manager
    KeyManager::GetInstance()->LoadFromFile();

    // Load configurations
    EmulatorSettings.Load();
    UserSettings.Load();

    if (installPkg) {
        return InstallPkgFromCli(Utf8Path(*installPkg), installDir, installOverwrite,
                                 installDeletePkg);
    }

    if (syncPatches) {
        return SyncPatchesFromCli();
    }

    if (checkUpdate || downloadUpdate) {
        return SonyUpdateFromCli(downloadUpdate.value_or(checkUpdate.value_or("")),
                                 downloadUpdate.has_value(), outputDir);
    }

    if (bigPicture) {
        BigPictureMode::Launch(argv[0], sameProcess);
        return 0;
    }

    // ---- Utility commands ----
    if (addGameFolder) {
        EmulatorSettings.AddGameInstallDir(*addGameFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Game folder successfully saved.");
        return 0;
    }

    if (setAddonFolder) {
        EmulatorSettings.SetAddonInstallDir(*setAddonFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Addon folder successfully saved.");
        return 0;
    }

    if (!gamePath.has_value()) {
        if (!gameArgs.empty()) {
            gamePath = gameArgs.front();
            gameArgs.erase(gameArgs.begin());
        } else {
            LOG_ERROR(Debug, "Please provide a game path or ID.");
            return 1;
        }
    }

    // ---- Apply flags ----
    if (patchFile)
        MemoryPatcher::patch_file = *patchFile;

    if (ignoreGamePatch)
        Core::FileSys::MntPoints::ignore_game_patches = true;

    if (fullscreenStr) {
        if (*fullscreenStr == "true") {
            EmulatorSettings.SetFullScreen(true);
        } else if (*fullscreenStr == "false") {
            EmulatorSettings.SetFullScreen(false);
        } else {
            LOG_ERROR(Debug, "Invalid argument for --fullscreen (use true|false)");
            return 1;
        }
    }

    if (showFps)
        EmulatorSettings.SetShowFpsCounter(true);

    if (configClean)
        EmulatorSettings.SetConfigMode(ConfigMode::Clean);

    if (configGlobal)
        EmulatorSettings.SetConfigMode(ConfigMode::Global);

    if (userfaultfd) {
        EmulatorSettings.SetUserfaultfdTracking(true);
    }

    // ---- Resolve game path or ID ----
    std::filesystem::path ebootPath(*gamePath);
    const auto archive_component_exists = [](const std::filesystem::path& p) -> bool {
        std::filesystem::path accum;
        for (const auto& comp : p) {
            accum /= comp;
            if (comp.extension() == ".zar") {
                return std::filesystem::is_regular_file(accum);
            }
        }
        return false;
    };
    if (!std::filesystem::exists(ebootPath) && !archive_component_exists(ebootPath)) {
        bool found = false;
        for (const auto& installDir : EmulatorSettings.GetGameInstallDirs()) {
            if (auto foundPath = Common::FS::FindGameByID(installDir, *gamePath, 0)) {
                ebootPath = *foundPath;
                found = true;
                break;
            }
        }
        if (!found) {
            for (const auto& installDir : EmulatorSettings.GetGameInstallDirs()) {
                if (auto foundPath = Common::FS::FindGameByID(installDir, *gamePath, 5)) {
                    ebootPath = *foundPath;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            LOG_ERROR(Debug, "Game ID or file path not found: {}", *gamePath);
            return 1;
        }
    }

    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
    emulator->Run(ebootPath, gameArgs, overrideRoot, mounts, env_vars, append_log);

    return 0;
}
