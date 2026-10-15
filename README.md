<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

<h1 align="center">
  <br>
  <a href="https://github.com/shadps4-emu/shadPS4/blob/main/.github/shadps4.png"><img src="https://raw.githubusercontent.com/shadps4-emu/shadPS4/main/.github/shadps4.png" alt="shadPS4 logo" width="220"></a>
  <br>
  <b>shadPS4-pkg</b>
  <br>
</h1>

<p align="center">
  A personal fork of <a href="https://github.com/shadps4-emu/shadPS4"><b>shadPS4</b></a> that tracks the latest upstream build and brings back the <b>PKG Installer</b>.
</p>

<p align="center">
  <a href="https://github.com/shadps4-emu/shadPS4"><img src="https://img.shields.io/badge/upstream-shadPS4-8A2BE2"></a>
  <a href="./LICENSE"><img src="https://img.shields.io/badge/license-GPL--2.0-blue"></a>
  <img src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20FreeBSD-lightgrey">
</p>

> [!WARNING]
> **This is an unofficial fork for personal use.** It is not affiliated with or supported by the shadPS4 team.
> Please **do not** report problems from this fork to the upstream shadPS4 repository, Discord, or compatibility list. If something breaks here, test it on an [official shadPS4 build](https://github.com/shadps4-emu/shadPS4/releases) first.

# What's different in this fork

| | Upstream shadPS4 | shadPS4-pkg |
|---|:---:|:---:|
| Latest emulator core | ✅ | ✅ (synced from upstream) |
| Install games, updates and DLC from `.pkg` files | ❌ (removed after 0.7.0) | ✅ Restored |
| Community patch downloader (GoldHEN + shadPS4 repos) | ❌ | ✅ |
| Official update lookup and download from Sony's servers | ❌ | ✅ |
| Opens a launcher when started with no arguments | ❌ | ✅ |

The PKG Installer was removed from upstream shadPS4 after 0.7.0. This fork brings it back on top of the current codebase, in the built-in launcher and on the command line, so `.pkg` files can be extracted and installed into a game folder without a separate tool.

Everything else (the emulator core, compatibility, and settings) follows upstream as closely as possible.

# Status

> [!IMPORTANT]
> shadPS4 itself is early in development, so many games still won't run. Don't expect a flawless experience.

- [x] Fork synced with the latest upstream shadPS4
- [x] PKG Installer re-implemented (window + `--install-pkg`)
- [x] Community patch downloader
- [x] Sony update lookup and downloader

See the [**changelog**](./documents/changelog.md) for what has changed in this fork.

# Building

This fork builds exactly the same way as upstream. Follow the guide for your platform:

- [**Windows**](./documents/building-windows.md)
- [**Linux**](./documents/building-linux.md)
- [**macOS**](./documents/building-macos.md)
- [**Docker**](./documents/building-docker.md) (containerized build with VSCode)

> [!IMPORTANT]
> macOS users need at least macOS 26.0 to run shadPS4. Intel Macs are not supported.

No extra libraries are needed for the new features: they use the SDL, Dear ImGui, LibreSSL and cpp-httplib copies that already ship in `externals/`.

# Usage

### The launcher

Start shadPS4 **with no arguments** (or with `-b`) to open the launcher. It lists your installed games; click one to play. The buttons along the bottom open:

- **Install PKG**: pick a `.pkg` (or drop one on the window), choose a game folder, and install. It shows the title, version, type (game, update or DLC) and where it will go, and asks before overwriting anything.
- **Patches**: the patch and update window (see below).
- **Settings**: game folders and emulator options.

### Command line

Run `shadPS4 --help` for the full list of commands. Common patterns:

```sh
shadPS4 CUSA00001                                     # Find a game folder called CUSA00001 in your install folders and boot it
shadPS4 --fullscreen true --config-clean CUSA00001    # The game argument is always last...
shadPS4 -g CUSA00001 --fullscreen true --config-clean # ...unless you specify it with -g
shadPS4 /path/to/game.elf                             # Boot a PS4 ELF directly (useful if it isn't named eboot.bin)
shadPS4 CUSA00001 -- -flag1 -flag2                    # Pass -flag1 and -flag2 to the game executable in argv
```

### Installing a PKG

From the launcher, use **Install PKG**. From the command line:

```sh
shadPS4 --install-pkg Game.pkg                          # Install into your first game folder
shadPS4 --install-pkg Game.pkg --install-dir D:/Games   # Install into a specific folder
shadPS4 --install-pkg Game.pkg --overwrite --delete-pkg # Replace an existing install, then delete the .pkg
```

Where things go:

| PKG type | Installed to |
|---|---|
| Game | `<game folder>/<TITLE ID>` |
| Update | Next to the game, in `<game folder name>-UPDATE` (shadPS4 overlays it on the game) |
| DLC | `<addon folder>/<TITLE ID>/<entitlement>` (set the addon folder with `--set-addon-folder`) |

> [!NOTE]
> Only **fake (fPKG) and homebrew** PKGs can be installed, the same as shadPS4 0.7.0. Retail PKGs, including official updates from Sony, are encrypted with console keys and can't be decrypted here. An update needs its base game installed first.

### Patches and official updates

Open **Patches** in the launcher and pick a game (or type any title ID).

- **Community Patches** downloads the patch collections from [GoldHEN / illusion0001](https://github.com/illusion0001/PS4-PS5-Game-Patch) and [shadPS4](https://github.com/shadps4-emu/ps4_cheats), lists the ones for your game, and lets you switch them on. Enabled patches apply automatically when the game boots (start with `-i` to skip them). Most patches only work on the exact game version they were made for; mismatches are highlighted.
- **Sony Updates** looks up a title's latest official update on Sony's public update server and downloads the PKG, checking every piece's checksum. Interrupted downloads resume. These PKGs are for your own PS4; shadPS4 can't install them.

The same is available from the command line:

```sh
shadPS4 --sync-patches                                     # Download or update both patch collections
shadPS4 --check-update CUSA00900                           # Show the latest official update for a title
shadPS4 --download-update CUSA00900 --output-dir D:/PS4    # Download it (default: user/download)
```

> [!TIP]
> Behind a proxy? The downloaders honor `HTTPS_PROXY`, `HTTP_PROXY` and `NO_PROXY`. Mirrors can be set with `SHADPS4_PKG_GITHUB_API`, `SHADPS4_PKG_GITHUB_RAW` and `SHADPS4_PKG_SONY_UPDATE_BASE`.

# Keyboard and mouse mappings

> [!NOTE]
> Some keyboards need the Fn key held for the F-keys. On macOS, use Command instead of Control, and Command+F11 for fullscreen.

| Key | Function |
|---|---|
| F10 | FPS counter |
| Ctrl+F10 | Video debug info |
| F11 | Fullscreen |
| F12 | RenderDoc capture (or game-only screenshot if RenderDoc is unavailable) |
| Alt+F12 | Screenshot including HUD/dialog overlays |

Xbox and DualShock controllers work out of the box. Default keyboard layout:

| Controller | Keyboard | Controller | Keyboard |
|---|---|---|---|
| Left stick up / down | W / S | Right stick up / down | I / K |
| Left stick left / right | A / D | Right stick left / right | J / L |
| Triangle | Numpad 8 or C | Circle | Numpad 6 or B |
| Cross | Numpad 2 or N | Square | Numpad 4 or V |
| D-pad up / down | Up / Down | D-pad left / right | Left / Right |
| L1 / R1 | Q / U | L2 / R2 | E / O |
| L3 / R3 | X / M | Options | Return |
| Touch pad / Back | Space | | |

Bindings can be customized per game from the Controller button in the settings menu. A binding supports up to three keys, mouse buttons, mouse-to-joystick movement, and more.

# Firmware files

shadPS4 can load some PlayStation 4 firmware modules. These go in the `sys_modules` folder:

<div align="center">

| Modules | Modules | Modules | Modules |
|---|---|---|---|
| libSceAt9Enc.sprx | libSceAudiodec.sprx | libSceAudiodecCpu.sprx | libSceAudiodecCpuDdp.sprx |
| libSceAudiodecCpuDtsHdLbr.sprx | libSceAudiodecCpuHevag.sprx | libSceAudiodecCpuM4aac.sprx | libSceAvPlayer.sprx |
| libSceAvPlayerStreaming.sprx | libSceBeisobmf.sprx | libSceBemp2sys.sprx | libSceCesCs.sprx |
| libSceFont.sprx | libSceFontFt.sprx | libSceFreeTypeOl.sprx | libSceFreeTypeOptOl.sprx |
| libSceFreeTypeOt.sprx | libSceJpegDec.sprx | libSceJpegEnc.sprx | libSceJson.sprx |
| libSceJson2.sprx | libSceLibcInternal.sprx | libSceNgs2.sprx | libScePngEnc.sprx |
| libScePsmKitSystem.sprx | libSceRtc.sprx | libSceRudp.sprx | libSceSystemGesture.sprx |
| libSceUlt.sprx | libSceWkFontConfig.sprx | libSceXml.sprx | libSceDepth.sprx |
| libScePadTracker.sprx | libSceMoveTracker.sprx | | |

</div>

> [!CAUTION]
> These modules, and any games or PKG files you use, must be dumped from a PlayStation 4 and content you legally own.

# Debugging

See the upstream [**Debugging documentation**](./documents/Debugging/Debugging.md) for how to test and debug the emulator. Remember to reproduce issues on an official build before reporting anything upstream.

# Credits

All emulator work belongs to the [**shadPS4 team**](https://github.com/shadps4-emu/shadPS4) and its contributors. This fork only re-adds the PKG Installer on top of their work. If you find shadPS4 useful, consider supporting the original project on [**Ko-fi**](https://ko-fi.com/shadps4).

**Upstream main team:** [georgemoralis](https://github.com/georgemoralis), [psucien](https://github.com/psucien), [viniciuslrangel](https://github.com/viniciuslrangel), [roamic](https://github.com/roamic), [squidbus](https://github.com/squidbus), [frodo](https://github.com/baggins183), [Stephen Miller](https://github.com/StevenMiller123), [kalaposfos13](https://github.com/kalaposfos13). Logo by [Xphalnos](https://github.com/Xphalnos).

<a href="https://github.com/shadps4-emu/shadPS4/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=shadps4-emu/shadPS4&max=24">
</a>

Upstream also thanks [Panda3DS](https://github.com/wheremyfoodat/Panda3DS), [fpPS4](https://github.com/red-prig/fpPS4), yuzu, [felix86](https://github.com/OFFTKP/felix86), and [emudev.org](https://emudev.org/) for their help along the way.

# License

Like upstream, this fork is licensed under the [**GPL-2.0-or-later**](./LICENSE).

---

<p align="center">
  <a href="https://www.anthropic.com/claude"><img src="https://img.shields.io/badge/AI--assisted-Claude%20Opus%205.5-D97757?logo=claude&logoColor=white" alt="AI-assisted with Claude Opus 5.5"></a>
</p>

> [!NOTE]
> **AI disclosure:** This fork is developed with assistance from **Claude Opus 5.5**, an AI model by Anthropic. AI-generated content, including this README, may contain mistakes. This disclosure is not an endorsement by Anthropic or the shadPS4 team.
