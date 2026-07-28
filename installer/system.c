#include <iopheap.h>
#include <kernel.h>
#include <fileXio_rpc.h>
#include <libcdvd.h>
#include <libmc.h>
#include <libpad.h>
#include <libpwroff.h>
#include <libsecr-common.h>
#include <hdd-ioctl.h>
#include <loadfile.h>
#include <malloc.h>
#include <sifcmd.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>
#include <usbhdfsd-common.h>
#include <limits.h>

#include <sys/stat.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
/* IMPORTANT: IOP-side calls (fileXio and mc functions) take the IOP flag values
   named FIO_O_xxx and FIO_MT_xxx from <io_common.h> -- NOT the POSIX O_xxx values
   from <sys/fcntl.h>. Old ps2sdk shipped its own <sys/fcntl.h> that merely did
   "#include <io_common.h>", so a plain O_WRONLY was 0x0002 (the IOP value). Modern
   ps2sdk renamed these to FIO_O_xxx and dropped that header, so O_WRONLY now
   resolves to newlib's POSIX 1 (which equals FIO_O_RDONLY) and an IOP file would
   silently open READ-ONLY. Always use the FIO_O_xxx names for these calls. */
#include <io_common.h>

#include <libgs.h>

#include "main.h"
#include "iop.h"
#include "pad.h"
#include "libsecr.h"
#include "mctools_rpc.h"
#include "system.h"
#include "graphics.h"
#include "UI.h"
#include "menu.h"
#include "ReqSpaceCalc.h"

#define IO_BLOCK_SIZE (256 * 512)

extern void *_gp;
/* Modern ps2sdk/newlib defines errno as a macro (via <errno.h>); the old manual
   `extern int errno` declaration collides with it and is no longer needed. */
extern unsigned short int SelectButton, CancelButton;
extern u8 dev9Loaded;
extern int InstallLockSema;

static int InitMCInfo(int port, int slot);
static int SignKELF(void *buffer, int size, unsigned char port, unsigned char slot);
static void GetKbitAndKc(void *buffer, u8 *Kbit, u8 *Kc);
static int SetKbitAndKc(void *buffer, u8 *Kbit, u8 *Kc);
static int TwinSignKELF(const char *RootFolder, void *buffer, int size, unsigned char port, unsigned char slot);
static char GetMGFolderLetter(unsigned char region);
static const char *GetMountParams(const char *command, char *BlockDevice);
static int DeleteFolder(const char *folder);
static int DeleteFolderIfEmpty(const char *folder);
static int AddDirContentsToFileCopyList(const char *RootFolderPath, const char *srcRelativePath, const char *destination, unsigned int CurrentLevel, struct FileCopyTarget **FileCopyList, unsigned int *CurrentNumFiles, unsigned int *CurrentNumDirs, unsigned int *TotalRequiredSpaceForFiles);
static int GetMcFreeSpace(int port, int slot);
static int EnableHDDBooting(void);
static int CopyFilesToHDD(const char *RootFolder, const struct FileCopyTarget *FileCopyList, unsigned int NumFilesEntries, unsigned int TotalNumBytes, unsigned int flags);
static int CreateBasicFoldersOnHDD(unsigned int flags);
static int _CleanupHDDTarget(void);
static int GetAllBaseFileStats(char MGLetter, const char *RootFolder, struct FileCopyTarget *FileCopyList, unsigned int NumFiles, unsigned int *TotalRequiredSpaceForFiles);
static int SyncMCFileWrite(int fd, int size, void *Buffer);
static int CopyFiles(const char *RootFolder, unsigned char port, unsigned char slot, const struct FileCopyTarget *FileCopyList, unsigned int NumFilesEntries, unsigned int TotalNumBytes, unsigned int flags);
static int CreateCrossLinkedFiles(int port, int slot);
static int GenerateUninstallFile(int port, int slot);
static void SetWorkerThreadCommand(int command, const void *arg);
static int GetWorkerThreadCommand(const void **arg);
static int DumpMemoryCard(int port, int slot, FILE *file, unsigned short int PagesPerCluster, const struct MCTools_McSpecData *McSpecData);
static int RestoreMemoryCard(int port, int slot, FILE *file, const struct MCTools_McSpecData *McSpecData);
static void WorkerThread(void *arg);

int GetBootDeviceID(void)
{
    static int BootDevice = -2;
    char path[256];
    int result;

    if (BootDevice < BOOT_DEVICE_UNKNOWN) {
        getcwd(path, sizeof(path));

        /* On the EE there is no kernel notion of a working directory: ps2sdk's
           crt0 derives it once at startup from argv[0] (the directory the ELF was
           launched from, keeping the trailing '/'). So getcwd() already points at
           our own folder on whatever device we were booted from, and the INSTALL/
           payload sits right next to us. Classify the transport from the prefix
           (the way wLaunchELF-R3Z does) so we can load only the matching backend.
           NOTE: mass/usb/mmce/mx4sio/mc/ata/hdd all need careful ordering because
           several share a leading letter; they differ within the first few chars. */
        if (!strncmp(path, "mass", 4) || !strncmp(path, "usb", 3)) // USB (BDM "massN:"/"usbN:" or legacy "mass:")
            result = BOOT_DEVICE_MASS;
        else if (!strncmp(path, "mmce", 4)) // SD2PSX / MemCard PRO SD (mmce0:/mmce1:)
            result = BOOT_DEVICE_MMCE;
        else if (!strncmp(path, "mx4sio", 6)) // MX4SIO SPI SD (mx4sio0:)
            result = BOOT_DEVICE_MX4SIO;
        else if (!strncmp(path, "mc", 2)) // Memory card (mc0:/mc1:)
            result = BOOT_DEVICE_MC;
        else if (!strncmp(path, "ata", 3)) // FAT/exFAT on the ATA bus via BDM (ata0:)
            result = BOOT_DEVICE_ATA;
        else if (!strncmp(path, "hdd", 3) || !strncmp(path, "pfs", 3)) // Internal HDD APA/PFS (hdd0:/pfs0:)
            result = BOOT_DEVICE_HDD;
        else if (!strncmp(path, "cdfs", 4) || !strncmp(path, "cdrom", 5)) // Disc (cdfs:/cdrom0:)
            result = BOOT_DEVICE_CDFS;
        else if (!strncmp(path, "host", 4)) // PCSX2 host: (development)
            result = BOOT_DEVICE_HOST;
        else
            result = BOOT_DEVICE_UNKNOWN;

        BootDevice = result;
    } else
        result = BootDevice;

    return result;
}

/* True when the installer was launched from a bare "mass:" path (an old
   usbhdfsd-based launcher) rather than "mass0:" (BDM) or another device. Used to
   pick the matching USB driver at startup so a single build works with both the
   legacy and the modern USB stacks. */
int IsLegacyMassBoot(void)
{
    char path[256];

    getcwd(path, sizeof(path));

    return (!strncmp(path, "mass:", 5));
}

/* Append one line to FHDBLOG.TXT next to the installer on the boot device.
   The file is opened and closed per line on purpose: if the console locks up,
   whatever was written before the lockup is already on disk, so the last line
   in the log is the last operation that was attempted. */
void InstallLog(const char *format, ...)
{
#ifdef INSTALL_LOG
    char path[300], cwd[256], line[256];
    va_list args;
    int len;
    FILE *file;

    getcwd(cwd, sizeof(cwd));
    for (len = strlen(cwd); len > 0 && cwd[len - 1] == '/';)
        cwd[--len] = '\0';
    snprintf(path, sizeof(path), "%s/FHDBLOG.TXT", cwd);

    if ((file = fopen(path, "a")) == NULL)
        return;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    fputs(line, file);
    fclose(file);
#else
    (void)format; // Build with EXTRA_CFLAGS=-DINSTALL_LOG to enable.
#endif
}

/* Does this directory open? Used to probe candidate payload locations. */
static int PathDirOpens(const char *path)
{
    int fd = fileXioDopen(path);
    if (fd >= 0) {
        fileXioDclose(fd);
        return 1;
    }
    return 0;
}

#ifdef DIAG_INSTALL_PATH
// On-screen diagnostic: show how we resolved the boot device + payload path and
// whether the INSTALL/ folder is actually readable. Enabled only in diag builds.
static void ShowInstallPathDiag(const char *root)
{
    char dbg[512], cwd[256], file[300];
    int fd, st;
    iox_stat_t stat;

    getcwd(cwd, sizeof(cwd));
    fd = fileXioDopen(root);
    if (fd >= 0)
        fileXioDclose(fd);
    snprintf(file, sizeof(file), "%s/SYS-CONF/FMCB_CFG.ELF", root);
    st = fileXioGetStat(file, &stat);

    snprintf(dbg, sizeof(dbg),
             "BootDev=%d  Legacy=%d\ncwd=[%s]\nroot=[%s]\nDopen(root)=%d  Stat(FMCB_CFG.ELF)=%d",
             GetBootDeviceID(), IsLegacyMassBoot(), cwd, root, fd, st);
    ShowMessageBox(SYS_UI_LBL_OK, -1, -1, -1, dbg, SYS_UI_LBL_WARNING);
}
#endif

/* Build the path to our INSTALL/ payload folder (which sits next to the ELF).
   The device NAME our own driver registers may differ from what the launcher put
   in argv[0]: bdmfs_fatfs registers the generic "massN:" AND typed "<transport>N:"
   names, drivers may or may not expose a bare (unit-less) alias, and we load only
   one BDM backend (so its devices are massN by BDM index). So for the BDM-backed
   transports (usb/mass/mx4sio/ata) we PROBE the likely names and use the first
   whose INSTALL/ folder actually opens: exactly what the launcher gave us, then
   the generic massN:/mass0:, then the typed <transport>N:. This makes us accept
   every semantic (usb:/usbN:/mass:/massN:/ata:/ataN:/mx4sio:/mx4sioN:). Non-BDM
   devices (mc/mmce/hdd/pfs/cdfs/host) have stable native names and are used
   verbatim. getcwd() keeps the trailing '/', so we just append "INSTALL". */
void GetInstallRootPath(char *out, int outlen)
{
    char cwd[256], sub[256], cand[300];
    const char *colon;
    int devlen, tlen, sublen;
    char unit;

    getcwd(cwd, sizeof(cwd));

    if ((colon = strchr(cwd, ':')) == NULL) {
        // No "device:" prefix -- strip any trailing '/' and append "/INSTALL".
        snprintf(sub, sizeof(sub), "%s", cwd);
        for (sublen = strlen(sub); sublen > 0 && sub[sublen - 1] == '/'; )
            sub[--sublen] = '\0';
        snprintf(out, outlen, "%s/INSTALL", sub);
        return;
    }

    devlen = (int)(colon - cwd); // device-token length, e.g. 5 for "mass0"

    // Subpath after the colon, with ANY trailing '/' removed. The launcher's cwd
    // may or may not keep a trailing slash (e.g. "mass0:/1.966" vs "mass0:/1.966/"),
    // so we strip it and always insert exactly one '/' before "INSTALL".
    snprintf(sub, sizeof(sub), "%s", colon + 1);
    for (sublen = strlen(sub); sublen > 0 && sub[sublen - 1] == '/'; )
        sub[--sublen] = '\0';

    // transport = device letters minus trailing digits; unit = last digit (or '0').
    tlen = devlen;
    while (tlen > 0 && cwd[tlen - 1] >= '0' && cwd[tlen - 1] <= '9')
        tlen--;
    unit = (tlen < devlen) ? cwd[devlen - 1] : '0';

    if ((tlen == 4 && !strncmp(cwd, "mass", 4)) ||
        (tlen == 3 && !strncmp(cwd, "usb", 3)) ||
        (tlen == 6 && !strncmp(cwd, "mx4sio", 6)) ||
        (tlen == 3 && !strncmp(cwd, "ata", 3))) {
        // Our driver may name the device differently than the launcher did (bare
        // vs numbered, generic massN: vs typed usbN:), so probe the likely names
        // and use the first whose INSTALL/ folder opens.
        snprintf(cand, sizeof(cand), "%.*s:%s/INSTALL", devlen, cwd, sub); // exactly as launched
        if (PathDirOpens(cand)) { snprintf(out, outlen, "%s", cand); return; }
        snprintf(cand, sizeof(cand), "mass%c:%s/INSTALL", unit, sub); // generic mass<unit>
        if (PathDirOpens(cand)) { snprintf(out, outlen, "%s", cand); return; }
        snprintf(cand, sizeof(cand), "mass0:%s/INSTALL", sub); // generic mass0
        if (PathDirOpens(cand)) { snprintf(out, outlen, "%s", cand); return; }
        snprintf(cand, sizeof(cand), "%.*s%c:%s/INSTALL", tlen, cwd, unit, sub); // typed <transport><unit>
        if (PathDirOpens(cand)) { snprintf(out, outlen, "%s", cand); return; }

        // Nothing opened; fall back to generic mass<unit>: and let the caller
        // surface the read error (a diag build shows what was tried).
        snprintf(out, outlen, "mass%c:%s/INSTALL", unit, sub);
        return;
    }

    // mc:/mmce:/hdd:/pfs:/cdfs:/host: -- stable native names, used verbatim.
    snprintf(out, outlen, "%.*s:%s/INSTALL", devlen, cwd, sub);
}

int GetConsoleRegion(void)
{
    static int region = -1;
    FILE *file;

    if (region < 0) {
        if ((file = fopen("rom0:ROMVER", "r")) != NULL) {
            fseek(file, 4, SEEK_SET);
            switch (fgetc(file)) {
                case 'J':
                    region = CONSOLE_REGION_JAPAN;
                    break;
                case 'A':
                case 'H': // Asia, but it uses the same folder as USA.
                    region = CONSOLE_REGION_USA;
                    break;
                case 'E':
                    region = CONSOLE_REGION_EUROPE;
                    break;
                case 'C':
                    region = CONSOLE_REGION_CHINA;
                    break;
            }

            fclose(file);
        }
    }

    return region;
}

int GetConsoleVMode(void)
{
    switch (GetConsoleRegion()) {
        case CONSOLE_REGION_EUROPE:
            return 1;
        default: // All other regions use NTSC
            return 0;
    }
}

/*	Some things to take note of while making a multi-install:
        1. The original release of the SCPH-10000 has boot ROM v1.00, which requires an update to its OSDSYS for argument passing (osdsys.elf).
        2. The SCPH-10000 and SCPH-15000 look for osd110.elf. Both require an update to their OSDSYS programs for argument passing (osd110.elf).
        3. The SCPH-18000 looks for osd130.elf.
        4. All of the above are in BIEXEC-SYSTEM, which should have different content from BAEXEC-SYSTEM, BEEXEC-SYSTEM and BCEXEC-SYSTEM.

    To satisfy the above 4 conditions:
        1. Check the region. If it's Japan, the BIEXEC-SYSTEM folder already exists. Otherwise, create it.
        2. If the console is not a Japanese console or isn't a first-generation SCPH-10000, don't install the system driver updates to BIEXEC-SYSTEM. Unless it's a cross-regional or cross-model install.
        3. Cross link the OSD update files in the system folder, except for the system driver updates for the early Japanese PCMCIA units.
        4. Cross link the remaining region folders with each other (e.g. BEEXEC-SYSTEM is a cross-linked with BAEXEC-SYSTEM if the console uses BAEXEC-SYSTEM).	*/

#define ROM100J_UPDATE_NUM_FILES 1
static struct InstallationFile BootROM100JUpdateFiles[ROM100J_UPDATE_NUM_FILES] = {
    {"SYSTEM/OSDSYS.XLF",
     "BIEXEC-SYSTEM/osdsys.elf",
     FILE_IS_KELF}};

#define ROM101J_UPDATE_NUM_FILES 1
static struct InstallationFile BootROM101JUpdateFiles[ROM101J_UPDATE_NUM_FILES] = {
    {"SYSTEM/OSD110.XLF",
     "BIEXEC-SYSTEM/osd110.elf",
     FILE_IS_KELF}};

#define PS2_SYS_INSTALL_NUM_FILES 2
static struct InstallationFile PS2SysFiles[PS2_SYS_INSTALL_NUM_FILES] = {
    {"SYSTEM/FMCB.XLF",
     "BREXEC-SYSTEM/osdmain.elf",
     FILE_IS_KELF},
    {"SYSTEM/ENDVDPL.XRX",
     "SYS-CONF/endvdpl.irx",
     FILE_IS_KELF}};

#define DEX_SYS_INSTALL_NUM_FILES 1
static struct InstallationFile DEXSysFiles[DEX_SYS_INSTALL_NUM_FILES] = {
    {"SYSTEM/FMCB.XLF",
     "BREXEC-SYSTEM/osdmain.elf",
     FILE_IS_KELF}};

