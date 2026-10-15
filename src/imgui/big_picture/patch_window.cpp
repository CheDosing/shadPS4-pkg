// SPDX-FileCopyrightText: Copyright 2026 shadPS4-pkg
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include <ImGuiFileDialog.h>
#include <fmt/format.h>

#include "common/path_util.h"
#include "core/devtools/layer.h"
#include "imgui/big_picture/patch_window.h"
#include "imgui/imgui_std.h"

namespace BigPictureMode {

namespace {

namespace Patches = Core::Downloads::CommunityPatches;
namespace Sony = Core::Downloads::SonyUpdates;

constexpr auto DownloadDirDialogKey = "ChooseDownloadFolder";

const ImVec4 ErrorColor{1.0f, 0.42f, 0.42f, 1.0f};
const ImVec4 WarningColor{1.0f, 0.82f, 0.35f, 1.0f};
const ImVec4 SuccessColor{0.45f, 0.9f, 0.5f, 1.0f};

template <size_t N>
void CopyToBuffer(std::array<char, N>& buffer, const std::string& text) {
    buffer.fill('\0');
    std::memcpy(buffer.data(), text.data(), std::min(text.size(), N - 1));
}

std::string FirmwareString(u32 system_ver) {
    if (system_ver == 0) {
        return "-";
    }
    return fmt::format("{:x}.{:02x}", (system_ver >> 24) & 0xFF, (system_ver >> 16) & 0xFF);
}

void DetailRow(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", value.c_str());
}

} // namespace

PatchWindow::~PatchWindow() {
    sync_cancel = true;
    download_cancel = true;
}

bool PatchWindow::IsBusy() const {
    return syncing || looking_up || downloading;
}

void PatchWindow::Open(std::vector<PatchGame> game_list) {
    games = std::move(game_list);
    if (download_dir.empty()) {
        download_dir = Common::FS::GetUserPath(Common::FS::PathType::DownloadDir);
    }
    if (game_index >= static_cast<int>(games.size())) {
        game_index = -1;
    }
    if (game_index < 0 && !games.empty()) {
        SelectGame(0);
    } else {
        LoadPatchList();
    }
}

void PatchWindow::SelectGame(int index) {
    game_index = index;
    if (index >= 0 && index < static_cast<int>(games.size())) {
        CopyToBuffer(manual_serial, games[index].serial);
        CopyToBuffer(sony_title_id, games[index].serial);
    }
    LoadPatchList();
}

void PatchWindow::LoadPatchList() {
    const std::string serial{manual_serial.data()};
    patches = serial.empty() ? std::vector<Patches::PatchEntry>{} : Patches::ListPatchesFor(serial);
    patches_dirty = false;
}

void PatchWindow::StartSync() {
    if (syncing) {
        return;
    }
    syncing = true;
    sync_cancel = false;
    sync_finished = false;
    {
        std::scoped_lock lock{mutex};
        sync_messages.clear();
    }
    sync_worker = std::jthread([this] {
        u32 repo_number = 0;
        for (const auto& repo : Patches::Repos()) {
            sync_repo = ++repo_number;
            const auto result = Patches::SyncRepo(repo, sync_progress, &sync_cancel);
            {
                std::scoped_lock lock{mutex};
                sync_messages.push_back((result.success ? "" : "! ") + result.message);
            }
            if (sync_cancel) {
                break;
            }
        }
        syncing = false;
    });
}

void PatchWindow::StartLookup() {
    if (looking_up || downloading) {
        return;
    }
    looking_up = true;
    {
        std::scoped_lock lock{mutex};
        update_info.reset();
        download_result.reset();
    }
    sony_worker = std::jthread([this, title_id = std::string{sony_title_id.data()}] {
        auto info = Sony::Lookup(title_id);
        {
            std::scoped_lock lock{mutex};
            update_info = std::move(info);
        }
        looking_up = false;
    });
}

void PatchWindow::StartDownload() {
    std::optional<Sony::UpdateInfo> info;
    {
        std::scoped_lock lock{mutex};
        info = update_info;
        download_result.reset();
    }
    if (!info || !info->found || downloading || looking_up) {
        return;
    }
    downloading = true;
    download_cancel = false;
    sony_worker = std::jthread([this, info = *info, dir = download_dir] {
        auto result = Sony::Download(info, dir, download_progress, &download_cancel);
        {
            std::scoped_lock lock{mutex};
            download_result = std::move(result);
        }
        downloading = false;
    });
}

void PatchWindow::DrawCommunityTab(float uiScale) {
    ImGui::TextWrapped("Community patches (60 FPS unlocks, resolution changes and fixes) from "
                       "the GoldHEN and shadPS4 patch repositories. Enabled patches are applied "
                       "automatically when the game starts.");

    if (!syncing && !sync_finished && sync_worker.joinable()) {
        sync_worker = {};
        sync_finished = true;
        LoadPatchList();
    }

    if (syncing) {
        const u32 total = sync_progress.files_total;
        const u32 done = sync_progress.files_done;
        const auto repos = Patches::Repos();
        const u32 repo = std::min<u32>(sync_repo, static_cast<u32>(repos.size()));
        const auto label =
            total == 0 ? fmt::format("Contacting GitHub ({}/{})...", repo, repos.size())
                       : fmt::format("{}: {} / {} files", repos[repo - 1].name, done, total);
        ImGui::ProgressBar(total == 0 ? 0.0f : static_cast<float>(done) / total,
                           ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, 0.0f), label.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            sync_cancel = true;
        }
    } else {
        if (ImGui::Button(Patches::HasDownloadedPatches() ? "Update patches"
                                                          : "Download patches")) {
            StartSync();
        }
    }
    {
        std::scoped_lock lock{mutex};
        for (const auto& message : sync_messages) {
            const bool failed = message.starts_with("! ");
            ImGui::PushStyleColor(ImGuiCol_Text, failed ? ErrorColor : SuccessColor);
            ImGui::TextWrapped("%s", failed ? message.c_str() + 2 : message.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();
    const std::string serial{manual_serial.data()};
    if (serial.empty()) {
        ImGui::TextDisabled("Pick a game or type a title ID above.");
        return;
    }
    if (!Patches::HasDownloadedPatches()) {
        ImGui::TextDisabled("No patches downloaded yet. Use \"Download patches\" first.");
        return;
    }
    if (patches.empty()) {
        ImGui::TextDisabled("No community patches exist for %s.", serial.c_str());
        return;
    }

    const std::string installed = (game_index >= 0 && game_index < static_cast<int>(games.size()) &&
                                   games[game_index].serial == serial)
                                      ? games[game_index].app_version
                                      : std::string{};

    const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    const float table_height =
        ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTable("PatchList", 5, table_flags, ImVec2(0, std::max(table_height, 100.0f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 70.0f * uiScale);
        ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("For version", ImGuiTableColumnFlags_WidthFixed, 200.0f * uiScale);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, 220.0f * uiScale);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 160.0f * uiScale);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < patches.size(); i++) {
            auto& patch = patches[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Checkbox("##on", &patch.enabled)) {
                patches_dirty = true;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", patch.name.c_str());
            if (!patch.note.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", patch.note.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            const bool mismatch =
                !installed.empty() && !patch.app_ver.empty() && patch.app_ver != installed;
            if (mismatch) {
                ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
            }
            ImGui::Text("%s", patch.app_ver.empty() ? "any" : patch.app_ver.c_str());
            if (mismatch) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Your game is version %s. Most patches only apply to the "
                                      "exact version they were made for.",
                                      installed.c_str());
                }
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", patch.author.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s", patch.repo.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::BeginDisabled(!patches_dirty);
    if (ImGui::Button("Save selection")) {
        std::string error;
        if (Patches::SaveEnabled(patches, error)) {
            patch_status = "Saved. The enabled patches apply the next time the game starts.";
            patch_status_error = false;
            LoadPatchList();
        } else {
            patch_status = error;
            patch_status_error = true;
        }
    }
    ImGui::EndDisabled();
    if (!patch_status.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, patch_status_error ? ErrorColor : SuccessColor);
        ImGui::TextWrapped("%s", patch_status.c_str());
        ImGui::PopStyleColor();
    }
}

void PatchWindow::DrawSonyTab(float uiScale) {
    ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
    ImGui::TextWrapped("Official updates from Sony's server are encrypted for retail consoles: "
                       "download them for your own PS4. shadPS4 can't install them.");
    ImGui::PopStyleColor();

    const bool busy = looking_up || downloading;
    ImGui::BeginDisabled(busy);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Title ID");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f * uiScale);
    const bool submitted =
        ImGui::InputText("##SonyTitleId", sony_title_id.data(), sony_title_id.size(),
                         ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Check for update") || submitted) {
        StartLookup();
    }
    ImGui::EndDisabled();

    if (looking_up) {
        ImGui::TextDisabled("Asking Sony's update server...");
        return;
    }

    std::optional<Sony::UpdateInfo> info;
    std::optional<Sony::DownloadResult> result;
    {
        std::scoped_lock lock{mutex};
        info = update_info;
        result = download_result;
    }
    if (!info) {
        return;
    }
    if (!info->found) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              info->error.starts_with("Sony has no") ? WarningColor : ErrorColor);
        ImGui::TextWrapped("%s", info->error.c_str());
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginDisabled(downloading);
    if (ImGui::Button(
            fmt::format("Download {} ({})", info->version, Sony::FormatSize(info->size)).c_str())) {
        StartDownload();
    }
    ImGui::SameLine();
    if (ImGui::Button("Change folder...")) {
        ImGuiFileDialog::Instance()->OpenDialog(DownloadDirDialogKey, "Choose a download folder",
                                                nullptr, Common::FS::PathToUTF8String(download_dir),
                                                "", 1, nullptr,
                                                ImGuiFileDialogFlags_DontShowHiddenFiles);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("to %s", Common::FS::PathToUTF8String(download_dir).c_str());
    ImGui::EndDisabled();

    if (downloading) {
        const u64 total = download_progress.bytes_total;
        const u64 done = download_progress.bytes_done;
        const auto label =
            fmt::format("{}{} / {} (part {} of {})",
                        download_progress.verifying ? "Checking existing data: " : "",
                        Sony::FormatSize(done), Sony::FormatSize(total),
                        download_progress.piece.load(), download_progress.pieces.load());
        ImGui::ProgressBar(total == 0 ? 0.0f : static_cast<float>(done) / total,
                           ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, 0.0f), label.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel download")) {
            download_cancel = true;
        }
    } else if (result) {
        ImGui::PushStyleColor(ImGuiCol_Text, result->success ? SuccessColor : ErrorColor);
        ImGui::TextWrapped("%s", result->message.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (ImGui::BeginTable("SonyUpdate", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 300.0f * uiScale);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        DetailRow("Title", info->title);
        DetailRow("Latest version", info->version);
        DetailRow("Size", Sony::FormatSize(info->size));
        DetailRow("Type", info->type.empty() ? "-" : info->type);
        DetailRow("Needs PS4 firmware", FirmwareString(info->system_ver));
        DetailRow("Content ID", info->content_id);
        ImGui::EndTable();
    }
}

void PatchWindow::Draw(bool* open, float uiScale) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Patches and Updates", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::DrawPrettyBackground();
    ImGui::SetWindowFontScale(uiScale);

    Overlay::TextCentered("Patches and Updates");
    ImGui::Dummy(ImVec2(0.0f, 10.f * uiScale));

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Game");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    const std::string preview =
        (game_index >= 0 && game_index < static_cast<int>(games.size()))
            ? fmt::format("{} [{}]", games[game_index].title, games[game_index].serial)
            : std::string{"Other title ID"};
    if (ImGui::BeginCombo("##Game", preview.c_str())) {
        for (int i = 0; i < static_cast<int>(games.size()); i++) {
            const auto label = fmt::format("{} [{}]##{}", games[i].title, games[i].serial, i);
            if (ImGui::Selectable(label.c_str(), i == game_index)) {
                SelectGame(i);
            }
        }
        if (ImGui::Selectable("Other title ID", game_index < 0)) {
            game_index = -1;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f * uiScale);
    if (ImGui::InputText("##ManualSerial", manual_serial.data(), manual_serial.size(),
                         ImGuiInputTextFlags_CharsUppercase)) {
        game_index = -1;
        for (int i = 0; i < static_cast<int>(games.size()); i++) {
            if (games[i].serial == manual_serial.data()) {
                game_index = i;
            }
        }
        CopyToBuffer(sony_title_id, std::string{manual_serial.data()});
        LoadPatchList();
    }

    const ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;
    ImGui::BeginChild("PatchContent", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), child_flags);
    if (ImGui::BeginTabBar("PatchTabs")) {
        if (ImGui::BeginTabItem("Community Patches")) {
            DrawCommunityTab(uiScale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sony Updates")) {
            DrawSonyTab(uiScale);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    if (ImGui::Button("Close")) {
        sync_cancel = true;
        download_cancel = true;
        *open = false;
    }
    if (IsBusy()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Closing cancels the running download.");
    }

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    if (ImGuiFileDialog::Instance()->Display(DownloadDirDialogKey, ImGuiWindowFlags_NoMove)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            download_dir = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(
                ImGuiFileDialog::Instance()->GetCurrentPath().c_str())));
        }
        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::End();
}

} // namespace BigPictureMode
