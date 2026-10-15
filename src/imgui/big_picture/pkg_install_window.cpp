// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include <ImGuiFileDialog.h>
#include <fmt/format.h>

#include "common/path_util.h"
#include "core/devtools/layer.h"
#include "core/downloads/sony_updates.h"
#include "core/emulator_settings.h"
#include "imgui/big_picture/pkg_install_window.h"
#include "imgui/imgui_std.h"

namespace BigPictureMode {

namespace {

constexpr auto PkgDialogKey = "ChoosePkgFile";
constexpr auto InstallDirDialogKey = "ChooseInstallFolder";

const ImVec4 ErrorColor{1.0f, 0.42f, 0.42f, 1.0f};
const ImVec4 WarningColor{1.0f, 0.82f, 0.35f, 1.0f};
const ImVec4 SuccessColor{0.45f, 0.9f, 0.5f, 1.0f};

std::string StartFolder(const std::filesystem::path& current) {
    std::error_code ec;
    if (!current.empty() && std::filesystem::is_directory(current.parent_path(), ec)) {
        return Common::FS::PathToUTF8String(current.parent_path());
    }
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home ? std::string{home} : std::string{"."};
}

void DetailRow(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", value.c_str());
}

} // namespace

PkgInstallWindow::~PkgInstallWindow() {
    progress.cancel = true;
}

void PkgInstallWindow::Open(const std::filesystem::path& pkg) {
    if (IsBusy()) {
        return;
    }
    RefreshInstallDirs();
    {
        std::scoped_lock lock{result_mutex};
        result.reset();
    }
    notified_install = false;
    if (!pkg.empty()) {
        SelectPkg(pkg);
    } else if (state == State::Finished) {
        state = State::ChoosePkg;
        plan.reset();
        pkg_path.clear();
    }
}

bool PkgInstallWindow::IsBusy() const {
    return state == State::Installing;
}

void PkgInstallWindow::RefreshInstallDirs() {
    install_dirs = EmulatorSettings.GetGameInstallDirs();
    if (install_dir_index >= static_cast<int>(install_dirs.size())) {
        install_dir_index = 0;
    }
}

void PkgInstallWindow::SelectPkg(const std::filesystem::path& pkg) {
    pkg_path = pkg;
    {
        std::scoped_lock lock{result_mutex};
        result.reset();
    }
    notified_install = false;
    Replan();
}

void PkgInstallWindow::Replan() {
    if (pkg_path.empty()) {
        plan.reset();
        state = State::ChoosePkg;
        return;
    }
    const auto dir =
        install_dirs.empty() ? std::filesystem::path{} : install_dirs[install_dir_index];
    plan = Core::PkgInstaller::PlanInstall(pkg_path, dir);
    state = State::Ready;
}

void PkgInstallWindow::StartInstall() {
    if (!plan || !plan->CanInstall()) {
        return;
    }
    progress.cancel = false;
    progress.files_done = 0;
    progress.files_total = 0;
    {
        std::scoped_lock lock{result_mutex};
        result.reset();
    }
    notified_install = false;
    state = State::Installing;

    worker = std::jthread([this, install_plan = *plan, remove_pkg = delete_after_install] {
        auto install_result = Core::PkgInstaller::RunInstall(install_plan, progress, remove_pkg);
        std::scoped_lock lock{result_mutex};
        result = std::move(install_result);
    });
}

void PkgInstallWindow::DrawDetails(float uiScale) {
    if (!plan) {
        ImGui::TextDisabled("Choose a .pkg file to see what it contains.");
        return;
    }

    if (ImGui::BeginTable("PkgDetails", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 260.0f * uiScale);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        if (plan->CanInstall() || !plan->title_id.empty()) {
            DetailRow("Title", plan->title);
            DetailRow("Title ID", plan->title_id);
            DetailRow("Type", std::string{Core::PkgInstaller::KindName(plan->kind)});
            DetailRow("Version", plan->pkg_version.empty() ? "-" : plan->pkg_version);
            if (!plan->installed_version.empty()) {
                DetailRow("Installed version", plan->installed_version);
            }
            DetailRow("Size", Core::Downloads::SonyUpdates::FormatSize(plan->pkg_size));
            if (!plan->flags.empty()) {
                DetailRow("Flags", plan->flags);
            }
            if (!plan->target_path.empty()) {
                DetailRow("Installs to", Common::FS::PathToUTF8String(plan->target_path));
            }
        }
        ImGui::EndTable();
    }
}

void PkgInstallWindow::Draw(bool* open, float uiScale, const std::function<void()>& onInstalled) {
    if (state == State::Installing) {
        std::scoped_lock lock{result_mutex};
        if (result) {
            state = State::Finished;
            worker = {};
        }
    }
    if (state == State::Finished && !notified_install) {
        notified_install = true;
        std::scoped_lock lock{result_mutex};
        if (result && result->success && onInstalled) {
            onInstalled();
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Install PKG", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::DrawPrettyBackground();
    ImGui::SetWindowFontScale(uiScale);

    Overlay::TextCentered("Install PKG");
    ImGui::Dummy(ImVec2(0.0f, 10.f * uiScale));

    const bool busy = state == State::Installing;
    const ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;
    const float footer_height = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("InstallContent", ImVec2(0, -footer_height), child_flags);

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Choose PKG...")) {
        ImGuiFileDialog::Instance()->OpenDialog(
            PkgDialogKey, "Choose a PKG to install", ".pkg,.PKG", StartFolder(pkg_path), "", 1,
            nullptr,
            ImGuiFileDialogFlags_DontShowHiddenFiles |
                ImGuiFileDialogFlags_DisableCreateDirectoryButton);
    }
    ImGui::SameLine();
    if (pkg_path.empty()) {
        ImGui::TextDisabled("No file chosen (you can also drop a .pkg on this window)");
    } else {
        ImGui::TextWrapped("%s", Common::FS::PathToUTF8String(pkg_path).c_str());
    }

    ImGui::Separator();
    ImGui::Text("Game install folder");
    if (install_dirs.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
        ImGui::TextWrapped("No game folder is set up yet. Add one to install games into.");
        ImGui::PopStyleColor();
    } else {
        const auto preview = Common::FS::PathToUTF8String(install_dirs[install_dir_index]);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
        if (ImGui::BeginCombo("##InstallDir", preview.c_str())) {
            for (int i = 0; i < static_cast<int>(install_dirs.size()); i++) {
                const auto label = Common::FS::PathToUTF8String(install_dirs[i]);
                if (ImGui::Selectable(label.c_str(), i == install_dir_index)) {
                    install_dir_index = i;
                    Replan();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Add folder...")) {
        ImGuiFileDialog::Instance()->OpenDialog(InstallDirDialogKey, "Choose a game folder",
                                                nullptr, StartFolder({}), "", 1, nullptr,
                                                ImGuiFileDialogFlags_DontShowHiddenFiles);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    DrawDetails(uiScale);

    if (state == State::Ready || state == State::Finished) {
        ImGui::Checkbox("Delete the PKG file after a successful install", &delete_after_install);
    }

    ImGui::EndChild();

    if (state == State::Installing) {
        const u32 total = progress.files_total;
        const u32 done = progress.files_done;
        const float fraction = total == 0 ? 0.0f : static_cast<float>(done) / total;
        const auto label = total == 0 ? std::string{"Reading PKG..."}
                                      : fmt::format("Installing: {} / {} files", done, total);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), label.c_str());
    } else if (state == State::Finished) {
        std::scoped_lock lock{result_mutex};
        if (result) {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, result->success ? SuccessColor : ErrorColor);
            ImGui::TextUnformatted(result->message.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", result->message.c_str());
            }
        }
    } else if (state == State::Ready && plan &&
               (!plan->error.empty() || !plan->overwrite_warning.empty())) {
        const auto& message = plan->error.empty() ? plan->overwrite_warning : plan->error;
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, plan->error.empty() ? WarningColor : ErrorColor);
        ImGui::TextUnformatted(message.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", message.c_str());
        }
    } else {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
    }

    const bool can_install = state == State::Ready && plan && plan->CanInstall();
    ImGui::BeginDisabled(!can_install);
    if (ImGui::Button("Install")) {
        if (plan->overwrite_warning.empty()) {
            StartInstall();
        } else {
            ImGui::OpenPopup("Confirm Overwrite");
        }
    }
    ImGui::EndDisabled();

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Overwrite", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(viewport->WorkSize.x * 0.6f);
        ImGui::TextWrapped("%s", plan ? plan->overwrite_warning.c_str() : "");
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ImGui::Button("Overwrite", ImVec2(200 * uiScale, 0))) {
            ImGui::CloseCurrentPopup();
            StartInstall();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(200 * uiScale, 0))) {
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (busy) {
        if (ImGui::Button("Cancel install")) {
            progress.cancel = true;
        }
    } else if (ImGui::Button("Close")) {
        *open = false;
    }

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    if (ImGuiFileDialog::Instance()->Display(PkgDialogKey, ImGuiWindowFlags_NoMove)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            SelectPkg(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(
                ImGuiFileDialog::Instance()->GetFilePathName().c_str()))));
        }
        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    if (ImGuiFileDialog::Instance()->Display(InstallDirDialogKey, ImGuiWindowFlags_NoMove)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            const std::filesystem::path dir(std::u8string(reinterpret_cast<const char8_t*>(
                ImGuiFileDialog::Instance()->GetCurrentPath().c_str())));
            EmulatorSettings.AddGameInstallDir(dir);
            EmulatorSettings.Save();
            RefreshInstallDirs();
            for (int i = 0; i < static_cast<int>(install_dirs.size()); i++) {
                if (install_dirs[i] == dir) {
                    install_dir_index = i;
                }
            }
            if (!pkg_path.empty()) {
                Replan();
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::End();
}

} // namespace BigPictureMode