#define PS2_SYS_HDDLOAD_INSTALL_NUM_FILES 3
static struct InstallationFile PS2HDDLOADSysFiles[PS2_SYS_HDDLOAD_INSTALL_NUM_FILES] = {
    {"SYSTEM/DEV9.IRX",
     "BIEXEC-SYSTEM/dev9.irx",
     0},
    {"SYSTEM/ATAD.IRX",
     "BIEXEC-SYSTEM/atad.irx",
     0},
    {"SYSTEM/HDDLOAD.IRX",
     "BIEXEC-SYSTEM/hddload.irx",
     0}};

#define PSX_SYS_INSTALL_NUM_FILES 3
static struct InstallationFile PSXSysFiles[PSX_SYS_INSTALL_NUM_FILES] = {
    {"SYSTEM/XFMCB.XLF",
     "BREXEC-SYSTEM/xosdmain.elf",
     FILE_IS_KELF},
    {"SYSTEM/XUDNL.XRX",
     "BREXEC-SYSTEM/xosdmain.irx",
     FILE_IS_KELF},
    {"SYSTEM/XENDVDPL.XRX",
     "BREXEC-SYSTEM/xendvdpl.irx",
     FILE_IS_KELF}};

#define SYS_FOLDER_RESOURCES_NUM_FILES 2
static struct InstallationFile SysResourceFiles[SYS_FOLDER_RESOURCES_NUM_FILES] = {
    {"SYSTEM/FMCB.ICN",
     "BREXEC-SYSTEM/icon.icn",
     0},
    {"SYSTEM/ICON.SYS",
     "BREXEC-SYSTEM/icon.sys",
     0}};

/* There is deliberately no hardcoded list of SYS-CONF files: the whole contents
   of INSTALL/SYS-CONF are copied to the memory card's SYS-CONF folder (see
   PerformInstallation), so the payload decides what ships. */

/* Only SYSTEM/ sources are listed here. The SYS-CONF files that used to be listed
   individually are now added generically (whole folder -> __sysconf:/FMCB). */
#define HDD_BASE_INSTALL_NUM_FILES 18
static struct InstallationFile HDDBaseFiles[HDD_BASE_INSTALL_NUM_FILES] = {
    // FSCK
    {
        "HDD/FSCK/FSCK.XLF",
        "hdd0:__system:pfs:/fsck/fsck.elf",
        FILE_IS_KELF},
    {"HDD/FSCK/LANG/NotoSans-Bold.ttf",
     "hdd0:__system:pfs:/fsck/lang/NotoSans-Bold.ttf",
     0},
    {"HDD/FSCK/LANG/NotoSansJP-Bold.otf",
     "hdd0:__system:pfs:/fsck/lang/NotoSansJP-Bold.otf",
     0},
    {"HDD/FSCK/LANG/fonts.txt",
     "hdd0:__system:pfs:/fsck/lang/fonts.txt",
     0},
    {"HDD/FSCK/LANG/strings_JA.txt",
     "hdd0:__system:pfs:/fsck/lang/strings_JA.txt",
     0},
    {"HDD/FSCK/LANG/labels_JA.txt",
     "hdd0:__system:pfs:/fsck/lang/labels_JA.txt",
     0},
    {"HDD/FSCK/LANG/strings_FR.txt",
     "hdd0:__system:pfs:/fsck/lang/strings_FR.txt",
     0},
    {"HDD/FSCK/LANG/labels_FR.txt",
     "hdd0:__system:pfs:/fsck/lang/labels_FR.txt",
     0},
    {"HDD/FSCK/LANG/strings_SP.txt",
     "hdd0:__system:pfs:/fsck/lang/strings_SP.txt",
     0},
    {"HDD/FSCK/LANG/labels_SP.txt",
     "hdd0:__system:pfs:/fsck/lang/labels_SP.txt",
     0},
    {"HDD/FSCK/LANG/strings_GE.txt",
     "hdd0:__system:pfs:/fsck/lang/strings_GE.txt",
     0},
    {"HDD/FSCK/LANG/labels_GE.txt",
     "hdd0:__system:pfs:/fsck/lang/labels_GE.txt",
     0},
    {"HDD/FSCK/LANG/strings_IT.txt",
     "hdd0:__system:pfs:/fsck/lang/strings_IT.txt",
     0},
    {"HDD/FSCK/LANG/labels_IT.txt",
     "hdd0:__system:pfs:/fsck/lang/labels_IT.txt",
     0},
    {"HDD/FSCK/LANG/strings_DU.txt",
     "hdd0:__system:pfs:/fsck/lang/strings_DU.txt",
     0},
    {"HDD/FSCK/LANG/labels_DU.txt",
     "hdd0:__system:pfs:/fsck/lang/labels_DU.txt",
     0},
    {"HDD/FSCK/LANG/strings_PO.txt",
     "hdd0:__system:pfs:/fsck/lang/strings_PO.txt",
     0},
    {"HDD/FSCK/LANG/labels_PO.txt",
     "hdd0:__system:pfs:/fsck/lang/labels_PO.txt",
     0},
};

#define PS2_SYS_HDD_INSTALL_NUM_FILES 3
static struct InstallationFile PS2SysHDDFiles[PS2_SYS_HDD_INSTALL_NUM_FILES] = {
    {"SYSTEM/MBR.XLF",
     "hdd0:__mbr",
     FILE_IS_KELF},
    {"SYSTEM/FHDB.XLF",
     "hdd0:__system:pfs:/osd/osdmain.elf",
     FILE_IS_KELF},
    {"SYSTEM/ENDVDPL.XRX",
     "hdd0:__sysconf:pfs:/FMCB/endvdpl.irx",
     FILE_IS_KELF}};

#define DEX_SYS_HDD_INSTALL_NUM_FILES 2
static struct InstallationFile DEXSysHDDFiles[DEX_SYS_HDD_INSTALL_NUM_FILES] = {
    {"SYSTEM/MBR.XLF",
     "hdd0:__mbr",
     FILE_IS_KELF},
    {"SYSTEM/FHDB.XLF",
     "hdd0:__system:pfs:/osd/osdmain.elf",
     FILE_IS_KELF}};

/* Remember to update NUM_CROSSLINKED_FILES in main.h appropriately!
    If folders are to be added/deleted from the list of supported folders, remember to update these functions:
        CreateBasicFolders()
        CleanupTarget()
        CleanupMultiInstallation()
        PerformInstallation()

        Within PerformInstallation, add/remove code which adds entries for the icon resources into the file copy list, for the new folder.
        Remember to adjust the system folder count for the code (Which multiplies by SYS_FOLDER_RESOURCES_NUM_FILES) which determines how many times the resource files must be copied, as well!

        Note: The names of the folders (which the files go into) must be uniform. They must fit the format of SysExecFolder.
*/
static struct FileAlias FileAlias[NUM_CROSSLINKED_FILES] = {
    {0x8415, "BIEXEC-SYSTEM/osd130.elf"},
    {0x8415, "BIEXEC-SYSTEM/osdmain.elf"},
    {0x8415, "BEEXEC-SYSTEM/osd130.elf"},
    {0x8415, "BEEXEC-SYSTEM/osdmain.elf"},
    {0x8415, "BAEXEC-SYSTEM/osd120.elf"},
    {0x8415, "BAEXEC-SYSTEM/osd130.elf"},
    {0x8415, "BAEXEC-SYSTEM/osdmain.elf"},
    {0x8415, "BCEXEC-SYSTEM/osdmain.elf"}};

static char MGFolderRegion, PS2SystemType;
static unsigned short int ROMVersion;
static char SysExecFolder[] = "BREXEC-SYSTEM"; // Read above.
static char PSXSysExecFolder[] = "BIEXEC-SYSTEM";
static char SysExecFile[12]; /* E.g. "osdmain.elf" or "osd110.elf" */
static char romver[16];

static int InitMCInfo(int port, int slot)
{
    int result;
    int type, space, format;

    mcGetInfo(port, slot, &type, &space, &format);
    mcSync(0, NULL, &result);

    if (result >= sceMcResChangedCard && type == sceMcTypePS2) {
        result = 0;
    } else
        result = -ENODEV;

    return result;
}

static int SignKELF(void *buffer, int size, unsigned char port, unsigned char slot)
{
    int result, InitSemaID, mcInitRes;

    /*	An IOP reboot would be done by the Utility Disc,
        to allow the SecrDownloadFile function of secrman_special to work on a DEX,
        even though secrman_special was meant for a CEX.

        A DEX was designed so that card authentication will not work right when a CEX SECRMAN module is used.
        This works since the memory card was authenticated by the ROM's SECRMAN module and SecrDownloadFile does not involve card authentication.

        However, to speed things up and to prevent more things from going wrong (particularly with USB support), we just reboot the IOP once at initialization and load all modules there.
        Our SECRMAN module is a custom version that has a check to support the DEX natively.	*/

    result = 1;
    if (SecrDownloadFile(2 + port, slot, buffer) == NULL) {
        DEBUG_PRINTF("Error signing file.\n");
        result = -EINVAL;
    }

    return result;
}

static void GetKbitAndKc(void *buffer, u8 *Kbit, u8 *Kc)
{
    int offset;
    unsigned char OffsetByte;
    SecrKELFHeader_t *header;

    header = (SecrKELFHeader_t *)buffer;
    offset = 0x20;
    if (header->BIT_count > 0)
        offset += header->BIT_count * 0x10;
    if ((*(unsigned int *)&header->flags) & 1) {
        OffsetByte = ((u8 *)buffer)[offset];
        offset += OffsetByte + 1;
    }
    if (((*(unsigned int *)&header->flags) & 0xF000) == 0)
        offset += 8;

    memcpy(Kbit, &((u8 *)buffer)[offset], 16);
    memcpy(Kc, &((u8 *)buffer)[offset + 16], 16);
}

static int SetKbitAndKc(void *buffer, u8 *Kbit, u8 *Kc)
{
    int offset;
    unsigned char OffsetByte;
    SecrKELFHeader_t *header;

    header = (SecrKELFHeader_t *)buffer;
    offset = 0x20;
    if (header->BIT_count > 0)
        offset += header->BIT_count * 0x10;
    if ((*(unsigned int *)&header->flags) & 1) {
        OffsetByte = ((u8 *)buffer)[offset];
        offset += OffsetByte + 1;
    }
    if (((*(unsigned int *)&header->flags) & 0xF000) == 0)
        offset += 8;

    memcpy(&((u8 *)buffer)[offset], Kbit, 16);
    memcpy(&((u8 *)buffer)[offset + 16], Kc, 16);
}

static int TwinSignKELF(const char *RootFolder, void *buffer, int size, unsigned char port, unsigned char slot)
{
    int result, i, iFileSize;
    struct InstallationFile *PS2ExecFile;
    FILE *iFile;
    void *iFileBuffer;
    char *iFilePath;
    u8 Kbit[16], Kc[16];

    // Try to locate a system PS2 KELF
    for (i = 0, PS2ExecFile = NULL; i < PS2_SYS_INSTALL_NUM_FILES; i++) {
        if (PS2SysFiles[i].flags & FILE_IS_KELF) {
            PS2ExecFile = &PS2SysFiles[i];
            break;
        }
    }
    if (PS2ExecFile == NULL) // Can't happen, but what if?
        return -1;

    if ((iFilePath = malloc(strlen(RootFolder) + strlen(PS2ExecFile->SrcRelPath) + 2)) != NULL) {
        sprintf(iFilePath, "%s/%s", RootFolder, PS2ExecFile->SrcRelPath);
        if ((iFile = fopen(iFilePath, "rb")) != NULL) {
            fseek(iFile, 0, SEEK_END);
            iFileSize = ftell(iFile);
            rewind(iFile);
            if ((iFileBuffer = memalign(64, iFileSize)) != NULL) {
                if (fread(iFileBuffer, 1, iFileSize, iFile) == iFileSize) {
                    if ((result = SignKELF(iFileBuffer, iFileSize, port, slot)) >= 0) {
                        GetKbitAndKc(iFileBuffer, Kbit, Kc);
                        SetKbitAndKc(buffer, Kbit, Kc);
                        result = 0;
                    }
                } else
                    result = -EIO;

                free(iFileBuffer);
            } else
                result = -ENOMEM;

            fclose(iFile);
        } else
            result = -errno;

        free(iFilePath);
    } else
        result = -ENOMEM;

    return result;
}

static char GetMGFolderLetter(unsigned char region)
{
    unsigned char FolderLetter;

    switch (region) {
        case 'C':
            FolderLetter = 'C';
            break;
        case 'J':
            FolderLetter = 'I';
            break;
        case 'H':
        case 'A':
            FolderLetter = 'A';
            break;
        case 'E':
            FolderLetter = 'E';
            break;
        default:
            DEBUG_PRINTF("Unrecognized region code: %c.\n", region);
            FolderLetter = 'R';
    }

    return FolderLetter;
}

void UpdateRegionalPaths(void)
{
    char ROMVersionNumStr[5], RegionCode;
    int size;
    FILE *file;

    /* Acquire the MG region folder letter of this console. */
    file = fopen("rom0:ROMVER", "r");
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);
    fread(romver, 1, size > sizeof(romver) ? sizeof(romver) : size, file);
    fclose(file);

    strncpy(ROMVersionNumStr, romver, 4);
    RegionCode = romver[4];

    /* NULL terminate the version number. */
    ROMVersionNumStr[4] = '\0';

    ROMVersion = strtoul(ROMVersionNumStr, NULL, 16);

    if (romver[5] == 'D') {
        PS2SystemType = PS2_SYSTEM_TYPE_DEX;
    } else {
        if ((file = fopen("rom0:PSXVER", "r")) != NULL) {
            fclose(file);
            PS2SystemType = PS2_SYSTEM_TYPE_PSX;
        } else {
            PS2SystemType = PS2_SYSTEM_TYPE_PS2;
        }
    }

    if (ROMVersion < 0x130) {
        /*	Consoles sporting a boot ROM older than v1.30 will look for OSDSYS updates named osdXXX.elf.
         *	The value of XXX seems to be the nearest boot ROM version number rounded UP to the nearest 10 (E.g. a console with ROM v1.01 will look for osd110.elf).
         *	Since the version numbers are handled in this function in HEX, it will be rounded up to the nearest 16 instead
         *	Since kernels v1.01 and v1.00 require specialized patching, the update file they should use would be osd130.elf.
         */
        if (ROMVersion == 0x100 || ROMVersion == 0x101) {
            strcpy(SysExecFile, "osd130.elf");
        } else {
            sprintf(SysExecFile, "osd%03x.elf", (ROMVersion + 0x10) & ~0x0F);
        }
    } else {
        strcpy(SysExecFile, "osdmain.elf");
    }

    DEBUG_PRINTF("ROMVER: %03x SysExec: %s\n", ROMVersion, SysExecFile);

    MGFolderRegion = GetMGFolderLetter(RegionCode);

    SysExecFolder[1] = MGFolderRegion;
}

int GetPs2Type(void)
{
    return PS2SystemType;
}

static const char *GetMountParams(const char *command, char *BlockDevice)
{
    const char *MountPath;
    int BlockDeviceNameLen;

    if ((MountPath = strchr(&command[5], ':')) != NULL) {
        BlockDeviceNameLen = (unsigned int)MountPath - (unsigned int)command;
        strncpy(BlockDevice, command, BlockDeviceNameLen);
        BlockDevice[BlockDeviceNameLen] = '\0';

        MountPath++; // This is the location of the mount path;
    }

    return MountPath;
}

static int createFolder(int port, int slot, const char *path)
{
    int result;
    sceMcTblGetDir table;

    if ((result = mcMkDir(port, slot, path)) == 0) {
        mcSync(0, NULL, &result);
        if (result == -4)
            result = 0; // EEXIST doesn't count as an error.
    }

    if (result == 0) {
        // Set desired file attributes.
        table.AttrFile = sceMcFileAttrReadable | sceMcFileAttrWriteable | sceMcFileAttrExecutable | sceMcFileAttrDupProhibit | sceMcFileAttrSubdir | sceMcFile0400;
        if ((result = mcSetFileInfo(port, slot, path, &table, sceMcFileInfoAttr)) == 0) {
            mcSync(0, NULL, &result);
        }
    }

    return result;
}

static int CreateBasicFolders(int port, int slot, unsigned int flags)
{
    unsigned int i;
    int result;
    /* No "APPS" folder: the contents of INSTALL/APPS are installed to the root of
       the memory card (e.g. INSTALL/APPS/app_dir1 -> mc0:/app_dir1). */
    char folders[][16] = {
        "BOOT",
        "SYS-CONF",
        "\0"};

    for (i = 0, result = 0; folders[i][0] != '\0' && result >= 0; i++) {
        if ((result = mcMkDir(port, slot, folders[i])) == 0) {
            mcSync(0, NULL, &result);
            if (result == -4)
                result = 0; // EEXIST doesn't count as an error.
        }
    }

    if (result >= 0) {
        if (!(flags & INSTALL_MODE_FLAG_MULTI_INST) && !(flags & INSTALL_MODE_FLAG_CROSS_REG)) {
            result = createFolder(port, slot, (flags & INSTALL_MODE_FLAG_CROSS_PSX) ? PSXSysExecFolder : SysExecFolder);
        } else {
            result = createFolder(port, slot, "BIEXEC-SYSTEM");

            if (result >= 0) {
                result = createFolder(port, slot, "BEEXEC-SYSTEM");
            }

            if (result >= 0) {
                result = createFolder(port, slot, "BAEXEC-SYSTEM");
            }

            if (result >= 0) {
                result = createFolder(port, slot, "BCEXEC-SYSTEM");
            }
        }
    }

    return result;
}

