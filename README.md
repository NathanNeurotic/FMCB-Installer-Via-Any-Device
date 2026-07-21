# FMCB-Installer-Modernized

A modernized fork of israpps' [FreeMcBoot-Installer](https://github.com/israpps/FreeMcBoot-Installer), with two goals:

1. **Builds on the latest ps2dev SDK.** The installer now compiles against `ps2dev/ps2dev:latest` (unified toolchain / gcc 15) instead of the old `ps2dev:v1.0`. The SDK-migration details are in the [Changelog](Changelog.md).
2. **Boots and installs from any device — not just USB.** The original hard-exited unless it was launched from a `mass:` (USB) path, so it wouldn't run from the FreeMcBoot / OSDMenu. It now detects its boot device from `argv[0]` (the way wLaunchELF‑R3Z does) and reads its `INSTALL/` payload from wherever it was launched:

   | Device | Path prefix | Status |
   |---|---|---|
   | USB — FAT32 **and** exFAT (BDM) | `mass:` / `massN:` / `usbN:` | ✅ hardware‑tested |
   | Memory card | `mcN:` | ✅ supported |
   | Internal HDD (APA / PFS) | `hddN:` / `pfsN:` | ✅ supported |
   | MMCE — SD2PSX / MemCard PRO | `mmceN:` | ✅ supported |
   | MX4SIO (SD in MC slot) | `mx4sioN:` | ✅ supported |
   | ATA / BDM exFAT + APA (APAJail) | `ataN:` | ✅ supported¹ |
   | Disc | `cdfs:` / `cdromN:` | ✅ supported |
   | PCSX2 host | `host:` | ✅ supported |

   This single build replaces the old FAT32/exFAT variant split: it embeds the storage backends and loads only the one matching the launch device at runtime.

   ¹ On an ATA boot the installer loads `ata_bd`, which provides **both** the ATA (`atad`) interface that `ps2hdd` needs **and** the BDM block device that `bdmfs_fatfs` needs — so `ps2hdd` rides `ata_bd` directly, with no separate `ps2atad`. That lets an exFAT partition and the APA (Sony HDD) format coexist on one drive (**APAJail**): you can boot the installer from the ATA/exFAT partition **and** install FreeHdBoot to the APA side of the same drive — exactly the way wLaunchELF‑R3Z does it.

## Building

Inside the `ps2dev/ps2dev:latest` toolchain (the base image needs `make`, e.g. `apk add build-base`):

```sh
cd installer
make rebuild                                    # single "any device" build  ->  FMCBInstaller.elf
make rebuild EXTRA_CFLAGS=-DDIAG_INSTALL_PATH   # debug build: shows the resolved payload path on screen
```

The GitHub Actions workflow (`.github/workflows/compile-core.yml`) builds the full per‑version `.7z` release packages.

## Notes

- **USB boot + install is hardware‑tested.** The other devices are implemented; verify on hardware.
- The BDM stack (`bdm` / `bdmfs_fatfs` / `usbmass_bd` / `mx4sio_bd`) uses the **wLaunchELF‑R3Z matched module set** (`installer/irx/compiled/` — R3Z's `bdm`/`bdmfs_fatfs`/`usbmass_bd` are byte‑identical to this project's), so MX4SIO runs on the same BDM core that boots USB. `ata_bd` comes from the SDK, paired with that core exactly as R3Z does.

---

