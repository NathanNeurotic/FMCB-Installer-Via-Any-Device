Package made by Matias Israelson (AKA: El_isra)
get the latest package here: https://github.com/israpps/FreeMcBoot-Installer/releases
FreeMcBoot installer originally made by sp193

Changes:

- changed icon Flags:
	* `B?EXEC-SYSTEM` will be shown as "ps2 software"
	* `SYS-CONF` will be shown as "settings"
- Changed icons:
  * `B?EXEC-SYSTEM` By SpaceCoyote
  * `SYS-CONF` By SpaceCoyote
- added 1.965 version with:
  * Poweroff external utility bundled
- changed background, font, and font color for all installers
- Added 1.953 version fused inside 1.965 package to avoid old magicgate binding issues caused by the program
- 1.965 and 1.953: replace original FSCK with 1.966 FSCK (updated), and removed old font, wose size was 16mb (thats a lot because FSCK goes to `__system` partition, wich is 128mb sized, (so that font took 1/8 of total size)
- added manual HDD formatting feature
- updated lang
- normal install variants names made noob-friendly
- installers can now detect and inform rare consoles and tell the user (ie: normal PS2 with `1.80` ROM)
- blocked multi-install, no need for it
- System update folders `B?EXEC-SYSTEM` will have icon.sys variations for easy identification. ie: japanese system update folder (`BIEXEC-SYSTEM`) will be shown on OSD as "FreeMcBoot (japan)"
- OPL 1.0.0 bundled in package
- replaced uLaunchELF 4.43x `41e4ebe` with uLaunchELF 4.43x_isr
- add uLaunchELF 4.43x_isr_hdd on FreeHdBoot install pacakge
- HDD APPS partition will hold OPL 1.0.0 and uLaunchELF 4.43x_isr in KELF format, ready to be executed from HDD-OSD
- (related to previous entry) modified HDD APPS partition Header attributes to allow executiuon of uLaunchELF KELF
- all the installers are rebuilt with ps2dev:v1.0
- rebuilt against the latest ps2dev SDK (unified toolchain / gcc 15); build now uses `ps2dev/ps2dev:latest`
- the installer can now be launched from **any** supported device instead of USB only — USB (FAT & exFAT/BDM, MX4SIO, iLink), memory card, internal HDD, MMCE (SD2PSX / MemCard PRO), and PCSX2 `host:`. This fixes it not launching from the FreeMcBoot / OSDMenu (previously it hard-exited unless the working directory was `mass:`)
- added MMCE (`mmceman`) support so the installer and its `INSTALL/` payload can live on an SD2PSX / MemCard PRO card
- merged the separate FAT32 and exFAT installer builds into a single `FMCBInstaller.elf`. No more `FMCBInstaller_EXFAT.elf`
- the single build now detects its boot transport from `argv[0]` and loads **only** the matching storage backend at runtime (the way wLaunchELF-R3Z does), so it is compatible with whatever device/naming a launcher hands it without loading every backend at once:
  * USB via BDM (`massN:`/`usbN:`, FAT32 **and** exFAT) — default
  * MX4SIO SD (`mx4sioN:`)
  * ATA/BDM FAT partition (`ataN:`)
  * SD2PSX / MemCard PRO SD via MMCE (`mmceN:`)
  * disc via CDFS (`cdfs:`)
  * memory card (`mcN:`), internal HDD (`hddN:`/`pfsN:`), and PCSX2 `host:`
  * legacy `usbhdfsd` for a bare `mass:` launch
- the BDM stack (`bdm`/`bdmfs_fatfs`/`usbmass_bd`/`mx4sio_bd`) uses the wLaunchELF-R3Z matched module set (R3Z's `bdm`/`bdmfs_fatfs`/`usbmass_bd` are byte-identical to this project's committed copies), so MX4SIO runs on the same BDM core that boots USB; `ata_bd` comes from the SDK, paired with that core exactly as R3Z does. (An earlier attempt using the latest SDK BDM modules black-screened on hardware — reverted.)
- on an ATA boot (APAJail), `ata_bd` provides **both** the `atad` interface that `ps2hdd` needs and the BDM block device that `bdmfs_fatfs` needs, so `ps2hdd` rides `ata_bd` directly (no `ps2atad`). The exFAT payload and the APA partitions coexist on one drive, so you can boot the installer from the ATA/exFAT partition **and** install FreeHdBoot to the APA side of the same drive — the way wLaunchELF-R3Z does
- reworked what the installer copies, and where:
  * **`INSTALL/APPS`** — on a memory card install its **contents** are copied to the **root of the card** (`INSTALL/APPS/app_dir1` → `mc0:/app_dir1`); no `APPS` folder is created on the card any more. On an HDD install the **whole folder** is installed to the standard **`__common`** partition as `__common:/APPS`
  * **`INSTALL/HDD_OSD/sysconf`** (new, optional) — contents are copied into the root of the **`__sysconf`** partition. Note this is *not* the same as the memory card's `SYS-CONF` folder
  * **`INSTALL/HDD_OSD/system`** (new, optional) — contents are copied into the root of the **`__system`** partition
  * **`INSTALL/BOOT`** is still installed to the card's `BOOT` folder as before (kept so users can still launch an exit/file-browser app); **`INSTALL/BOOT-HDD`** and **`INSTALL/APPS-HDD`** are no longer installed at all
  * the dedicated `PP.FHDB.APPS` HDD partition is no longer created, and the code that built its HDD-OSD attribute area has been removed entirely — so `SYSTEM/AICON.SYS`, `SYSTEM/AICON.ICN` and `SYSTEM/ASYSTEM.CNF` are no longer used or referenced at all (unrelated to the regional `B?ICON.SYS` files, which are still used)
- new installer background
- fixed MC dump/restore freezing: modern ps2sdk dropped `sio_printf`, and the shim replacing it (buffer + newlib `vsnprintf`) needs far more stack than the original. The dump/restore worker thread only had 2KB and overflowed. Worker thread 2KB -> 8KB, IOP init thread 4KB -> 8KB, main stack 8KB -> 32KB, and the shim's buffer 512 -> 256 bytes
- the `INSTALL/` payload path is now built by inserting exactly one `/` between the launch directory and `INSTALL` and probing candidate device names, so it resolves whether or not the launcher's `argv[0]` keeps a trailing slash and whichever name (bare/numbered, `massN:`/`usbN:`) it uses