struct DirentToDelete
{
    struct DirentToDelete *next;
    char *filename;
};

static int DeleteFolder(const char *folder)
{
    int fd, result;
    char *path;
    iox_dirent_t dirent;
    struct DirentToDelete *head, *start;

    result = 0;
    start = head = NULL;
    if ((fd = fileXioDopen(folder)) >= 0) {
        /* Generate a list of files in the directory. */
        while (fileXioDread(fd, &dirent) > 0) {
            if ((strcmp(dirent.name, ".") == 0) || ((strcmp(dirent.name, "..") == 0)))
                continue;

            if (FIO_S_ISDIR(dirent.stat.mode)) {
                if ((path = malloc(strlen(folder) + strlen(dirent.name) + 2)) != NULL) {
                    sprintf(path, "%s/%s", folder, dirent.name);
                    result = DeleteFolder(path);
                    free(path);
                }
            } else {
                if (start == NULL) {
                    if ((start = head = malloc(sizeof(struct DirentToDelete))) == NULL) {
                        break;
                    }
                } else {
                    if ((head->next = malloc(sizeof(struct DirentToDelete))) == NULL) {
                        break;
                    }

                    head = head->next;
                }

                head->next = NULL;

                if ((head->filename = malloc(strlen(dirent.name) + 1)) != NULL) {
                    strcpy(head->filename, dirent.name);
                } else
                    break;
            }
        }

        fileXioDclose(fd);
    } else
        result = fd;

    if (result >= 0) {
        /* Delete the files. */
        for (head = start; head != NULL; head = start) {
            if (head->filename != NULL) {
                if ((path = malloc(strlen(folder) + strlen(head->filename) + 2)) != NULL) {
                    sprintf(path, "%s/%s", folder, head->filename);
                    DEBUG_PRINTF("Deleting %s...", path);
                    result = fileXioRemove(path);
                    DEBUG_PRINTF("%d\n", result);

                    free(path);
                }
                free(head->filename);
            }

            start = head->next;
            free(head);
        }

        if (result >= 0) {
            DEBUG_PRINTF("Deleting folder %s...", folder);
            result = fileXioRmdir(folder);
            DEBUG_PRINTF("%d\n", result);
        }
    }

    return result;
}

static int DeleteFolderIfEmpty(const char *folder)
{
    iox_dirent_t dirent;
    unsigned int NumFilesInFolder;
    int fd, result;

    result = 0;
    NumFilesInFolder = 0;
    if ((fd = fileXioDopen(folder)) >= 0) {
        while (fileXioDread(fd, &dirent) > 0) {
            if ((strcmp(dirent.name, ".") == 0) || ((strcmp(dirent.name, "..") == 0)))
                continue;

            NumFilesInFolder++;
        }

        fileXioDclose(fd);
    } else
        result = fd;

    if (result >= 0 && NumFilesInFolder == 0) {
        result = DeleteFolder(folder);
    }

    return result;
}

int CleanupTarget(int port, int slot)
{
    char PathToFolder[66];

    sprintf(PathToFolder, "mc%u:BIEXEC-SYSTEM", port);
    DeleteFolder(PathToFolder);
    sprintf(PathToFolder, "mc%u:BAEXEC-SYSTEM", port);
    DeleteFolder(PathToFolder);
    sprintf(PathToFolder, "mc%u:BEEXEC-SYSTEM", port);
    DeleteFolder(PathToFolder);
    sprintf(PathToFolder, "mc%u:BCEXEC-SYSTEM", port);
    DeleteFolder(PathToFolder);

    return 0;
}

static int AddDirContentsToFileCopyList(const char *RootFolderPath, const char *srcRelativePath, const char *destination, unsigned int CurrentLevel, struct FileCopyTarget **FileCopyList, unsigned int *CurrentNumFiles, unsigned int *CurrentNumDirs, unsigned int *TotalRequiredSpaceForFiles)
{
    char *path;
    void *TempFileCopyListPtr;
    int result, fd;
    iox_dirent_t dirent;
    struct FileCopyTarget *NewFileCopyTarget;
    unsigned int CurrentNumFileEnts;
#ifdef DEBUG_TTY_FEEDBACK
    unsigned int i;
    u64 FileSize;
#endif

    result = 0;
    if (srcRelativePath != NULL) {
        path = malloc(strlen(RootFolderPath) + strlen(srcRelativePath) + 2);
        sprintf(path, "%s/%s", RootFolderPath, srcRelativePath);
    } else {
        path = malloc(strlen(RootFolderPath) + 2);
        sprintf(path, "%s/", RootFolderPath);
    }

    DEBUG_PRINTF("Path: %s\n", path);
    if ((fd = fileXioDopen(path)) >= 0) {
        while (fileXioDread(fd, &dirent) > 0) {
#ifdef DEBUG_TTY_FEEDBACK
            for (i = 0; i < CurrentLevel; i++)
                sio_printf("\t");
            FileSize = (u64)dirent.stat.hisize << 32 | dirent.stat.size;
            sio_printf("%c%c%c%c%c%c%c%c%c%c %lu %s\n", FIO_S_ISDIR(dirent.stat.mode) ? 'd' : '-', dirent.stat.mode & FIO_S_IRUSR ? 'r' : '-', dirent.stat.mode & FIO_S_IWUSR ? 'w' : '-', dirent.stat.mode & FIO_S_IXUSR ? 'x' : '-', dirent.stat.mode & FIO_S_IRGRP ? 'r' : '-', dirent.stat.mode & FIO_S_IWGRP ? 'w' : '-', dirent.stat.mode & FIO_S_IXGRP ? 'x' : '-', dirent.stat.mode & FIO_S_IROTH ? 'r' : '-', dirent.stat.mode & FIO_S_IWOTH ? 'w' : '-', dirent.stat.mode & FIO_S_IXOTH ? 'x' : '-', FileSize, dirent.name);
#endif

            if (strcmp(dirent.name, ".") == 0 || strcmp(dirent.name, "..") == 0)
                continue;

            if (FIO_S_ISDIR(dirent.stat.mode)) {
                (*CurrentNumDirs)++;
            } else {
                (*TotalRequiredSpaceForFiles) += dirent.stat.size;
                (*CurrentNumFiles)++;
            }

            CurrentNumFileEnts = *CurrentNumFiles + *CurrentNumDirs;
            if ((TempFileCopyListPtr = realloc(*FileCopyList, sizeof(struct FileCopyTarget) * CurrentNumFileEnts)) != NULL) {
                *FileCopyList = TempFileCopyListPtr;
                NewFileCopyTarget = &(*FileCopyList)[CurrentNumFileEnts - 1];

                memset(NewFileCopyTarget, 0, sizeof(struct FileCopyTarget));
                if ((NewFileCopyTarget->source = malloc((srcRelativePath != NULL ? strlen(srcRelativePath) + 2 : 1) + strlen(dirent.name))) != NULL) {
                    if (srcRelativePath != NULL) {
                        sprintf(NewFileCopyTarget->source, "%s/%s", srcRelativePath, dirent.name);
                    } else {
                        strcpy(NewFileCopyTarget->source, dirent.name);
                    }
                    if ((NewFileCopyTarget->target = malloc(strlen(destination) + strlen(dirent.name) + 2)) != NULL) {
                        /* An empty destination means "install into the root of the
                           target device", so don't emit a leading '/' in that case
                           (memory card paths are relative to the card's root). */
                        if (destination[0] != '\0')
                            sprintf(NewFileCopyTarget->target, "%s/%s", destination, dirent.name);
                        else
                            strcpy(NewFileCopyTarget->target, dirent.name);

                        NewFileCopyTarget->mode = dirent.stat.mode;
                        NewFileCopyTarget->flags = 0;

                        if (FIO_S_ISDIR(dirent.stat.mode)) {
                            NewFileCopyTarget->size = 0;
                            result = AddDirContentsToFileCopyList(RootFolderPath, NewFileCopyTarget->source, NewFileCopyTarget->target, CurrentLevel + 1, FileCopyList, CurrentNumFiles, CurrentNumDirs, TotalRequiredSpaceForFiles);
                            if (result < 0)
                                break;
                        } else {
                            NewFileCopyTarget->size = dirent.stat.size;
                        }
                    } else {
                        result = -ENOMEM;
                        break;
                    }
                } else {
                    result = -ENOMEM;
                    break;
                }
            } else {
                DEBUG_PRINTF("Can't alloc for file copy list. Num dirents: %u\n", CurrentNumFileEnts);
                result = -ENOMEM;
                break;
            }
        }

        fileXioDclose(fd);
    } else
        result = fd == -ENOENT ? 0 : fd;

    free(path);

    return result;
}

static int GetMcFreeSpace(int port, int slot)
{
    int result;
    int type, space, format;

    mcGetInfo(port, slot, &type, &space, &format);
    mcSync(0, NULL, &result);

    if (result < -1 || type != MC_TYPE_PS2) {
        space = 0;
    }

    return space;
}

static int EnableHDDBooting(void)
{
    int OpResult, result;
    unsigned char OSDConfigBuffer[15];

    do {
        sceCdOpenConfig(0, 0, 1, &OpResult);
    } while (OpResult & 9);

    do {
        result = sceCdReadConfig(OSDConfigBuffer, &OpResult);
    } while (OpResult & 9 || result == 0);

    do {
        result = sceCdCloseConfig(&OpResult);
    } while (OpResult & 9 || result == 0);

    if ((OSDConfigBuffer[0] & 3) != 2) { // If ATAD support and HDD booting are not already activated.
        OSDConfigBuffer[0] = (OSDConfigBuffer[0] & ~3) | 2;

        do {
            sceCdOpenConfig(0, 1, 1, &OpResult);
        } while (OpResult & 9);

        do {
            result = sceCdWriteConfig(OSDConfigBuffer, &OpResult);
        } while (OpResult & 9 || result == 0);

        do {
            result = sceCdCloseConfig(&OpResult);
        } while (OpResult & 9 || result == 0);

        result = 0;
    } else
        result = 1;

    return result;
}

int IsHDDBootingEnabled(void)
{
    int OpResult, result;
    unsigned char OSDConfigBuffer[15];

    do {
        sceCdOpenConfig(0, 0, 1, &OpResult);
    } while (OpResult & 9);

    do {
        result = sceCdReadConfig(OSDConfigBuffer, &OpResult);
    } while (OpResult & 9 || result == 0);

    do {
        result = sceCdCloseConfig(&OpResult);
    } while (OpResult & 9 || result == 0);

    return ((OSDConfigBuffer[0] & 3) == 2);
}

/* Don't set this to be too large, as FILEXIO's RPC receive buffer is only about 0x4C00 bytes large */
#define MBR_WRITE_BLOCK_SIZE 2

static inline int InstallMBRToHDD(FILE *file, void *IOBuffer, unsigned int size)
{
    hddSetOsdMBR_t OSDData;
    unsigned short int NumSectorsToWrite, MBR_NumSectors, i;
    iox_stat_t stat;
    int result;
    unsigned int MBR_Sector, remaining, tLengthBytes;

    InstallLog("  MBR: getstat hdd0:__mbr ...\n");
    if ((result = fileXioGetStat("hdd0:__mbr", &stat)) >= 0) {
        MBR_Sector = stat.private_5 + 0x2000;
        MBR_NumSectors = ((size + 0x1FF) & ~0x1FF) / 512;
        InstallLog("  MBR: ok, start LBA=%u sectors=%u (size=%u)\n", MBR_Sector, MBR_NumSectors, size);

        for (i = 0, remaining = size; i < MBR_NumSectors && result >= 0; i += NumSectorsToWrite, remaining -= tLengthBytes) {
            NumSectorsToWrite = ((MBR_NumSectors - i) > MBR_WRITE_BLOCK_SIZE) ? MBR_WRITE_BLOCK_SIZE : MBR_NumSectors - i;
            tLengthBytes = remaining > (MBR_WRITE_BLOCK_SIZE * 512) ? (MBR_WRITE_BLOCK_SIZE * 512) : remaining;
            ((hddAtaTransfer_t *)IOBuffer)->lba = MBR_Sector + i;
            ((hddAtaTransfer_t *)IOBuffer)->size = NumSectorsToWrite;
            if (fread(((hddAtaTransfer_t *)IOBuffer)->data, 1, tLengthBytes, file) == tLengthBytes) {
                if (MBR_WRITE_BLOCK_SIZE * 512 - tLengthBytes > 0)
                    memset(((hddAtaTransfer_t *)IOBuffer)->data + tLengthBytes, 0, MBR_WRITE_BLOCK_SIZE * 512 - tLengthBytes);
                if (i == 0)
                    InstallLog("  MBR: first ATA write, lba=%u ...\n", MBR_Sector);
                result = fileXioDevctl("hdd0:", APA_DEVCTL_ATA_WRITE, IOBuffer, MBR_WRITE_BLOCK_SIZE * 512 + sizeof(hddAtaTransfer_t), NULL, 0);
                if (i == 0)
                    InstallLog("  MBR: first ATA write returned %d\n", result);
            } else
                result = -EIO;
        }

        if (result >= 0) {
            OSDData.start = MBR_Sector;
            OSDData.size = MBR_NumSectors;
            InstallLog("  MBR: all writes done, setting OSDMBR ...\n");
            fileXioDevctl("hdd0:", APA_DEVCTL_SET_OSDMBR, &OSDData, sizeof(OSDData), NULL, 0);
            InstallLog("  MBR: OSDMBR set\n");
        }
    } else
        InstallLog("  MBR: getstat FAILED (%d)\n", result);

    return result;
}

