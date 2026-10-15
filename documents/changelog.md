<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4-pkg changelog

Changes made in **shadPS4-pkg**, a personal fork of [shadPS4](https://github.com/shadps4-emu/shadPS4) that tracks the latest upstream build and re-adds the PKG Installer. Newest entries go at the top. The upstream shadPS4 release history follows below.

15/10/2026 - PKG Installer, patch downloader and Sony updates
=================

**PKG Installer (restored from shadPS4 0.7.0)**

- Brought back the PKG reader (`pkg.cpp`, `pkg_type.cpp`) and its crypto. The crypto now runs on the LibreSSL already bundled with shadPS4 instead of Crypto++, which upstream no longer ships
- New **Install PKG** window in the launcher: choose or drop a `.pkg`, pick the game folder, see title/version/type/destination, confirm before overwriting, progress bar and cancel
- New `--install-pkg`, `--install-dir`, `--overwrite` and `--delete-pkg` command-line options
- Games install to `<game folder>/<TITLE ID>`, updates to `<game folder>-UPDATE` (which current shadPS4 overlays automatically), DLC to the addon folder
- Safer than 0.7.0: malformed, truncated or retail PKGs fail with a clear message instead of reading out of bounds, partial installs are cleaned up, and free disk space is checked first
- Fixed a 0.7.0 bug where PKGs whose file table extends past the first decrypted chunk read past the end of that buffer; those blocks are now decrypted from the file

**Patch downloader**

- New **Patches** window with a Community Patches tab: downloads the GoldHEN (illusion0001) and shadPS4 patch collections, lists the patches for a game, and saves which ones are enabled. Re-downloading keeps your selection
- New `--sync-patches` command-line option

**Sony update lookup**

- Sony Updates tab: looks up a title's latest official update on Sony's update server and downloads the PKG in pieces, verifying each piece's SHA-1 and resuming interrupted downloads
- New `--check-update` and `--download-update` (with `--output-dir`) command-line options

**Launcher**

- Running shadPS4 with no arguments now opens the launcher instead of a "this is a CLI application" message
- W/A/S/D still navigate the launcher, but now type normally while a text box is active

**Tested** on Linux with two homebrew fPKGs (Apollo Save Tool, Homebrew Store): every extracted PNG and font checksum verified, 270 corrupted PKGs installed or rejected without a crash. Game rendering could not be tested without a GPU.

15/10/2026 - Fork created
=================

- Forked from upstream shadPS4 at commit `805e12c` (usbd: Implement libSceUsbStorage HLE for virtual USB media access, #5338)
- Reworked the README for the fork: unofficial-fork notice, "What's different" table, status checklist, and upstream credits
- Logo now links to the original image in the upstream repository
- Added an AI-assistance disclosure (Claude Opus 5.5) to the README
- Updated the Linux and Windows build guides to clone this fork and to direct build problems away from the upstream team

---

# Upstream shadPS4 changelog

v0.4.0 31/10/2024 - codename divicius
=================

- Shader recompiler fixes
- Emulated support for cpus that doesn't have SSE4.2a (intel cpus)
- Frame graph + Precise 60 fps timing
- Save data: fix nullptr & concurrent file write
- Auto Update
- Error dialog implementation
- Swapchain recreation and window resizing
- Add playback of background/title music in game list
- Kernel: Quiet sceKernelWaitEventFlag error log on timeout
- Improve keyboard navigation in game list
- core/memory: Pooled memory implementation
- Fix PKG loading
- replace trophy xml assert with error
- Refactor audio handling with range checks, buffer threshold, and lock
- audio_core: Fix return value types and shift some error handling to library
- Devtools: PM4 Explorer
- Initial support of Geometry shaders
- Working touchpad support
- net: Stub sceNetErrnoLoc
- Add support to click touchpad using back button on non PS4/5 controllers
- Multiple Install Folders
- Using a more standard data directory for linux
- video_core: Implement sceGnmInsertPushColorMarker
- ime_dialog: Initial implementation
- Network libs fixes
- Use GetSystemTimePreciseAsFileTime to fix fps timing issues
- Added adaptive mutex initializer
- Small Np + trophy fixes
- Separate Updates from Game Folder
- Minor Fixes for Separate Update Folder
- AvPlayer: Do not align w/h to 16 with vdec2
- Improve sceSystemServiceReceiveEvent stub
- renderer_vulkan: Commize and adjust buffer bindings
- Add poll interval to libScePad
- Add more surface format mappings.
- vulkan: Report only missing format feature flags.
- IME implementation
- Videodec2 implementation
- path_util: Make sure macOS has current directory set and clean up path code.
- Load LLE modules from sys_modules/GAMEID folder

v0.3.0 23/09/2024 - codename broamic
=================

- Cheat/Patching support
- DLC support
- New translations support (26 languages)
- Support for unlocking trophies
- Support for more controllers (Dualshock and Xbox)
- Many GUI improvements
- AVplayer

v0.2.0 15/08/2024 - codename validptr
=================
- Adding macOS support
- Big shader recompiler improvements
- Core improvements
- GUI improvements

v0.1.0 01/07/2024 - codename madturtle
=================
- Added a shader recompiler, with this we have a lot of games that starts to work
- Rewrote a big part of core

v0.0.3 23/03/2024 - codename salad
=================
- Switching to std::thread
- Use unique_ptr where possible
- Replace printf/scanf with type safe fmt
- Implemented sceKernelGetProcessTime
- Implemented sceKernelGetProcessTimeCounter, sceKernelGetProcessTimeCounterFrequency
- Pause emu with P button
- Timers rewrote with std::chrono
- Added sceSystemServiceGetStatus
- Initial FileSystem implementation
- Initial TLS work
- New logging implementation
- Some functions implemented for userService, systemService
- Added sceAudioOut module and output using SDL audio

v0.0.2 21/10/2023
=================
- Using cstdint header in variable types
- run_main_entry: Rewrite in asm for stack setup
- Printf libc implementation for work with sysv_abi
- Initial pad emulation (only digital pad atm)
- Implemented sceVideoOutIsFlipPending
- Added auto stubs, now unsupported hle function will resolve as empty stubs
- Rewrote libc_cxa functions
- Libc implementations ( _ZdlPv,_Znwm,rand,_Fsin,qsort,free,strncpy,memmove,atan2f,pow,_Sin)
- ET_SCE_DYNAMIC behaves as valid for execution now
- Initial FileSystem work (not yet usable)

v0.0.1 29/09/2023
=================
First public release. Everything is new.