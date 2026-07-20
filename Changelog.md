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
- merged the separate FAT32 and exFAT installer builds into a single `FMCBInstaller.elf`. It embeds both USB stacks and loads the right one from the launch path at runtime: the BDM stack (FAT32 **and** exFAT, plus MX4SIO / iLink, `mass0:`) by default, falling back to the legacy `usbhdfsd` driver only when launched from a bare `mass:` path. No more `FMCBInstaller_EXFAT.elf`