static int CopyFilesToHDD(const char *RootFolder, const struct FileCopyTarget *FileCopyList, unsigned int NumFilesEntries, unsigned int TotalNumBytes, unsigned int flags)
{
    unsigned int i, BytesCopied, remaining, CopyLength;
    int result, size;
    FILE *file, *DestFile;
    char *path, CurrentlyMountedBlockDeviceName[40], BlockDeviceToMount[40];
    const char *MountPath;
    void *buffer;

    InitProgressScreen(SYS_UI_LBL_INSTALLING);

    if ((buffer = memalign(64, IO_BLOCK_SIZE)) != NULL) {
        result = 0;
        CurrentlyMountedBlockDeviceName[0] = '\0';
        for (i = 0, BytesCopied = 0; (i < NumFilesEntries) && (result >= 0); i++) {
            DrawFileCopyProgressScreen(TotalNumBytes > 0 ? (float)((double)BytesCopied / TotalNumBytes) : 0.0f);

            if (FileCopyList[i].flags & FILE_SKIP) { // Not present in the payload.
                InstallLog("[%u/%u] SKIP (not in payload) %s\n", i + 1, NumFilesEntries, FileCopyList[i].source ? FileCopyList[i].source : "?");
                continue;
            }

            InstallLog("[%u/%u] %s -> %s (%u bytes)\n", i + 1, NumFilesEntries,
                       FileCopyList[i].source ? FileCopyList[i].source : "?",
                       FileCopyList[i].target ? FileCopyList[i].target : "?",
                       FileCopyList[i].size);

            if (FIO_S_ISDIR(FileCopyList[i].mode)) {
                DEBUG_PRINTF("mkdir: %s\n", FileCopyList[i].target);

                // Check mount command.
                if ((MountPath = GetMountParams(FileCopyList[i].target, BlockDeviceToMount)) != NULL) {
                    if (strcmp(BlockDeviceToMount, CurrentlyMountedBlockDeviceName)) {
                        if (CurrentlyMountedBlockDeviceName[0] != '\0')
                            fileXioUmount("pfs0:");

                        if ((result = fileXioMount("pfs0:", BlockDeviceToMount, FIO_MT_RDWR)) < 0) {
                            DEBUG_PRINTF("Failed to mount partition: %s, %d.\n", BlockDeviceToMount, result);
                            result = ERROR_SIDE_DST | -EIO;
                            break;
                        }

                        strcpy(CurrentlyMountedBlockDeviceName, BlockDeviceToMount);
                    }

                    if ((result = fileXioMkdir(MountPath, 0777)) < 0) {
                        if (result != -EEXIST)
                            result |= ERROR_SIDE_DST;
                        else
                            result = 0;
                    }
                }
            } else {
                path = malloc(strlen(RootFolder) + strlen(FileCopyList[i].source) + 2);
                sprintf(path, "%s/%s", RootFolder, FileCopyList[i].source);

                DEBUG_PRINTF("Copying %s -> %s...\n", FileCopyList[i].source, FileCopyList[i].target);

                if ((file = fopen(path, "rb")) == NULL) {
                    DEBUG_PRINTF("Error: Can't open file: %s, %d\n", path, -errno);

                    free(path);

                    result = (-errno) | ERROR_SIDE_SRC;
                    break;
                }

                free(path);

                /* Copy the file */
                size = FileCopyList[i].size;
                if (!strcmp(FileCopyList[i].target, "hdd0:__mbr")) {
                    if ((result = InstallMBRToHDD(file, buffer, size)) < 0)
                        result |= ERROR_SIDE_DST;
                    else
                        BytesCopied += size;

                    /* NOTE: deliberately no fclose() here -- the unconditional
                       fclose(file) below closes it. Closing it twice used to
                       freeze the HDD install: on the modern ps2sdk newlib backs
                       each FILE's lock with a real EE semaphore, so the first
                       close deletes that semaphore and frees the lock, and the
                       second close then locks freed memory and does a WaitSema()
                       on a stale id. When that id happened to name a live,
                       already-taken semaphore (InstallLockSema is held for the
                       whole install) the thread blocked forever. The MBR is the
                       first entry in the copy list, hence "frozen at 0%". */
                } else {
                    // Check mount command.
                    if ((MountPath = GetMountParams(FileCopyList[i].target, BlockDeviceToMount)) != NULL) {
                        if (strcmp(BlockDeviceToMount, CurrentlyMountedBlockDeviceName)) {
                            if (CurrentlyMountedBlockDeviceName[0] != '\0')
                                fileXioUmount("pfs0:");

                            if ((result = fileXioMount("pfs0:", BlockDeviceToMount, FIO_MT_RDWR)) < 0) {
                                DEBUG_PRINTF("Failed to mount partition: %s, %d.\n", BlockDeviceToMount, result);
                                result = ERROR_SIDE_DST | -EIO;
                            }

                            strcpy(CurrentlyMountedBlockDeviceName, BlockDeviceToMount);
                        }

                        if (result >= 0) {
                            if ((DestFile = fopen(MountPath, "wb")) != NULL) {
                                for (remaining = size; remaining > 0; BytesCopied += CopyLength, remaining -= CopyLength) {
                                    DrawFileCopyProgressScreen(TotalNumBytes > 0 ? (float)((double)BytesCopied / TotalNumBytes) : 0.0f);
                                    CopyLength = remaining > IO_BLOCK_SIZE ? IO_BLOCK_SIZE : remaining;

                                    if (fread(buffer, 1, CopyLength, file) == CopyLength) {
                                        if (fwrite(buffer, 1, CopyLength, DestFile) != CopyLength) {
                                            result = ERROR_SIDE_DST | -EIO;
                                            break;
                                        }
                                    } else {
                                        result = ERROR_SIDE_SRC | -EIO;
                                        break;
                                    }
                                }

                                fclose(DestFile);
                            } else
                                result = -errno;
                        }
                    } else
                        result = -1;
                }

                fclose(file);
            }
        }

        fileXioUmount("pfs0:");
        free(buffer);
    } else
        result = -ENOMEM;

    return result;
}

/* Create a folder on the currently mounted pfs0:, treating "already exists" as
   success. Returns 0 on success, otherwise the error (logged by the caller). */
static int MakeDirOnMountedPfs(const char *path)
{
    int result;

    if ((result = fileXioMkdir(path, 0777)) == -EEXIST)
        result = 0;
    if (result < 0)
        DEBUG_PRINTF("mkdir %s failed: %d\n", path, result);

    return result;
}

static int CreateBasicFoldersOnHDD(unsigned int flags)
{
    int result;

    /*	Folders to create:
            hdd0:__system/osd
            hdd0:__system/fsck
            hdd0:__system/fsck/lang
            hdd0:__sysconf/FMCB
            hdd0:__sysconf/PS2BBL
            hdd0:__common/APPS

        This is preparation, not the installation itself, so it is best-effort:
        a partition that cannot be mounted (or a folder that cannot be created)
        is logged and skipped rather than aborting the whole install. If such a
        folder is genuinely needed, the file copy reports a precise error for it. */

    if ((result = fileXioMount("pfs0:", "hdd0:__system", FIO_MT_RDWR)) >= 0) {
        MakeDirOnMountedPfs("pfs0:/osd");
        MakeDirOnMountedPfs("pfs0:/fsck");
        MakeDirOnMountedPfs("pfs0:/fsck/lang");

        fileXioUmount("pfs0:");
    } else
        DEBUG_PRINTF("CreateBasicFoldersOnHDD: can't mount __system: %d\n", result);

    if ((result = fileXioMount("pfs0:", "hdd0:__sysconf", FIO_MT_RDWR)) >= 0) {
        MakeDirOnMountedPfs("pfs0:/FMCB");
        MakeDirOnMountedPfs("pfs0:/PS2BBL");

        fileXioUmount("pfs0:");
    } else
        DEBUG_PRINTF("CreateBasicFoldersOnHDD: can't mount __sysconf: %d\n", result);

    if ((result = fileXioMount("pfs0:", "hdd0:__common", FIO_MT_RDWR)) >= 0) {
        MakeDirOnMountedPfs("pfs0:/APPS");

        fileXioUmount("pfs0:");
    } else
        DEBUG_PRINTF("CreateBasicFoldersOnHDD: can't mount __common: %d\n", result);

    return 0;
}

static int _CleanupHDDTarget(void)
{
    int result;

    /* Basically, do the opposite of CreateBasicFoldersOnHDD(). */
    if ((result = fileXioMount("pfs0:", "hdd0:__system", FIO_MT_RDWR)) >= 0) {
        fileXioRemove("pfs0:/osd/osdmain.elf");
        DeleteFolderIfEmpty("pfs0:/osd");
        DeleteFolder("pfs0:/fsck/lang");
        fileXioRemove("pfs0:/fsck/fsck.elf");
        DeleteFolderIfEmpty("pfs0:/fsck");

        fileXioUmount("pfs0:");
    }
    if (result >= 0) {
        if ((result = fileXioMount("pfs0:", "hdd0:__sysconf", FIO_MT_RDWR)) >= 0) {
            fileXioRemove("pfs0:/FMCB/FMCB_CFG.ELF");
            fileXioRemove("pfs0:/FMCB/USBD.IRX");
            fileXioRemove("pfs0:/FMCB/USBHDFSD.IRX");
            DeleteFolderIfEmpty("pfs0:/FMCB");

            fileXioUmount("pfs0:");
        }
    }

    return result;
}

int CleanupHDDTarget(void)
{
    int result;

    WaitSema(InstallLockSema);

    result = _CleanupHDDTarget();

    SignalSema(InstallLockSema);

    return result;
}

static int GetAllBaseFileStats(char MGLetter, const char *RootFolder, struct FileCopyTarget *FileCopyList, unsigned int NumFiles, unsigned int *TotalRequiredSpaceForFiles)
{
    char *path;
    int result;
    unsigned int i;
    iox_stat_t stat;

    *TotalRequiredSpaceForFiles = 0;

    // Start getting the sizes and attributes of the files.
    for (i = 0, result = 0; i < NumFiles && result >= 0; i++) {
        path = malloc(strlen(RootFolder) + strlen(FileCopyList[i].source) + 2);
        sprintf(path, "%s/%s", RootFolder, FileCopyList[i].source);

        // Update regional paths.
        if (strncmp(FileCopyList[i].target, "BREXEC-SYSTEM", 13) == 0) {
            FileCopyList[i].target[1] = MGLetter;
        }

        if ((result = fileXioGetStat(path, &stat)) >= 0) {
            FileCopyList[i].mode = stat.mode;
            FileCopyList[i].size = stat.size;
            (*TotalRequiredSpaceForFiles) += stat.size;
        } else {
            /* The file is not in the payload. Don't abort the whole installation
               over it -- flag it so the copy stage skips it. This lets the INSTALL
               folder be customised without having to match the built-in list. */
            DEBUG_PRINTF("Not in payload, skipping: %s\n", path);
            FileCopyList[i].flags |= FILE_SKIP;
            FileCopyList[i].mode = 0;
            FileCopyList[i].size = 0;
            result = 0;
        }

        free(path);
    }

    return result;
}