[![Codacy Badge](https://api.codacy.com/project/badge/Grade/3a7e81446817406a94eeb77bcc3762dd)](https://app.codacy.com/gh/israpps/FreeMcBoot-Installer?utm_source=github.com&utm_medium=referral&utm_content=israpps/FreeMcBoot-Installer&utm_campaign=Badge_Grade_Settings)
[![Build [All]](https://github.com/israpps/FreeMcBoot-Installer/actions/workflows/compile-core.yml/badge.svg)](https://github.com/israpps/FreeMcBoot-Installer/actions/workflows/compile-core.yml)

[![GitHub release (latest by SemVer and asset including pre-releases)](https://img.shields.io/github/downloads-pre/israpps/FreeMcBoot-Installer/latest/FMCB-1966.7z?color=black&label=&logo=GitHub)](https://github.com/israpps/FreeMcBoot-Installer/releases/tag/latest)
[![GitHub release (latest by SemVer and asset including pre-releases)](https://img.shields.io/github/downloads-pre/israpps/FreeMcBoot-Installer/latest/FMCB-1965.7z?color=black&label=&logo=GitHub)](https://github.com/israpps/FreeMcBoot-Installer/releases/tag/latest)
[![GitHub release (latest by SemVer and asset including pre-releases)](https://img.shields.io/github/downloads-pre/israpps/FreeMcBoot-Installer/latest/FMCB-1964.7z?color=black&label=&logo=GitHub)](https://github.com/israpps/FreeMcBoot-Installer/releases/tag/latest)
[![GitHub release (latest by SemVer and asset including pre-releases)](https://img.shields.io/github/downloads-pre/israpps/FreeMcBoot-Installer/latest/FMCB-1963.7z?color=black&label=&logo=GitHub)](https://github.com/israpps/FreeMcBoot-Installer/releases/tag/latest)
[![GitHub release (latest by SemVer and asset including pre-releases)](https://img.shields.io/github/downloads-pre/israpps/FreeMcBoot-Installer/latest/FMCB-1953.7z?color=black&label=&logo=GitHub)](https://github.com/israpps/FreeMcBoot-Installer/releases/tag/latest)

[![GitHub release (by tag)](https://img.shields.io/github/downloads/israpps/FreeMcBoot-Installer/APPS/total?color=000000&label=Apps%20Pack)](https://github.com/israpps/FreeMcBoot-Installer/releases/tag/APPS)

 Custom installers for FreeMcBoot 1.966, 1.965, 1.953, 1.964 and 1.963

They're packed with updated software.

In addition, several enhancements were made:
+ Installer:
  - Forbid multi install (corrupts memory card filesystem and doesn't achieve anything different than normal install)
  - Renamed normal install options to be user friendly
  - added manual HDD format option
  - added variant of installer that can be launched from exfat USB
+ Installation package:
  - updated Kernel patch updates for SCPH-10000 & SCPH-15000 to the one used on FreeMcBoot 1.966
  - Updated FreeHdBoot FSCK and MBR bootstraps to the one used on FreeHdBoot 1.966
  - added console shutdown ELF to all versions prior to 1.966
  - Optional custom IRX files to make FreeMcBoot/FreeHdBoot support EXFAT USB storage devices
  - internal HDD APPS partition header data changed to allow KELF execution from HDD-OSD.

[Original source code and binaries](https://sites.google.com/view/ysai187/home/projects/fmcbfhdb)

Special Thanks to SP193 for leaving the installer source code! it will help me out to add features to mi wLE mod ^^

-----

<details>
  <summary> <b> APPS Package contents: </b> </summary>

```ini
ESR ESR r10f_direct
[Open PS2 Loader]
1.0.0
latest
0.9.3
0.9.2
0.9.1
0.9.0
0.8
0.7
0.6
0.5
[Cheats]
Cheat device (PAL)
Cheat device (NTSC)
[uLaunchELF]
4.43x_isr
4.43x_isr_hdd
4.43a 41e4ebe
4.43a_khn
4.43a latest
[MultiMedia]
SMS
Argon
[PS2ESDL]
v0.810 OB
v0.825 OB
[GSM]
v0.23x
v0.38
[Emulators]
FCEU
InfoNES
SNES Station (0.2.4S)
SNES Station (0.2.6C)
SNES9x
InfoGB
GPS2
GPSP-KAI
ReGBA
TempGBA
VBAM
PVCS
RetroArch (1.9.1)
[Utilities]
MechaPwn 2.0
LensChanger 1.2b
Padtest
RDRAM TEST
PS2 Ident
HDD Checker v0.964
Memory Card Anihilator 2.0
HWC Language Selector
Launch disc
Shutdown System app
```

</details>