int PerformHDDInstallation(unsigned int flags)
{
    struct FileCopyTarget *FileCopyList;
    unsigned int NumFiles, NumDirectories, i, file, TotalRequiredSpaceForFiles, CurrNumFiles, CurrNumFolders;
    int result;
    unsigned int AvailableSpace, TotalRequiredSpace;
    u32 FreeSectors;
    char RootFolder[256];

    GetInstallRootPath(RootFolder, sizeof(RootFolder));
#ifdef DIAG_INSTALL_PATH
    ShowInstallPathDiag(RootFolder);
#endif

    InstallLog("\n=== FreeHdBoot (HDD) install, payload: %s ===\n", RootFolder);

    // Generate the file copy list.
    NumFiles = HDD_BASE_INSTALL_NUM_FILES;
    switch (PS2SystemType) {
        case PS2_SYSTEM_TYPE_DEX:
            NumFiles += DEX_SYS_HDD_INSTALL_NUM_FILES;
            break;
        default: // Default to CEX
            NumFiles += PS2_SYS_HDD_INSTALL_NUM_FILES;
    }
    NumDirectories = 0;
    TotalRequiredSpaceForFiles = 0;

    /* No NumFiles-- for SKIP_CNF: FREEHDB.CNF now comes from the generic SYS-CONF
       folder copy and is flagged FILE_SKIP there instead. */

    WaitSema(InstallLockSema);

    file = 0;
    if ((FileCopyList = malloc(NumFiles * sizeof(struct FileCopyTarget))) != NULL) {
        memset(FileCopyList, 0, NumFiles * sizeof(struct FileCopyTarget));

        if (PS2SystemType == PS2_SYSTEM_TYPE_DEX) {
            for (i = 0; i < DEX_SYS_HDD_INSTALL_NUM_FILES; i++) {
                FileCopyList[file].source = malloc(strlen(DEXSysHDDFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, DEXSysHDDFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(DEXSysHDDFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, DEXSysHDDFiles[i].DestRelPath);

                FileCopyList[file].flags = DEXSysHDDFiles[i].flags;
                file++;
            }
        } else { // Default to CEX (Consumer) PS2.
            for (i = 0; i < PS2_SYS_HDD_INSTALL_NUM_FILES; i++) {
                FileCopyList[file].source = malloc(strlen(PS2SysHDDFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, PS2SysHDDFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(PS2SysHDDFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, PS2SysHDDFiles[i].DestRelPath);

                FileCopyList[file].flags = PS2SysHDDFiles[i].flags;
                file++;
            }
        }

        for (i = 0; i < HDD_BASE_INSTALL_NUM_FILES; i++) {
            FileCopyList[file].source = malloc(strlen(HDDBaseFiles[i].SrcRelPath) + 1);
            strcpy(FileCopyList[file].source, HDDBaseFiles[i].SrcRelPath);

            FileCopyList[file].target = malloc(strlen(HDDBaseFiles[i].DestRelPath) + 1);
            strcpy(FileCopyList[file].target, HDDBaseFiles[i].DestRelPath);

            FileCopyList[file].flags = HDDBaseFiles[i].flags;
            file++;
        }

        // Start getting the sizes and attributes of the files.
        result = GetAllBaseFileStats(MGFolderRegion, RootFolder, FileCopyList, NumFiles, &TotalRequiredSpaceForFiles);

        /* NOTE: the BOOT-HDD and APPS-HDD folders are deliberately NOT installed
           any more, and no dedicated "PP.FHDB.APPS" partition is created. The APPS
           folder now goes onto the standard __common partition instead. */

        /* Everything the HDD install copies lives under INSTALL/HDD, with one
           folder per destination. All of them are optional: a folder that is not
           in the payload is simply skipped.
               HDD/APPS/...           -> hdd0:__common:/APPS/...
               HDD/CFG_FHDB/...       -> hdd0:__sysconf:/FMCB/...
               HDD/CFG_PS2BBL/...     -> hdd0:__sysconf:/PS2BBL/...
               HDD/HDD_OSD/sysconf/.. -> hdd0:__sysconf:/...     (partition root)
               HDD/HDD_OSD/system/... -> hdd0:__system:/osd/...  (next to osdmain.elf)
           (HDD/FSCK is handled by the hardcoded list above, because FSCK.XLF has
           to be signed as a KELF and renamed to fsck.elf.) */
        if (result >= 0) {
            if ((result = AddDirContentsToFileCopyList(RootFolder, "HDD/APPS", "hdd0:__common:pfs:/APPS", 1, &FileCopyList, &NumFiles, &NumDirectories, &TotalRequiredSpaceForFiles)) < 0) {
                DEBUG_PRINTF("AddDirContentsToFileCopyList (HDD/APPS -> __common) failed: %d\n", result);
            }
        }

        if (result >= 0) {
            unsigned int CfgStart = NumFiles + NumDirectories;

            if ((result = AddDirContentsToFileCopyList(RootFolder, "HDD/CFG_FHDB", "hdd0:__sysconf:pfs:/FMCB", 1, &FileCopyList, &NumFiles, &NumDirectories, &TotalRequiredSpaceForFiles)) < 0) {
                DEBUG_PRINTF("AddDirContentsToFileCopyList (HDD/CFG_FHDB) failed: %d\n", result);
            } else if (flags & INSTALL_MODE_FLAG_SKIP_CNF) {
                // Keep the user's existing configuration file, if requested.
                for (i = CfgStart; i < NumFiles + NumDirectories; i++) {
                    if (FileCopyList[i].target != NULL && strstr(FileCopyList[i].target, "FREEHDB.CNF") != NULL)
                        FileCopyList[i].flags |= FILE_SKIP;
                }
            }
        }

        if (result >= 0) {
            if ((result = AddDirContentsToFileCopyList(RootFolder, "HDD/CFG_PS2BBL", "hdd0:__sysconf:pfs:/PS2BBL", 1, &FileCopyList, &NumFiles, &NumDirectories, &TotalRequiredSpaceForFiles)) < 0) {
                DEBUG_PRINTF("AddDirContentsToFileCopyList (HDD/CFG_PS2BBL) failed: %d\n", result);
            }
        }

        if (result >= 0) {
            if ((result = AddDirContentsToFileCopyList(RootFolder, "HDD/HDD_OSD/sysconf", "hdd0:__sysconf:pfs:", 1, &FileCopyList, &NumFiles, &NumDirectories, &TotalRequiredSpaceForFiles)) < 0) {
                DEBUG_PRINTF("AddDirContentsToFileCopyList (HDD/HDD_OSD/sysconf) failed: %d\n", result);
            }
        }

        if (result >= 0) {
            if ((result = AddDirContentsToFileCopyList(RootFolder, "HDD/HDD_OSD/system", "hdd0:__system:pfs:/osd", 1, &FileCopyList, &NumFiles, &NumDirectories, &TotalRequiredSpaceForFiles)) < 0) {
                DEBUG_PRINTF("AddDirContentsToFileCopyList (HDD/HDD_OSD/system) failed: %d\n", result);
            }
        }

        if (result >= 0 && !(flags & INSTALL_MODE_FLAG_SKIP_CLEANUP)) {
            if ((result = _CleanupHDDTarget()) < 0) {
                DisplayErrorMessage(SYS_UI_MSG_CLEANUP_FAILED);
            }
        }

        // Create basic folders.
        if (result >= 0) {
            InstallLog("Creating folders on HDD ...\n");
            if ((result = CreateBasicFoldersOnHDD(flags)) < 0) {
                DEBUG_PRINTF("CreateBasicFoldersOnHDD failed: %d\n", result);
            }
            InstallLog("Folders created (%d). Starting copy of %u entries, %u bytes.\n",
                       result, NumFiles + NumDirectories, TotalRequiredSpaceForFiles);
        }

        if (result >= 0) {
            // Begin copying files
            if ((result = CopyFilesToHDD(RootFolder, FileCopyList, NumFiles + NumDirectories, TotalRequiredSpaceForFiles, flags)) < 0) {
                DEBUG_PRINTF("CopyFilesToHDD failed: %d\n", result);
            }
        }

        if (result >= 0) {
            if ((result = EnableHDDBooting()) < 0) {
                DEBUG_PRINTF("EnableHDDBooting() failed: %d\n", result);
                result |= ERROR_SIDE_DST;
            }
        }

        free(FileCopyList);
    } else
        result = -ENOMEM;

    SignalSema(InstallLockSema);

    return result;
}

int PerformInstallation(unsigned char port, unsigned char slot, unsigned int flags)
{
    struct FileCopyTarget *FileCopyList;
    unsigned int NumFiles, NumDirectories, i, file, TotalRequiredSpaceForFiles, TotalRequiredSpace, AvailableSpace;
    int result;
    char RootFolder[256], MGLetter;
    unsigned char TargetSystemType;

    if (!(flags & INSTALL_MODE_FLAG_CROSS_PSX)) {
        TargetSystemType = PS2SystemType;
        MGLetter = MGFolderRegion;
    } else {
        TargetSystemType = PS2_SYSTEM_TYPE_PSX;
        MGLetter = 'I';
    }

    GetInstallRootPath(RootFolder, sizeof(RootFolder));
#ifdef DIAG_INSTALL_PATH
    ShowInstallPathDiag(RootFolder);
#endif

    WaitSema(InstallLockSema);

    // Generate the file copy list.
    /* SYS-CONF is added generically (whole folder) later, so it contributes no
       entries here. */
    NumFiles = SYS_FOLDER_RESOURCES_NUM_FILES;
    switch (TargetSystemType) {
        case PS2_SYSTEM_TYPE_PSX:
            NumFiles += PSX_SYS_INSTALL_NUM_FILES;
            break;
        case PS2_SYSTEM_TYPE_DEX:
            NumFiles += DEX_SYS_INSTALL_NUM_FILES;
            break;
        default: // Default to CEX
            NumFiles += PS2_SYS_INSTALL_NUM_FILES;
    }
    NumDirectories = 0;
    TotalRequiredSpaceForFiles = 0;

    /* No NumFiles-- for SKIP_CNF: FREEMCB.CNF now comes from the generic SYS-CONF
       folder copy and is flagged FILE_SKIP there instead. */

    if ((flags & INSTALL_MODE_FLAG_MULTI_INST) || (flags & INSTALL_MODE_FLAG_CROSS_REG) || (MGLetter == 'I' && (flags & INSTALL_MODE_FLAG_CROSS_MODEL))) {
        NumFiles += (ROM100J_UPDATE_NUM_FILES + ROM101J_UPDATE_NUM_FILES);
    } else {
        if (MGLetter == 'I' && ROMVersion == 0x100) {
            NumFiles += ROM100J_UPDATE_NUM_FILES;
        } else if (MGLetter == 'I' && ROMVersion == 0x101) {
            NumFiles += ROM101J_UPDATE_NUM_FILES;
        }
    }

    // Remember to include resource files for each system folder!
    if ((flags & INSTALL_MODE_FLAG_MULTI_INST) || (flags & INSTALL_MODE_FLAG_CROSS_REG)) {
        NumFiles += SYS_FOLDER_RESOURCES_NUM_FILES * 3; // For the other three system folders.
    }

    if ((MGLetter == 'I' && ROMVersion <= 0x120) || (flags & INSTALL_MODE_FLAG_MULTI_INST) || (flags & INSTALL_MODE_FLAG_CROSS_REG) || (MGLetter == 'I' && (flags & INSTALL_MODE_FLAG_CROSS_MODEL))) {
        NumFiles += PS2_SYS_HDDLOAD_INSTALL_NUM_FILES;
    }

    if (flags & INSTALL_MODE_FLAG_CROSS_MODEL) {
        for (i = 0; i < NUM_CROSSLINKED_FILES; i++) {
            if (strncmp(FileAlias[i].name, SysExecFolder, sizeof(SysExecFolder) - 1) == 0 && strcmp(&FileAlias[i].name[sizeof(SysExecFolder)], SysExecFile) != 0) {
                NumFiles++;
            }
        }
    } else if (flags & INSTALL_MODE_FLAG_CROSS_REG) {
        for (i = 0; i < NUM_CROSSLINKED_FILES; i++) {
            if (strncmp(FileAlias[i].name, SysExecFolder, sizeof(SysExecFolder) - 1) != 0 || strcmp(&FileAlias[i].name[sizeof(SysExecFolder)], SysExecFile) != 0) {
                NumFiles++;
            }
        }
    }

    if ((FileCopyList = malloc(NumFiles * sizeof(struct FileCopyTarget))) != NULL) {
        memset(FileCopyList, 0, NumFiles * sizeof(struct FileCopyTarget));

        file = 0;
        if (TargetSystemType == PS2_SYSTEM_TYPE_PSX) {
            for (i = 0; i < PSX_SYS_INSTALL_NUM_FILES; i++, file++) {
                FileCopyList[file].source = malloc(strlen(PSXSysFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, PSXSysFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(PSXSysFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, PSXSysFiles[i].DestRelPath);

                FileCopyList[file].flags = PSXSysFiles[i].flags;
            }
        } else if (TargetSystemType == PS2_SYSTEM_TYPE_DEX) {
            for (i = 0; i < DEX_SYS_INSTALL_NUM_FILES; i++, file++) {
                FileCopyList[file].source = malloc(strlen(DEXSysFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, DEXSysFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(DEXSysFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, DEXSysFiles[i].DestRelPath);

                FileCopyList[file].flags = DEXSysFiles[i].flags;
            }
        } else { // Default to CEX (Consumer) PS2.
            for (i = 0; i < PS2_SYS_INSTALL_NUM_FILES; i++, file++) {
                FileCopyList[file].source = malloc(strlen(PS2SysFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, PS2SysFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(PS2SysFiles[i].DestRelPath) + 1);
                if (strcmp(PS2SysFiles[i].DestRelPath, "BREXEC-SYSTEM/osdmain.elf") != 0) {
                    strcpy(FileCopyList[file].target, PS2SysFiles[i].DestRelPath);
                } else {
                    sprintf(FileCopyList[file].target, "BREXEC-SYSTEM/%s", SysExecFile);
                }

                FileCopyList[file].flags = PS2SysFiles[i].flags;
            }

            if (flags & INSTALL_MODE_FLAG_CROSS_MODEL) {
                for (i = 0; i < NUM_CROSSLINKED_FILES; i++) {
                    if (strncmp(FileAlias[i].name, SysExecFolder, sizeof(SysExecFolder) - 1) == 0 && strcmp(&FileAlias[i].name[sizeof(SysExecFolder)], SysExecFile) != 0) {
                        FileCopyList[file].source = malloc(strlen(PS2SysFiles[0].SrcRelPath) + 1);
                        strcpy(FileCopyList[file].source, PS2SysFiles[0].SrcRelPath);

                        FileCopyList[file].target = malloc(strlen(FileAlias[i].name) + 1);
                        strcpy(FileCopyList[file].target, FileAlias[i].name);

                        FileCopyList[file].flags = PS2SysFiles[0].flags;
                        file++;
                    }
                }
            } else if (flags & INSTALL_MODE_FLAG_CROSS_REG) {
                for (i = 0; i < NUM_CROSSLINKED_FILES; i++) {
                    if (strncmp(FileAlias[i].name, SysExecFolder, sizeof(SysExecFolder) - 1) != 0 || strcmp(&FileAlias[i].name[sizeof(SysExecFolder)], SysExecFile) != 0) {
                        FileCopyList[file].source = malloc(strlen(PS2SysFiles[0].SrcRelPath) + 1);
                        strcpy(FileCopyList[file].source, PS2SysFiles[0].SrcRelPath);

                        FileCopyList[file].target = malloc(strlen(FileAlias[i].name) + 1);
                        strcpy(FileCopyList[file].target, FileAlias[i].name);

                        FileCopyList[file].flags = PS2SysFiles[0].flags;
                        file++;
                    }
                }
            }
        }

        if ((MGLetter == 'I' && ROMVersion == 0x100) || (flags & INSTALL_MODE_FLAG_MULTI_INST) || (flags & INSTALL_MODE_FLAG_CROSS_REG) || (MGLetter == 'I' && (flags & INSTALL_MODE_FLAG_CROSS_MODEL))) {
            for (i = 0; i < ROM100J_UPDATE_NUM_FILES; i++, file++) {
                FileCopyList[file].source = malloc(strlen(BootROM100JUpdateFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, BootROM100JUpdateFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(BootROM100JUpdateFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, BootROM100JUpdateFiles[i].DestRelPath);

                FileCopyList[file].flags = BootROM100JUpdateFiles[i].flags;
            }
        }

        if ((MGLetter == 'I' && ROMVersion == 0x101) || (flags & INSTALL_MODE_FLAG_MULTI_INST) || (flags & INSTALL_MODE_FLAG_CROSS_REG) || (MGLetter == 'I' && (flags & INSTALL_MODE_FLAG_CROSS_MODEL))) {
            for (i = 0; i < ROM101J_UPDATE_NUM_FILES; i++, file++) {
                FileCopyList[file].source = malloc(strlen(BootROM101JUpdateFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, BootROM101JUpdateFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(BootROM101JUpdateFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, BootROM101JUpdateFiles[i].DestRelPath);

                FileCopyList[file].flags = BootROM101JUpdateFiles[i].flags;
            }
        }

        if ((MGLetter == 'I' && ROMVersion <= 0x120) || (flags & INSTALL_MODE_FLAG_MULTI_INST) || (flags & INSTALL_MODE_FLAG_CROSS_REG) || (MGLetter == 'I' && (flags & INSTALL_MODE_FLAG_CROSS_MODEL))) {
            for (i = 0; i < PS2_SYS_HDDLOAD_INSTALL_NUM_FILES; i++, file++) {
                FileCopyList[file].source = malloc(strlen(PS2HDDLOADSysFiles[i].SrcRelPath) + 1);
                strcpy(FileCopyList[file].source, PS2HDDLOADSysFiles[i].SrcRelPath);

                FileCopyList[file].target = malloc(strlen(PS2HDDLOADSysFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, PS2HDDLOADSysFiles[i].DestRelPath);

                FileCopyList[file].flags = PS2HDDLOADSysFiles[i].flags;
            }
        }

        // Install resource files for the other system folders.
        if ((flags & INSTALL_MODE_FLAG_MULTI_INST) || (flags & INSTALL_MODE_FLAG_CROSS_REG)) {
            // Japan
            for (i = 0; i < SYS_FOLDER_RESOURCES_NUM_FILES; i++, file++) {

                if (strcmp(SysResourceFiles[i].SrcRelPath, "SYSTEM/ICON.SYS") == 0) {
                    FileCopyList[file].source = malloc(strlen("SYSTEM/BIICON.SYS") + 1);
                    strcpy(FileCopyList[file].source, "SYSTEM/BIICON.SYS");
                } else {
                    FileCopyList[file].source = malloc(strlen(SysResourceFiles[i].SrcRelPath) + 1);
                    strcpy(FileCopyList[file].source, SysResourceFiles[i].SrcRelPath);
                }
                FileCopyList[file].target = malloc(strlen(SysResourceFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, SysResourceFiles[i].DestRelPath);

                if (strncmp(FileCopyList[file].target, "BREXEC-SYSTEM", 13) == 0) {
                    FileCopyList[file].target[1] = 'I';
                }

                FileCopyList[file].flags = SysResourceFiles[i].flags;
            }

            // Europe
            for (i = 0; i < SYS_FOLDER_RESOURCES_NUM_FILES; i++, file++) {

                if (strcmp(SysResourceFiles[i].SrcRelPath, "SYSTEM/ICON.SYS") == 0) {
                    FileCopyList[file].source = malloc(strlen("SYSTEM/BEICON.SYS") + 1);
                    strcpy(FileCopyList[file].source, "SYSTEM/BEICON.SYS");
                } else {
                    FileCopyList[file].source = malloc(strlen(SysResourceFiles[i].SrcRelPath) + 1);
                    strcpy(FileCopyList[file].source, SysResourceFiles[i].SrcRelPath);
                }

                FileCopyList[file].target = malloc(strlen(SysResourceFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, SysResourceFiles[i].DestRelPath);

                if (strncmp(FileCopyList[file].target, "BREXEC-SYSTEM", 13) == 0) {
                    FileCopyList[file].target[1] = 'E';
                }

                FileCopyList[file].flags = SysResourceFiles[i].flags;
            }

            // USA/HK/SG
            for (i = 0; i < SYS_FOLDER_RESOURCES_NUM_FILES; i++, file++) {

                if (strcmp(SysResourceFiles[i].SrcRelPath, "SYSTEM/ICON.SYS") == 0) {
                    FileCopyList[file].source = malloc(strlen("SYSTEM/BAICON.SYS") + 1);
                    strcpy(FileCopyList[file].source, "SYSTEM/BAICON.SYS");
                } else {
                    FileCopyList[file].source = malloc(strlen(SysResourceFiles[i].SrcRelPath) + 1);
                    strcpy(FileCopyList[file].source, SysResourceFiles[i].SrcRelPath);
                }

                FileCopyList[file].target = malloc(strlen(SysResourceFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, SysResourceFiles[i].DestRelPath);

                if (strncmp(FileCopyList[file].target, "BREXEC-SYSTEM", 13) == 0) {
                    FileCopyList[file].target[1] = 'A';
                }

                FileCopyList[file].flags = SysResourceFiles[i].flags;
            }

            // Mainland China
            for (i = 0; i < SYS_FOLDER_RESOURCES_NUM_FILES; i++, file++) {
                if (strcmp(SysResourceFiles[i].SrcRelPath, "SYSTEM/ICON.SYS") == 0) {
                    FileCopyList[file].source = malloc(strlen("SYSTEM/BCICON.SYS") + 1);
                    strcpy(FileCopyList[file].source, "SYSTEM/BCICON.SYS");
                } else {
                    FileCopyList[file].source = malloc(strlen(SysResourceFiles[i].SrcRelPath) + 1);
                    strcpy(FileCopyList[file].source, SysResourceFiles[i].SrcRelPath);
                }

                FileCopyList[file].target = malloc(strlen(SysResourceFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, SysResourceFiles[i].DestRelPath);

                if (strncmp(FileCopyList[file].target, "BREXEC-SYSTEM", 13) == 0) {
                    FileCopyList[file].target[1] = 'C';
                }

                FileCopyList[file].flags = SysResourceFiles[i].flags;
            }
        } else {
            for (i = 0; i < SYS_FOLDER_RESOURCES_NUM_FILES; i++, file++) {

                if (strcmp(SysResourceFiles[i].SrcRelPath, "SYSTEM/ICON.SYS") == 0) {
                    FileCopyList[file].source = malloc(strlen("SYSTEM/BRICON.SYS") + 1);
                    strcpy(FileCopyList[file].source, "SYSTEM/BRICON.SYS");
                    FileCopyList[file].source[8] = MGLetter;
                } else {
                    FileCopyList[file].source = malloc(strlen(SysResourceFiles[i].SrcRelPath) + 1);
                    strcpy(FileCopyList[file].source, SysResourceFiles[i].SrcRelPath);
                }
                FileCopyList[file].target = malloc(strlen(SysResourceFiles[i].DestRelPath) + 1);
                strcpy(FileCopyList[file].target, SysResourceFiles[i].DestRelPath);

                FileCopyList[file].flags = SysResourceFiles[i].flags;
            }
        }

        /* NOTE: SYS-CONF is NOT a hardcoded file list -- its whole contents are
           added generically further below, so the payload can ship whatever
           configuration files it likes. */

        // Start getting the sizes and attributes of the files.
        result = GetAllBaseFileStats(MGLetter, RootFolder, FileCopyList, NumFiles, &TotalRequiredSpaceForFiles);

        /* Everything under INSTALL/MC is copied verbatim to the root of the memory
           card, so the payload alone decides what ends up there:
               INSTALL/MC/BOOT/...      -> mc0:/BOOT/...
               INSTALL/MC/SYS-CONF/...  -> mc0:/SYS-CONF/...
               INSTALL/MC/APP_WLE-R3Z/  -> mc0:/APP_WLE-R3Z/
           Two legacy upper-case names are written in lower case for older packages,
           whose icon.sys refers to them that way. */
        if (result >= 0) {
            unsigned int McStart = NumFiles + NumDirectories;

            if ((result = AddDirContentsToFileCopyList(RootFolder, "MC", "", 1, &FileCopyList, &NumFiles, &NumDirectories, &TotalRequiredSpaceForFiles)) < 0) {
                DEBUG_PRINTF("AddDirContentsToFileCopyList (MC) failed: %d\n", result);
            } else {
                for (i = McStart; i < NumFiles + NumDirectories; i++) {
                    if (FileCopyList[i].target == NULL)
                        continue;

                    if (strcmp(FileCopyList[i].target, "SYS-CONF/ICON.SYS") == 0)
                        strcpy(FileCopyList[i].target, "SYS-CONF/icon.sys");
                    else if (strcmp(FileCopyList[i].target, "SYS-CONF/SYSCONF.ICN") == 0)
                        strcpy(FileCopyList[i].target, "SYS-CONF/sysconf.icn");

                    // Keep the user's existing configuration file, if requested.
                    if ((flags & INSTALL_MODE_FLAG_SKIP_CNF) && strstr(FileCopyList[i].target, "FREEMCB.CNF") != NULL)
                        FileCopyList[i].flags |= FILE_SKIP;
                }
            }
        }

        // Calculate available and required space.
        TotalRequiredSpace = CalculateRequiredSpace(FileCopyList, NumFiles, NumDirectories);
        // I do not like this, but the Sony documentation implies that we can (And have to?) assume that the memory card has a cluster size of 2.
        TotalRequiredSpace += 1024; // 1 cluster

        AvailableSpace = GetMcFreeSpace(port, slot) * 1024;
        TotalRequiredSpace += (NumFiles + NumDirectories + 3) / 2; // A new cluster is required for every two files.
        // Multi-installations will need more space.
        if (flags & INSTALL_MODE_FLAG_MULTI_INST) {
            TotalRequiredSpace += (NUM_CROSSLINKED_FILES + 1) / 2; // Reserve one additional file slot for the uninstallation file.
        }
        TotalRequiredSpace += NumDirectories * 2;

        if (AvailableSpace < TotalRequiredSpace) {
            DEBUG_PRINTF("Insuffient space on card: %u required, %u available\n", TotalRequiredSpace, AvailableSpace);
            DisplayOutOfSpaceMessage(AvailableSpace, TotalRequiredSpace);
            result = -ENOSPC;
        }

        if (result >= 0 && !(flags & INSTALL_MODE_FLAG_SKIP_CLEANUP)) {
            if ((result = CleanupTarget(port, slot)) < 0) {
                DisplayErrorMessage(SYS_UI_MSG_CLEANUP_FAILED);
            }
        }

        // Create basic folders.
        if (result >= 0) {
            if ((result = CreateBasicFolders(port, slot, flags)) < 0) {
                DEBUG_PRINTF("CreateBasicFolders failed: %d\n", result);
            }
        }

        if (result >= 0) {
            // Begin copying files
            if ((result = CopyFiles(RootFolder, port, slot, FileCopyList, NumFiles + NumDirectories, TotalRequiredSpaceForFiles, flags)) < 0) {
                DEBUG_PRINTF("CopyFiles failed: %d\n", result);
            }
        }

        // Free filenames.
        for (i = 0; i < NumFiles + NumDirectories; i++) {
            if (FileCopyList[i].source != NULL)
                free(FileCopyList[i].source);
            if (FileCopyList[i].target != NULL)
                free(FileCopyList[i].target);
        }

        free(FileCopyList);

        if ((flags & INSTALL_MODE_FLAG_MULTI_INST) && result >= 0) {
            DEBUG_PRINTF("Generating uninstall file...\n");

            if ((result = GenerateUninstallFile(port, slot)) >= 0) {
                DEBUG_PRINTF("Generating cross-linked files...\n");
                result = CreateCrossLinkedFiles(port, slot);
                DEBUG_PRINTF("Completed generating cross-linked files.\n");

                if (result < 0)
                    result = -EEXTCRSLNKFAIL;
            }
        }
    } else
        result = -ENOMEM;

    SignalSema(InstallLockSema);

    return result;
}

static int SyncMCFileWrite(int fd, int size, void *Buffer)
{
    int result;

    result = 0;
    mcSync(0, NULL, &result);

    mcClose(fd);

    /* Free the buffer. */
    if (Buffer != NULL)
        free(Buffer);

    if (result < 0)
        DEBUG_PRINTF("Memory Card write failed: %d\n", result);
    else {
        if (result != size) {
            DEBUG_PRINTF("Memory Card write size mismatch: %d\n", result);
            result = -EIO;
        }
    }

    mcSync(0, NULL, NULL);

    return result;
}

static int CopyFiles(const char *RootFolder, unsigned char port, unsigned char slot, const struct FileCopyTarget *FileCopyList, unsigned int NumFilesEntries, unsigned int TotalNumBytes, unsigned int flags)
{
    unsigned int i, size, PrevFileSize, BytesCopied;
    int McFileFD, result;
    FILE *file;
    char *path;
    void *buffer, *NextFileBuf;

    InitProgressScreen(SYS_UI_LBL_INSTALLING);

    result = 0;
    for (i = 0, PrevFileSize = 0, buffer = NULL, BytesCopied = 0; (i < NumFilesEntries) && (result >= 0); i++, BytesCopied += PrevFileSize) {
        DrawFileCopyProgressScreen((float)((double)BytesCopied / TotalNumBytes));

        if (FileCopyList[i].flags & FILE_SKIP) {
            /* Not present in the payload. Flush the previous file's pending write
               first (writes are deferred to the next iteration), exactly as the
               directory case below does, so skipping never drops data. */
            if (i > 0 && PrevFileSize > 0) {
                result = SyncMCFileWrite(McFileFD, PrevFileSize, buffer);
                if (result < 0)
                    break;
                PrevFileSize = 0;
            }
            continue;
        }

        if (FIO_S_ISDIR(FileCopyList[i].mode)) {
            if (i > 0 && PrevFileSize > 0) {
                result = SyncMCFileWrite(McFileFD, PrevFileSize, buffer);
                if (result < 0)
                    break;
                PrevFileSize = 0;
            }

            DEBUG_PRINTF("mkdir: %s\n", FileCopyList[i].target);

            if ((result = mcMkDir(port, slot, FileCopyList[i].target)) == 0) {
                mcSync(0, NULL, &result);
                if (result == -4)
                    result = 0; // EEXIST doesn't count as an error.
            }
        } else {
            path = malloc(strlen(RootFolder) + strlen(FileCopyList[i].source) + 2);
            sprintf(path, "%s/%s", RootFolder, FileCopyList[i].source);

            DEBUG_PRINTF("Copying %s -> %s...\n", FileCopyList[i].source, FileCopyList[i].target);

            if ((file = fopen(path, "rb")) == NULL) {
                DEBUG_PRINTF("Error: Can't open file: %s, %d\n", path, errno);

                free(path);

                result = (-errno) | ERROR_SIDE_SRC;
                break;
            }

            free(path);

            /* Copy the file */
            size = FileCopyList[i].size;

            if ((NextFileBuf = memalign(64, (size + 0x3F) & ~0x3F)) != NULL) {
                if (fread(NextFileBuf, 1, size, file) != size) {
                    fclose(file);
                    result = ERROR_SIDE_SRC | -EIO;
                } else {
                    fclose(file);

                    if (i > 0 && PrevFileSize > 0) {
                        result = SyncMCFileWrite(McFileFD, PrevFileSize, buffer);
                        if (result < 0)
                            break;
                    }

                    if (FileCopyList[i].flags & FILE_IS_KELF) { /* Sign the KELF file before writing it. */
                        if (!(flags & INSTALL_MODE_FLAG_CROSS_PSX)) {
                            if ((result = SignKELF(NextFileBuf, size, port, slot)) < 0) {
                                DEBUG_PRINTF("Error signing file %s. Code: %d.\n", FileCopyList[i].source, result);
                                free(NextFileBuf);
                                result = -EEXTMGSIGNERR;
                                break;
                            }
                        } else {
                            if ((result = TwinSignKELF(RootFolder, NextFileBuf, size, port, slot)) < 0) {
                                DEBUG_PRINTF("Error twin-signing file %s. Code: %d.\n", FileCopyList[i].source, result);
                                free(NextFileBuf);
                                result = -EEXTMGSIGNERR;
                                break;
                            }
                        }
                    }

                    /* Point the buffer pointer to the buffer containing the next file. */
                    buffer = NextFileBuf;

                    mcOpen(port, slot, FileCopyList[i].target, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
                    mcSync(0, NULL, &McFileFD);

                    if (McFileFD < 0) {
                        DEBUG_PRINTF("Failed to create file on memory card: %s (%d)\n", FileCopyList[i].target, McFileFD);
                        result = McFileFD;
                    } else {
                        mcWrite(McFileFD, buffer, size);
                        PrevFileSize = size;
                    }
                }
            } else {
                result = -ENOMEM;
            }
        }
    }

    if (result >= 0) { /* After the copying process has begun for the last file, we need to synchronize writing. */
        result = SyncMCFileWrite(McFileFD, PrevFileSize, buffer);
    }

    return result;
}

int HasOldFMCBConfigFile(int port, int slot)
{
    int result;
    FILE *file;
    char path[25];

    sprintf(path, "mc%u:SYS-CONF/FREEMCB.CNF", port);
    if ((file = fopen(path, "r")) != NULL) {
        result = 1;
        fclose(file);
    } else
        result = 0;

    return result;

    /*	For some reason, getstat() does not return an error when uninstall.dat does not exist. :(
        return((fileXioGetstat(path, &DirEntData)==0)?1:0); */
}

int HasOldFMCBConfigFileOnHDD(void)
{
    int result;
    FILE *file;

    result = 0;
    if (fileXioMount("pfs0:", "hdd0:__sysconf", FIO_MT_RDONLY) >= 0) {
        if ((file = fopen("pfs0:/FMCB/FREEHDB.CNF", "r")) != NULL) {
            result = 1;
            fclose(file);
        }

        fileXioUmount("pfs0:");
    }

    return result;
}

int HasOldMultiInstall(int port, int slot)
{
    int result;
    FILE *file;
    char path[27];

    sprintf(path, "mc%u:SYS-CONF/FMCBUINST.dat", port);
    if ((file = fopen(path, "rb")) != NULL) {
        result = 1;
        fclose(file);
    } else {
        sprintf(path, "mc%u:SYS-CONF/uninstall.dat", port);
        if ((file = fopen(path, "rb")) != NULL) {
            result = 1;
            fclose(file);
        } else
            result = 0;
    }

    return result;

    /*	For some reason, getstat() does not return an error when uninstall.dat does not exist. :(
        return((fileXioGetstat(path, &DirEntData)==0)?1:0); */
}

static inline int HandleLegacyFMCBUninstallFile(FILE *file, char *MGFolderLetter, unsigned short int *BootROMVersion, struct FileAlias **LocalMcFileAlias)
{
    int result, num_entries, i;
    char FMCBInstConRegion, RomVersion[5];

    result = 0;
    fread(RomVersion, 1, 4, file);
    RomVersion[4] = '\0';
    fread(&FMCBInstConRegion, 1, 1, file);

    *BootROMVersion = (unsigned short int)strtoul(RomVersion, NULL, 16);
    *MGFolderLetter = GetMGFolderLetter(FMCBInstConRegion);

    fseek(file, 24, SEEK_SET);
    if (fread(&num_entries, sizeof(int), 1, file) == 1) {
        DEBUG_PRINTF("HandleLegacyFMCBUninstallFile: num_entries: %d\n", num_entries);

        *LocalMcFileAlias = malloc(sizeof(struct FileAlias) * (num_entries + 1));
        (*LocalMcFileAlias)[num_entries].name[0] = '\0';

        for (i = 0; i < num_entries; i++) {
            fseek(file, 8, SEEK_CUR);
            sprintf((*LocalMcFileAlias)[i].name, "B%cEXEC-SYSTEM/", *MGFolderLetter);
            fread(&(*LocalMcFileAlias)[i].name[14], 1, 16, file); // Insert after BREXEC-SYSTEM/
            (*LocalMcFileAlias)[i].name[14 + 16] = '\0';
            DEBUG_PRINTF("HandleLegacyFMCBUninstallFile: LocalMcFileAlias[%d].name=%s\n", i, (*LocalMcFileAlias)[i].name);
        }
        (*LocalMcFileAlias)[i].name[0] = '\0';
        result = num_entries;
    } else
        result = -EIO;

    return result;
}

static inline int HandleFMCBUninstallFile(FILE *file, char *MGFolderLetter, unsigned short int *BootROMVersion, struct FileAlias **LocalMcFileAlias)
{
    int result, i;
    unsigned char num_entries;
    char FMCBInstConRegion, RomVersion[5];

    result = 0;
    fseek(file, sizeof(struct UninstallationDataFileHeader), SEEK_SET);
    if (fread(RomVersion, 1, 4, file) == 4 && fread(&FMCBInstConRegion, 1, 1, file) == 1 && fread(&num_entries, 1, 1, file) == 1) {
        DEBUG_PRINTF("HandleFMCBUninstallFile: num_entries: %d\n", num_entries);

        RomVersion[4] = '\0';
        *BootROMVersion = (unsigned short int)strtoul(RomVersion, NULL, 16);
        *MGFolderLetter = GetMGFolderLetter(FMCBInstConRegion);
        *LocalMcFileAlias = malloc(sizeof(struct FileAlias) * (num_entries + 1));

        for (i = 0; i < num_entries; i++) {
            if (fread((*LocalMcFileAlias)[i].name, 1, sizeof((*LocalMcFileAlias)[i].name), file) == sizeof((*LocalMcFileAlias)[i].name))
                DEBUG_PRINTF("HandleFMCBUninstallFile: LocalMcFileAlias[%d].name=%s\n", i, (*LocalMcFileAlias)[i].name);
            else {
                result = -EIO;
                break;
            }
        }
        (*LocalMcFileAlias)[i].name[0] = '\0';
        result = num_entries;
    } else
        result = -EIO;

    return result;
}

static inline int HandleFMCBUninstallFile_100(FILE *file, char *MGFolderLetter, unsigned short int *BootROMVersion, struct FileAlias **LocalMcFileAlias)
{
    int result, i;
    unsigned char num_entries;
    char FMCBInstConRegion, RomVersion[5];

    result = 0;
    fseek(file, sizeof(struct UninstallationDataFileHeader), SEEK_SET);
    if (fread(RomVersion, 1, 4, file) == 4 && fread(&FMCBInstConRegion, 1, 1, file) == 1 && fread(&num_entries, 1, 1, file) == 1) {
        DEBUG_PRINTF("HandleFMCBUninstallFile_100: num_entries: %d\n", num_entries);

        RomVersion[4] = '\0';
        *BootROMVersion = (unsigned short int)strtoul(RomVersion, NULL, 16);
        *MGFolderLetter = GetMGFolderLetter(FMCBInstConRegion);
        *LocalMcFileAlias = malloc(sizeof(struct FileAlias) * (num_entries + 1));

        for (i = 0; i < num_entries; i++) {
            sprintf((*LocalMcFileAlias)[i].name, "B%cEXEC-SYSTEM/", *MGFolderLetter);
            fread(&(*LocalMcFileAlias)[i].name[14], 1, 16, file); // Insert after BREXEC-SYSTEM/
            (*LocalMcFileAlias)[i].name[14 + 16] = '\0';
            DEBUG_PRINTF("HandleFMCBUninstallFile_100: LocalMcFileAlias[%d].name=%s\n", i, (*LocalMcFileAlias)[i].name);
        }
        (*LocalMcFileAlias)[i].name[0] = '\0';
        result = num_entries;
    } else
        result = -EIO;

    return result;
}

int CleanupMultiInstallation(int port, int slot)
{
    struct FileAlias *LocalMcFileAlias;
    struct UninstallationDataFileHeader FileHeader;
    unsigned char MGFolderLetter, i;
    unsigned short int InstallerBootROMVersion;
    char path[27], *FullPath;
    int result, FMCBUninstallFileVersion, NumCrosslinkedFiles;
    FILE *file;
    int InitSemaID;

    // Reboot the IOP to load MCTOOLS.IRX in. This will also prevent MCMAN's cache from storing outdated content.
    InitSemaID = IopInitStart(IOP_MOD_MCTOOLS | IOP_REBOOT);

    sprintf(path, "mc%u:SYS-CONF/FMCBUINST.dat", port);
    memset(&FileHeader, 0, sizeof(struct UninstallationDataFileHeader));

    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    // Call sceMcGetInfo on the memory card to be accessed, after IOP reboots. Otherwise I/O operations may fail.
    if ((result = InitMCInfo(port, slot)) != 0)
        return result;

    if ((result = MCToolsInitPageCache(port, slot)) < 0) {
        return EEXTCACHEINITERR;
    }

    result = 0;

    WaitSema(InstallLockSema);

    if ((file = fopen(path, "rb")) == NULL) {
        sprintf(path, "mc%u:SYS-CONF/uninstall.dat", port);
        if ((file = fopen(path, "rb")) != NULL)
            FMCBUninstallFileVersion = 0;
    } else {
        if (fread(&FileHeader, 1, sizeof(struct UninstallationDataFileHeader), file) == sizeof(struct UninstallationDataFileHeader)) {
            FMCBUninstallFileVersion = FileHeader.version;
            rewind(file);
        } else
            FMCBUninstallFileVersion = -1;

        if ((FileHeader.signature[0] != 'F' || FileHeader.signature[1] != 'B') || (FMCBUninstallFileVersion != UNINST_FILE_VERSION && FMCBUninstallFileVersion != UNINST_FILE_VERSION_100)) {
            result = -EEXTUNSUPPUNINST;
            fclose(file);
        }
    }

    if ((file != NULL) && (result >= 0)) {
        DEBUG_PRINTF("Uninstall file loaded.\n");

        if (FMCBUninstallFileVersion != 0) {
            if (FMCBUninstallFileVersion == UNINST_FILE_VERSION) {
                result = HandleFMCBUninstallFile(file, &MGFolderLetter, &InstallerBootROMVersion, &LocalMcFileAlias);
            } else if (FMCBUninstallFileVersion == UNINST_FILE_VERSION_100) {
                result = HandleFMCBUninstallFile_100(file, &MGFolderLetter, &InstallerBootROMVersion, &LocalMcFileAlias);
            } else
                result = -1;
        } else {
            result = HandleLegacyFMCBUninstallFile(file, &MGFolderLetter, &InstallerBootROMVersion, &LocalMcFileAlias);
        }

        fclose(file);

        if (result >= 0) {
            NumCrosslinkedFiles = result;

            mcDelete(port, slot, (FMCBUninstallFileVersion == 0) ? "SYS-CONF/uninstall.dat" : "SYS-CONF/FMCBUINST.dat");
            mcSync(0, NULL, NULL);

            for (i = 0; LocalMcFileAlias[i].name[0] != '\0'; i += result) {
                DEBUG_PRINTF("LocalMcFileAlias[%d]: %s\n", i, LocalMcFileAlias[i].name);
                if ((result = MCToolsDeleteCrossLinkedFiles(port, slot, ".", &LocalMcFileAlias[i], NumCrosslinkedFiles)) <= 0) {
                    result = 1; // If the entry can't be deleted, don't fail here. Just do whatever that can be done.
                }
            }

            MCToolsFlushPageCache(); // This must be strictly done immediately after crosslinking operation.

            DEBUG_PRINTF("Deleting other multi-installation files and folders.\n");

            if (MGFolderLetter != 'I' || InstallerBootROMVersion != 0x100) {
                for (i = 0; i < ROM100J_UPDATE_NUM_FILES; i++) {
                    FullPath = malloc(strlen(BootROM100JUpdateFiles[i].DestRelPath) + 1);
                    strcpy(FullPath, BootROM100JUpdateFiles[i].DestRelPath);

                    // Update regional paths.
                    if (strncmp(FullPath, "BREXEC-SYSTEM", 13) == 0) {
                        FullPath[1] = MGFolderLetter;
                    }

                    mcDelete(port, slot, FullPath);
                    mcSync(0, NULL, NULL);
                    free(FullPath);
                }
            }

            if (MGFolderLetter != 'I' || InstallerBootROMVersion != 0x101) {
                for (i = 0; i < ROM101J_UPDATE_NUM_FILES; i++) {
                    FullPath = malloc(strlen(BootROM101JUpdateFiles[i].DestRelPath) + 1);
                    strcpy(FullPath, BootROM101JUpdateFiles[i].DestRelPath);

                    // Update regional paths.
                    if (strncmp(FullPath, "BREXEC-SYSTEM", 13) == 0) {
                        FullPath[1] = MGFolderLetter;
                    }

                    mcDelete(port, slot, FullPath);
                    mcSync(0, NULL, NULL);
                    free(FullPath);
                }
            }

            if (MGFolderRegion != 'I' || ROMVersion > 0x120) {
                for (i = 0; i < PS2_SYS_HDDLOAD_INSTALL_NUM_FILES; i++) {
                    FullPath = malloc(strlen(PS2HDDLOADSysFiles[i].DestRelPath) + 1);
                    strcpy(FullPath, PS2HDDLOADSysFiles[i].DestRelPath);

                    // Update regional paths.
                    if (strncmp(FullPath, "BREXEC-SYSTEM", 13) == 0) {
                        FullPath[1] = MGFolderLetter;
                    }

                    mcDelete(port, slot, FullPath);
                    mcSync(0, NULL, NULL);
                    free(FullPath);
                }
            }

            /* Now, delete the 3 other system folders, which may be different from those that this console will generate. */
            switch (MGFolderLetter) {
                case 'I':
                    sprintf(path, "mc%u:BAEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BEEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BCEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    break;
                case 'A':
                    sprintf(path, "mc%u:BIEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BEEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BCEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    break;
                case 'E':
                    sprintf(path, "mc%u:BAEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BIEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BCEXEC-SYSTEM", port);
                    DeleteFolder(path);
                case 'C':
                    sprintf(path, "mc%u:BAEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BIEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    sprintf(path, "mc%u:BEEXEC-SYSTEM", port);
                    DeleteFolder(path);
                    break;
            }
        }

        if (LocalMcFileAlias != NULL)
            free(LocalMcFileAlias);
    }

    SignalSema(InstallLockSema);

    // Reboot the IOP to allow the remainder of the program to continue.
    InitSemaID = IopInitStart(IOP_MOD_SET_MAIN | IOP_REBOOT);
    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    return result;
}

static int CreateCrossLinkedFiles(int port, int slot)
{
    char path[27];
    int result, NumFiles, InitSemaID;
    unsigned int FileIndex;

    // Reboot the IOP to load MCTOOLS.IRX in. This will also prevent MCMAN's cache from storing outdated content.
    InitSemaID = IopInitStart(IOP_MOD_MCTOOLS | IOP_REBOOT);

    sprintf(path, "%s/%s", SysExecFolder, SysExecFile);

    result = 0;
    FileIndex = 0;
    for (NumFiles = 0; FileIndex + NumFiles < NUM_CROSSLINKED_FILES; NumFiles++) {
        if (strcmp(FileAlias[FileIndex + NumFiles].name, path) == 0) {
            break;
        }
    }

    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    // Call sceMcGetInfo on the memory card to be accessed, after IOP reboots. Otherwise I/O operations may fail.
    if ((result = InitMCInfo(port, slot)) != 0)
        return result;

    if ((result = MCToolsInitPageCache(port, slot)) < 0) {
        return -EEXTCACHEINITERR;
    }

    for (; NumFiles > 0; FileIndex += result, NumFiles -= result) {
        if ((result = MCToolsCreateCrossLinkedFiles(port, slot, path, &FileAlias[FileIndex], NumFiles)) <= 0) {
            result = -EIO;
            break;
        }
    }

    FileIndex++;

    for (NumFiles = NUM_CROSSLINKED_FILES - FileIndex; NumFiles > 0; FileIndex += result, NumFiles -= result) {
        if ((result = MCToolsCreateCrossLinkedFiles(port, slot, path, &FileAlias[FileIndex], NumFiles)) <= 0) {
            result = -EIO;
            break;
        }
    }

    if (result >= 0)
        result = MCToolsFlushPageCache(); // This must be strictly done immediately after crosslinking operation.

    // Reboot the IOP to allow the remainder of the program to continue.
    InitSemaID = IopInitStart(IOP_MOD_SET_MAIN | IOP_REBOOT);
    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    return result;
}

static int GenerateUninstallFile(int port, int slot)
{
    unsigned char i, TotalNumEnts;
    int result;
    FILE *file;
    char path[27], SysExecFileFullPath[66];
    struct UninstallationDataFileHeader FileHeader;

    sprintf(path, "mc%u:SYS-CONF/FMCBUINST.dat", port);

    result = 0;
    if ((file = fopen(path, "wb")) != NULL) {
        FileHeader.signature[0] = 'F';
        FileHeader.signature[1] = 'B';
        FileHeader.version = UNINST_FILE_VERSION;
        fwrite(&FileHeader, sizeof(struct UninstallationDataFileHeader), 1, file);

        fwrite(romver, 1, 5, file); /* Record the console's ROM version number and region code. */

        sprintf(SysExecFileFullPath, "%s/%s", SysExecFolder, SysExecFile);
        for (i = 0, TotalNumEnts = 0; i < NUM_CROSSLINKED_FILES; i++) {
            if (strcmp(FileAlias[i].name, SysExecFileFullPath) != 0) {
                TotalNumEnts++;
            }
        }
        fwrite(&TotalNumEnts, sizeof(TotalNumEnts), 1, file);

        for (i = 0; i < NUM_CROSSLINKED_FILES; i++) {
            if (strcmp(FileAlias[i].name, SysExecFileFullPath) != 0) {
                fwrite(FileAlias[i].name, 1, sizeof(FileAlias[i].name), file);
            }
        }

        fclose(file);

        if (result < 0) {
            mcDelete(port, slot, "SYS-CONF/FMCBUINST.dat");
            mcSync(0, NULL, NULL);
        }
    } else
        result = -errno;

    return result;
}

int CheckPrerequisites(const struct McData *McData, unsigned char OperationMode)
{
    unsigned int flags;
    static const unsigned char OpModeFlags[EVENT_OPTION_COUNT] = {
        CHECK_MULTI_INSTALL,           /* EVENT_INSTALL */
        CHECK_MULTI_INSTALL,           /* EVENT_MULTI_INSTALL */
        CHECK_MULTI_INSTALL,           /* EVENT_CLEANUP */
        CHECK_MUST_HAVE_MULTI_INSTALL, /* EVENT_CLEANUP_MULTI */
        0,                             /* EVENT_FORMAT */
        0,                             /* EVENT_DUMP_MC */
        0,                             /* EVENT_RESTORE_MC */
        0,                             /* EVENT_EXIT */
    };

    flags = OpModeFlags[OperationMode];

    if ((flags & CHECK_MUST_HAVE_MULTI_INSTALL) && (!(McData->flags & MC_FLAG_CARD_HAS_MULTI_INST))) {
        DisplayErrorMessage(SYS_UI_MSG_MULTI_INST_REQ);
        return -CHECK_MUST_HAVE_MULTI_INSTALL;
    }
    if ((flags & CHECK_MULTI_INSTALL) && (McData->flags & MC_FLAG_CARD_HAS_MULTI_INST)) {
        DisplayErrorMessage(SYS_UI_MSG_HAS_MULTI_INST);
        return -CHECK_MULTI_INSTALL;
    }

    return 0;
}

int GetNumMemcardsInserted(struct McData *McData)
{
    int result;
    unsigned char nCards, i;

    /* Poll the memory card slots. */
    for (i = 0, nCards = 2; i < 2; i++) {
        mcGetInfo(i, 0, &McData[i].Type, &McData[i].SpaceFree, &McData[i].Format);
        mcSync(0, NULL, &result);

        if (result < 0)
            McData[i].flags = 0; /* Clear the flags field if a new card has been inserted. */

        if (result < -1 || McData[i].Type != MC_TYPE_PS2) {
            DEBUG_PRINTF("No Playstation 2 Memory Card inserted into memory card slot %d.\n", i + 1);
            nCards--;
        } else {
            if (HasOldMultiInstall(i, 0))
                McData[i].flags |= MC_FLAG_CARD_HAS_MULTI_INST;
        }
    }

    return nCards;
}

int SysCreateThread(void *function, void *stack, unsigned int StackSize, void *arg, int priority)
{
    ee_thread_t ThreadData;
    int ThreadID;

    ThreadData.func = function;
    ThreadData.stack = stack;
    ThreadData.stack_size = StackSize;
    ThreadData.gp_reg = &_gp;
    ThreadData.initial_priority = priority;
    ThreadData.attr = ThreadData.option = 0;

    if ((ThreadID = CreateThread(&ThreadData)) >= 0) {
        if (StartThread(ThreadID, arg) < 0) {
            DeleteThread(ThreadID);
            ThreadID = -1;
        }
    }

    return ThreadID;
}

int PerformMemoryCardDump(int port, int slot)
{
    struct WorkerThreadMcMaintParams WorkerThreadParam;
    char filename[] = "mc0.bin";
    u32 CurrentCPUTicks, PreviousCPUTicks, PadStatus;
    unsigned char seconds, ClusterSize;
    int result, WorkerThreadState, InitSemaID;
    FILE *file;
    unsigned int ClusterSizeBytes, TimeElasped, rate, TotalSizeToTransferKB;
    float PercentageComplete;
    struct MCTools_McSpecData McSpecData;

    // Reboot the IOP to load MCTOOLS.IRX in. This will also prevent MCMAN's cache from storing outdated content.
    InitSemaID = IopInitStart(IOP_MOD_MCTOOLS | IOP_REBOOT);
    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    // Call sceMcGetInfo on the memory card to be accessed, after IOP reboots. Otherwise I/O operations may fail.
    if ((result = InitMCInfo(port, slot)) != 0)
        return result;

    if ((result = MCToolsGetMCInfo(port, slot, &McSpecData)) < 0) {
        DEBUG_PRINTF("MCToolsGetMCInfo() failed with error %d\n", result);
        return result;
    }

    WaitSema(InstallLockSema);

    MCToolsFlushMCMANClusterCache(port, slot);

    ClusterSize = (McSpecData.CardSize % 32 != 0) ? 2 : 32; // Sony wants to assume that the cluster size is 2, but try 32 to speed things up.
    ClusterSizeBytes = McSpecData.PageSize * ClusterSize;

    filename[2] = '0' + port;
    if ((file = fopen(filename, "wb")) == NULL) {
        DEBUG_PRINTF("Error creating file %s\n", filename);
        result = -errno;
    } else {
        InitProgressScreen(SYS_UI_LBL_DUMPING_MC);
        // Draw the progress screen once, so that loading the font will not compete with the actual dumping operation.
        DrawMemoryCardDumpingProgressScreen(0, 0, 0);

        TimeElasped = 0;
        PreviousCPUTicks = cpu_ticks();
        TotalSizeToTransferKB = McSpecData.CardSize * McSpecData.PageSize / 1024;

        WorkerThreadParam.port = port;
        WorkerThreadParam.slot = slot;
        WorkerThreadParam.file = file;
        WorkerThreadParam.ClusterSize = ClusterSize;
        WorkerThreadParam.McSpecData = &McSpecData;
        SendWorkerThreadCommand(WORKER_THREAD_CMD_DUMP_MC, &WorkerThreadParam);

        do {
            WorkerThreadState = GetWorkerThreadState();
            PercentageComplete = GetWorkerThreadProgress();

            PadStatus = ReadCombinedPadStatus();
            if (PadStatus & CancelButton) {
                if (DisplayPromptMessage(SYS_UI_MSG_QUIT_DUMPING, SYS_UI_LBL_NO, SYS_UI_LBL_YES) == 2) {
                    SendWorkerThreadCommand(WORKER_THREAD_CMD_STOP, NULL);
                    result = 1;
                    break;
                }
            }

            CurrentCPUTicks = cpu_ticks();
            if ((seconds = (CurrentCPUTicks > PreviousCPUTicks ? CurrentCPUTicks - PreviousCPUTicks : UINT_MAX - PreviousCPUTicks + CurrentCPUTicks) / 295000000) > 0) {
                TimeElasped += seconds;
                PreviousCPUTicks = CurrentCPUTicks;
            }

            rate = (TimeElasped > 0) ? PercentageComplete * TotalSizeToTransferKB / TimeElasped : 0;
            DrawMemoryCardDumpingProgressScreen(PercentageComplete, rate, rate > 0 ? ((1.0f - PercentageComplete) * TotalSizeToTransferKB) / rate : UINT_MAX);
        } while (WorkerThreadState == WORKER_THREAD_RES_CMD_OK || WorkerThreadState == WORKER_THREAD_RES_BSY);

        result = (WorkerThreadState == WORKER_THREAD_RES_OK) ? 0 : WorkerThreadState;

        fclose(file);
    }

    SignalSema(InstallLockSema);

    // Reboot the IOP to allow the remainder of the program to continue.
    InitSemaID = IopInitStart(IOP_MOD_SET_MAIN | IOP_REBOOT);
    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    return result;
}

int PerformMemoryCardRestore(int port, int slot)
{
    struct WorkerThreadMcMaintParams WorkerThreadParam;
    int result, WorkerThreadState;
    FILE *file;
    char filename[] = "mc0.bin";
    unsigned char seconds;
    unsigned int TimeElasped, rate, TotalSizeToTransferKB;
    u32 CurrentCPUTicks, PreviousCPUTicks, PadStatus;
    float PercentageComplete;
    struct MCTools_McSpecData McSpecData;
    int InitSemaID;

    result = 0;
    // Reboot the IOP to load MCTOOLS.IRX in. This will also prevent MCMAN's cache from storing outdated content.
    InitSemaID = IopInitStart(IOP_MOD_MCTOOLS | IOP_REBOOT);
    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    // Call sceMcGetInfo on the memory card to be accessed, after IOP reboots. Otherwise I/O operations may fail.
    if ((result = InitMCInfo(port, slot)) != 0)
        return result;

    if ((result = MCToolsGetMCInfo(port, slot, &McSpecData)) < 0) {
        DEBUG_PRINTF("MCToolsGetMCInfo() failed with error %d\n", result);
        return result;
    }

    MCToolsFlushMCMANClusterCache(port, slot);

    WaitSema(InstallLockSema);

    filename[2] = '0' + port;
    if ((file = fopen(filename, "rb")) == NULL) {
        DEBUG_PRINTF("Error opening file %s\n", filename);
        result = -errno;
    } else {
        InitProgressScreen(SYS_UI_LBL_RESTORING_MC);
        // Draw the progress screen once, so that loading the font will not compete with the actual restore operation.
        DrawMemoryCardRestoreProgressScreen(0, 0, 0);

        TimeElasped = 0;
        PreviousCPUTicks = cpu_ticks();
        TotalSizeToTransferKB = McSpecData.CardSize * McSpecData.PageSize / 1024;

        WorkerThreadParam.port = port;
        WorkerThreadParam.slot = slot;
        WorkerThreadParam.file = file;
        WorkerThreadParam.McSpecData = &McSpecData;
        SendWorkerThreadCommand(WORKER_THREAD_CMD_RESTORE_MC, &WorkerThreadParam);

        do {
            WorkerThreadState = GetWorkerThreadState();
            PercentageComplete = GetWorkerThreadProgress();

            PadStatus = ReadCombinedPadStatus();
            if (PadStatus & CancelButton) {
                if (DisplayPromptMessage(SYS_UI_MSG_QUIT_RESTORING, SYS_UI_LBL_NO, SYS_UI_LBL_YES) == 2) {
                    SendWorkerThreadCommand(WORKER_THREAD_CMD_STOP, NULL);
                    result = 1;
                    break;
                }
            }

            CurrentCPUTicks = cpu_ticks();
            if ((seconds = (CurrentCPUTicks > PreviousCPUTicks ? CurrentCPUTicks - PreviousCPUTicks : UINT_MAX - PreviousCPUTicks + CurrentCPUTicks) / 295000000) > 0) {
                TimeElasped += seconds;
                PreviousCPUTicks = CurrentCPUTicks;
            }

            rate = (TimeElasped > 0) ? PercentageComplete * TotalSizeToTransferKB / TimeElasped : 0;
            DrawMemoryCardRestoreProgressScreen(PercentageComplete, rate, rate > 0 ? ((1.0f - PercentageComplete) * TotalSizeToTransferKB) / rate : UINT_MAX);
        } while (WorkerThreadState == WORKER_THREAD_RES_CMD_OK || WorkerThreadState == WORKER_THREAD_RES_BSY);

        result = (WorkerThreadState == WORKER_THREAD_RES_OK) ? 0 : WorkerThreadState;

        fclose(file);
    }

    SignalSema(InstallLockSema);

    // Reboot the IOP to allow the remainder of the program to continue.
    InitSemaID = IopInitStart(IOP_MOD_SET_MAIN | IOP_REBOOT);
    WaitSema(InitSemaID);
    DeleteSema(InitSemaID);

    ReinitializeUI();

    return result;
}

static int WorkerThreadID = -1;
static volatile int WorkerThreadResultCode;
static int WorkerThreadCmd;
static const void *WorkerThreadArg;
static void *WorkerThreadStack;
static volatile int MainThreadID;
static volatile float WorkerThreadProgress;

static void SetWorkerThreadCommand(int command, const void *arg)
{
    WorkerThreadCmd = command;
    WorkerThreadArg = arg;
}

static int GetWorkerThreadCommand(const void **arg)
{
    if (arg != NULL)
        *arg = WorkerThreadArg;
    return WorkerThreadCmd;
}

static int DumpMemoryCard(int port, int slot, FILE *file, unsigned short int PagesPerCluster, const struct MCTools_McSpecData *McSpecData)
{
    void *buffer1, *buffer2;
    unsigned char ClusterBank;
    int result;
    unsigned short int ClusterSizeBytes;
    unsigned int cluster, NumClusters;

    DEBUG_PRINTF("DumpMemoryCard: slot: %d, port: %d, ClusterSize: %d, PageSize: %u, CardSize: %u\n", port, slot, PagesPerCluster, McSpecData->PageSize, McSpecData->CardSize);

    result = 0;
    NumClusters = McSpecData->CardSize / PagesPerCluster;
    ClusterSizeBytes = PagesPerCluster * McSpecData->PageSize;
    if (((buffer1 = memalign(64, ClusterSizeBytes)) != NULL) && ((buffer2 = memalign(64, ClusterSizeBytes)) != NULL)) {
        ClusterBank = 0;
        for (cluster = 0; cluster < NumClusters; cluster++) {
            WorkerThreadProgress = (float)cluster / NumClusters;

            if (GetWorkerThreadCommand(NULL) == WORKER_THREAD_CMD_STOP) {
                break;
            }

            if ((result = MCToolsReadCluster(port, slot, cluster, PagesPerCluster, McSpecData, (ClusterBank == 0) ? buffer1 : buffer2)) < 0) {
                DEBUG_PRINTF("MCToolsReadCluster() RPC send failed with error %d\n", result);
                result = ERROR_SIDE_SRC | -EIO;
                break;
            }

            /* Do not start writing data on the first pass. Let both buffers fill up with data first or an empty buffer might end up getting written instead. */
            if (cluster > 0) {
                if (fwrite((ClusterBank == 1) ? buffer1 : buffer2, 1, ClusterSizeBytes, file) != ClusterSizeBytes) {
                    DEBUG_PRINTF("Error writing to dump target.\n");
                    result = ERROR_SIDE_DST | -EIO;
                    break;
                }
            }

            MCToolsSync(0);

            if ((result = MCToolsAsyncGetLastError()) < 0) {
                DEBUG_PRINTF("MCToolsReadCluster() failed with error %d\n", result);
                result = ERROR_SIDE_SRC | -EIO;
                break;
            }

            ClusterBank = (ClusterBank == 0) ? 1 : 0; /* Swap banks */
        }

        /* There is one cluster left not written when the loop above ends */
        if (result >= 0) {
            if (fwrite((ClusterBank == 1) ? buffer1 : buffer2, 1, ClusterSizeBytes, file) != ClusterSizeBytes) {
                DEBUG_PRINTF("Error writing to dump target.\n");
                result = ERROR_SIDE_DST | -EIO;
            }
        }

        free(buffer1);
        free(buffer2);
    } else {
        result = -ENOMEM;

        if (buffer1 != NULL)
            free(buffer1);

        /*	If buffer1 is NULL, that means that buffer2 will be NULL as allocation wouldn't have even taken place for buffer 2.
         *	If buffer1 is not NULL, that means that buffer2 will be NULL as allocation has failed for at least one buffer - and that buffer is buffer2.
         */
    }

    if (GetWorkerThreadCommand(NULL) == WORKER_THREAD_CMD_STOP) {
        SetWorkerThreadCommand(WORKER_THREAD_CMD_NONE, NULL);
        WakeupThread(MainThreadID);
        WorkerThreadResultCode = WORKER_THREAD_RES_ABRT;
    }

    return result;
}

static int RestoreMemoryCard(int port, int slot, FILE *file, const struct MCTools_McSpecData *McSpecData)
{
    void *buffer1, *buffer2;
    int result;
    unsigned char BlockBank;
    unsigned int block, BlockSize, NumBlocks;

    DEBUG_PRINTF("RestoreMemoryCard: slot: %d, port: %d, PageSize: %u, CardSize: %u, BlockSize: %u\n", port, slot, McSpecData->PageSize, McSpecData->CardSize, McSpecData->BlockSize);

    result = 0;
    BlockSize = McSpecData->PageSize * McSpecData->BlockSize;
    if (((buffer1 = memalign(64, BlockSize)) != NULL) && ((buffer2 = memalign(64, BlockSize)) != NULL)) {
        BlockBank = 0;
        NumBlocks = McSpecData->CardSize / McSpecData->BlockSize;
        for (block = 0; block < NumBlocks; block++) {
            WorkerThreadProgress = (float)block / NumBlocks;

            if (GetWorkerThreadCommand(NULL) == WORKER_THREAD_CMD_STOP) {
                break;
            }

            if (fread((BlockBank == 0) ? buffer1 : buffer2, 1, BlockSize, file) != BlockSize) {
                DEBUG_PRINTF("Error reading from restoration source.\n");
                result = ERROR_SIDE_SRC | -EIO;
                break;
            }

            MCToolsSync(0);

            if ((result = MCToolsAsyncGetLastError()) < 0) {
                DEBUG_PRINTF("MCToolsWriteBlock() failed with error %d\n", result);
                result = ERROR_SIDE_DST | -EIO;
                break;
            }

            if ((result = MCToolsWriteBlock(port, slot, block, McSpecData, (BlockBank == 0) ? buffer1 : buffer2)) < 0) {
                DEBUG_PRINTF("MCToolsWriteBlock() RPC send failed with error %d\n", result);
                result = ERROR_SIDE_DST | -EIO;
                break;
            }

            BlockBank = (BlockBank == 0) ? 1 : 0; /* Swap banks */
        }

        MCToolsSync(0);

        if (result >= 0) {
            if ((result = MCToolsAsyncGetLastError()) < 0) {
                DEBUG_PRINTF("MCToolsWriteBlock() failed with error %d\n", result);
                result = ERROR_SIDE_DST | -EIO;
            }
        }

        free(buffer1);
        free(buffer2);
    } else {
        result = -ENOMEM;

        if (buffer1 != NULL)
            free(buffer1);

        /*	If buffer1 is NULL, that means that buffer2 will be NULL as allocation wouldn't have even taken place for buffer 2.
         *	If buffer1 is not NULL, that means that buffer2 will be NULL as allocation has failed for at least one buffer - and that buffer is buffer2.
         */
    }

    if (GetWorkerThreadCommand(NULL) == WORKER_THREAD_CMD_STOP) {
        SetWorkerThreadCommand(WORKER_THREAD_CMD_NONE, NULL);
        WakeupThread(MainThreadID);
        WorkerThreadResultCode = WORKER_THREAD_RES_ABRT;
    }

    return result;
}

static void WorkerThread(void *arg)
{
    int result, command;
    const void *CommandArgs;

    while (1) {
        command = GetWorkerThreadCommand(&CommandArgs);

        switch (command) {
            case WORKER_THREAD_CMD_NONE:
                SleepThread();
                break;
            case WORKER_THREAD_CMD_DUMP_MC:
                SetWorkerThreadCommand(WORKER_THREAD_CMD_NONE, NULL);
                WorkerThreadProgress = 0;
                WorkerThreadResultCode = WORKER_THREAD_RES_CMD_OK;
                WakeupThread(MainThreadID);
                SleepThread();
                WorkerThreadResultCode = WORKER_THREAD_RES_BSY;
                result = DumpMemoryCard(((struct WorkerThreadMcMaintParams *)CommandArgs)->port, ((struct WorkerThreadMcMaintParams *)CommandArgs)->slot, ((struct WorkerThreadMcMaintParams *)CommandArgs)->file, ((struct WorkerThreadMcMaintParams *)CommandArgs)->ClusterSize, ((struct WorkerThreadMcMaintParams *)CommandArgs)->McSpecData);
                WorkerThreadResultCode = result == 0 ? WORKER_THREAD_RES_OK : result;
                break;
            case WORKER_THREAD_CMD_RESTORE_MC:
                SetWorkerThreadCommand(WORKER_THREAD_CMD_NONE, NULL);
                WorkerThreadProgress = 0;
                WorkerThreadResultCode = WORKER_THREAD_RES_CMD_OK;
                WakeupThread(MainThreadID);
                SleepThread();
                WorkerThreadResultCode = WORKER_THREAD_RES_BSY;
                result = RestoreMemoryCard(((struct WorkerThreadMcMaintParams *)CommandArgs)->port, ((struct WorkerThreadMcMaintParams *)CommandArgs)->slot, ((struct WorkerThreadMcMaintParams *)CommandArgs)->file, ((struct WorkerThreadMcMaintParams *)CommandArgs)->McSpecData);
                WorkerThreadResultCode = result == 0 ? WORKER_THREAD_RES_OK : result;
                break;
            default:
                SetWorkerThreadCommand(WORKER_THREAD_CMD_NONE, NULL);
                WorkerThreadResultCode = -1;
                WakeupThread(MainThreadID);
                SleepThread();
        }
    }
}

int StartWorkerThread(void)
{
    SetWorkerThreadCommand(WORKER_THREAD_CMD_NONE, NULL);
    WorkerThreadResultCode = WORKER_THREAD_RES_OK;
    MainThreadID = GetThreadId();
    /* 8KB, not the original 2KB: the MC dump/restore routines run on this thread
       and call DEBUG_PRINTF(), which on the modern SDK goes through our sio_printf
       shim (buffer + newlib vsnprintf) and needs far more stack than ps2sdk's old
       sio_printf did. 2KB overflowed and froze dump/restore; the main thread has
       8KB (see linkfile _stack_size) and handles the same calls fine. */
    WorkerThreadStack = memalign(128, 0x2000);
    return (WorkerThreadID = SysCreateThread(&WorkerThread, WorkerThreadStack, 0x2000, NULL, 0x70));
}

void StopWorkerThread(void)
{
    if (WorkerThreadID >= 0) {
        SendWorkerThreadCommand(WORKER_THREAD_CMD_STOP, NULL);
        TerminateThread(WorkerThreadID);
        DeleteThread(WorkerThreadID);
        WorkerThreadID = -1;
    }
}

int SendWorkerThreadCommand(int command, const void *arg)
{
    int result;

    SetWorkerThreadCommand(command, arg);
    WakeupThread(WorkerThreadID);
    SleepThread();
    result = GetWorkerThreadState();
    WakeupThread(WorkerThreadID);

    return result;
}

int GetWorkerThreadState(void)
{
    return WorkerThreadResultCode;
}

float GetWorkerThreadProgress(void)
{
    return WorkerThreadProgress;
}

int IsUnsupportedModel(void)
{
    FILE *file;
    int result;
    char versionStr[5];
    unsigned int version;

    result = 0;
    if ((file = fopen("rom0:ROMVER", "r")) != NULL) {
        if (fread(versionStr, 1, 4, file) == 4) {
            versionStr[4] = '\0';
            version = (unsigned int)strtoul(versionStr, NULL, 16);
            result = version >= 0x230;
        }

        fclose(file);
    }

    return result;
}
int IsRareModel(void)
{
    FILE *file;
    int result = 0;
    char versionStr[5];
    unsigned int version;

    result = 0;
    if ((file = fopen("rom0:ROMVER", "r")) != NULL) {
        if (fread(versionStr, 1, 4, file) == 4) {
            versionStr[4] = '\0';
            version = (unsigned int)strtoul(versionStr, NULL, 16);
            if ((version == 0x180) || (version == 0x210))
                result = 1;
        }

        fclose(file);
    }

    return result;
}

int HDDCheckSMARTStatus(void)
{
    return (fileXioDevctl("hdd0:", APA_DEVCTL_SMART_STAT, NULL, 0, NULL, 0) != 0);
}

int HDDCheckSectorErrorStatus(void)
{
    return (fileXioDevctl("hdd0:", APA_DEVCTL_SMART_STAT, NULL, 0, NULL, 0) != 0);
}

int HDDCheckPartErrorStatus(void)
{
    return (fileXioDevctl("hdd0:", APA_DEVCTL_GET_ERROR_PART_NAME, NULL, 0, NULL, 0) != 0);
}

int HDDCheckHasSpace(unsigned int PartSize) // Partition size in MBytes
{
    iox_dirent_t dirent;
    int result, fd;
    u32 total, used, required, size;

    result = 0;
    used = 0;
    required = PartSize * (1024 * 1024 / 512);
    // Check if there is an empty partition that can be used. At the same time, calculate the amount of space used.
    if ((fd = fileXioDopen("hdd0:")) >= 0) {
        while (fileXioDread(fd, &dirent) > 0) {
            size = dirent.stat.size;
            used += size;

            if (dirent.stat.mode == APA_TYPE_FREE) {
                if (size >= required) { // There is an empty partition that can be used. No need to continue!
                    result = 1;
                    break;
                }
            }
        }

        fileXioDclose(fd);
    } else
        result = fd;

    // No? Check unused areas.
    if (result == 0) {
        total = fileXioDevctl("hdd0:", APA_DEVCTL_TOTAL_SECTORS, NULL, 0, NULL, 0);
        result = ((used % required) == 0 && (used + required < total));
    }

    return result;
}

int HDDCheckStatus(void)
{
    int status;

    status = fileXioDevctl("hdd0:", HDIOC_STATUS, NULL, 0, NULL, 0);

    if (status == 0)
        fileXioRemove("hdd0:_tmp"); // Remove _tmp, if it exists.

    return status;
}

void poweroffCallback(void *arg)
{ // Power button was pressed. If no installation is in progress, begin shutdown of the PS2.
    if (PollSema(InstallLockSema) == InstallLockSema) {
        // If dev9.irx was loaded successfully, shut down DEV9.
        if (dev9Loaded) {
            fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);
            while (fileXioDevctl("dev9x:", DDIOC_OFF, NULL, 0, NULL, 0) < 0) {};
        }

        // As required by some (typically 2.5") HDDs, issue the SCSI STOP UNIT command to avoid causing an emergency park.
        fileXioDevctl("mass:", USBMASS_DEVCTL_STOP_ALL, NULL, 0, NULL, 0);

        /* Power-off the PlayStation 2 console. */
        poweroffShutdown();
    }
}
