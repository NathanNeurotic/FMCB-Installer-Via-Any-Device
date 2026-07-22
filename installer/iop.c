#include <iopcontrol.h>
#include <iopcontrol_special.h>
#include <iopheap.h>
#include <kernel.h>
#include <libcdvd.h>
#include <libmc.h>
#include <libpwroff.h>
#include <loadfile.h>
#include <libpad.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <fileXio_rpc.h>

#include "main.h"
#include "iop.h"
#include "system.h"
#include "pad.h"
#include "mctools_rpc.h"
#include "libsecr.h"
#include <unistd.h>
#define IMPORT_IRX(_IRX) \
extern unsigned char _IRX[]; \
extern unsigned int size_##_IRX;

IMPORT_IRX(IOMANX_irx);
IMPORT_IRX(FILEXIO_irx);
IMPORT_IRX(SIO2MAN_irx);
IMPORT_IRX(PADMAN_irx);
IMPORT_IRX(MCMAN_irx);
IMPORT_IRX(MCSERV_irx);
IMPORT_IRX(MMCEMAN_irx);
IMPORT_IRX(SECRSIF_irx);
IMPORT_IRX(MCTOOLS_irx);
IMPORT_IRX(USBD_irx);
/* All storage backends are embedded; at runtime we load ONLY the one matching
   the device we were launched from (see LoadBootDeviceModules). This keeps a
   single build compatible with any launcher/device the way wLaunchELF-R3Z is,
   without loading every backend at once (e.g. MX4SIO must NOT probe the memory
   card slot during a normal MC install).
     BDM core:  bdm + bdmfs_fatfs (FAT32/exFAT, registers "massN:" + typed names)
     backends:  usbmass_bd (USB), mx4sio_bd (MX4SIO SD), ata_bd (ATA)
     legacy:    usbhdfsd (bare "mass:" from old usbhdfsd launchers)
     others:    mmceman (SD2PSX/MemCard PRO SD), cdfs (disc) */
IMPORT_IRX(usbmass_bd_irx);
IMPORT_IRX(mx4sio_bd_irx);
IMPORT_IRX(ata_bd_irx);
IMPORT_IRX(bdm_irx);
IMPORT_IRX(bdmfs_fatfs_irx);
IMPORT_IRX(USBHDFSD_irx);
IMPORT_IRX(CDFS_irx);
IMPORT_IRX(POWEROFF_irx);
IMPORT_IRX(DEV9_irx);
IMPORT_IRX(ATAD_irx);
IMPORT_IRX(HDD_irx);
IMPORT_IRX(PFS_irx);
IMPORT_IRX(IOPRP_img);

u8 dev9Loaded;

/* 8KB (was 4KB): this thread runs SecrInit()/InitMCTOOLS(), which call
   DEBUG_PRINTF() -> our sio_printf shim (buffer + newlib vsnprintf) and need more
   stack than ps2sdk's old sio_printf did. Same class of overflow that froze MC
   dump/restore on the worker thread. */
#define SYSTEM_INIT_THREAD_STACK_SIZE 0x2000

struct SystemInitParams
{
    int InitCompleteSema;
    unsigned int flags;
};

static void SystemInitThread(struct SystemInitParams *SystemInitParams)
{
    static const char PFS_args[] = "-n\0"
                                   "24\0"
                                   "-o\0"
                                   "8";
    /* R3Z uses tighter PFS limits when APA shares a drive with exFAT (ATA/APAJail). */
    static const char PFS_ata_args[] = "-m\0"
                                       "2\0"
                                       "-o\0"
                                       "10\0"
                                       "-n\0"
                                       "40";
    int i;

    if (SystemInitParams->flags & IOP_MOD_HDD) {
        if (GetBootDeviceID() == BOOT_DEVICE_ATA) {
            /* ATA boot (APAJail): ata_bd (loaded in LoadBootDeviceModules) already
               provides BOTH the atad interface that ps2hdd imports AND the BDM block
               device for the exFAT payload, so ps2hdd rides ata_bd -- no ps2atad.
               This lets the exFAT payload (bdmfs_fatfs) and the APA install (ps2hdd)
               share one ATA driver on the same drive, exactly as wLaunchELF-R3Z does,
               so ATA-booted users can still install FreeHdBoot to the APA side. */
            SifExecModuleBuffer(HDD_irx, size_HDD_irx, 0, NULL, NULL);
            SifExecModuleBuffer(PFS_irx, size_PFS_irx, sizeof(PFS_ata_args), PFS_ata_args, NULL);
        } else if (SifExecModuleBuffer(ATAD_irx, size_ATAD_irx, 0, NULL, NULL) >= 0) {
            SifExecModuleBuffer(HDD_irx, size_HDD_irx, 0, NULL, NULL);
            SifExecModuleBuffer(PFS_irx, size_PFS_irx, sizeof(PFS_args), PFS_args, NULL);
        }
    }

    if (SystemInitParams->flags & IOP_MOD_SECRSIF) {
        SifExecModuleBuffer(SECRSIF_irx, size_SECRSIF_irx, 0, NULL, NULL);
        SecrInit();
    }

    if (SystemInitParams->flags & IOP_MOD_MCTOOLS) {
        SifExecModuleBuffer(MCTOOLS_irx, size_MCTOOLS_irx, 0, NULL, NULL);
        InitMCTOOLS();
    }

    SifExitIopHeap();
    SifLoadFileExit();

    SignalSema(SystemInitParams->InitCompleteSema);
    ExitDeleteThread();
}

/* Load the single storage backend needed to read the INSTALL/ payload from the
   device we were launched from. Only one BDM backend is ever loaded, so the boot
   device is unambiguously "massN:" (and its typed alias); GetInstallRootPath()
   normalizes the payload path accordingly. */
static void LoadBootDeviceModules(void)
{
    switch (GetBootDeviceID()) {
        case BOOT_DEVICE_MASS:
            if (IsLegacyMassBoot()) {
                // Bare "mass:" -> legacy usbhdfsd (FAT only).
                SifExecModuleBuffer(USBD_irx, size_USBD_irx, 0, NULL, NULL);
                SifExecModuleBuffer(USBHDFSD_irx, size_USBHDFSD_irx, 0, NULL, NULL);
            } else {
                // "massN:"/"usbN:" -> BDM USB (FAT32 + exFAT). This is the exact
                // stack + load order (bdm/bdmfs before usbd, then usbmass_bd) the
                // shipping exFAT installer used; loading usbd first / using the SDK
                // BDM modules black-screened on real hardware.
                SifExecModuleBuffer(bdm_irx, size_bdm_irx, 0, NULL, NULL);
                SifExecModuleBuffer(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, NULL);
                SifExecModuleBuffer(USBD_irx, size_USBD_irx, 0, NULL, NULL);
                SifExecModuleBuffer(usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL, NULL);
            }
            break;
        case BOOT_DEVICE_MX4SIO:
            // MX4SIO SD (SPI SD in the MC slot) -> BDM core + R3Z's matched
            // mx4sio_bd (bdm/bdmfs_fatfs are byte-identical to R3Z's).
            SifExecModuleBuffer(bdm_irx, size_bdm_irx, 0, NULL, NULL);
            SifExecModuleBuffer(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, NULL);
            SifExecModuleBuffer(mx4sio_bd_irx, size_mx4sio_bd_irx, 0, NULL, NULL);
            break;
        case BOOT_DEVICE_ATA:
            // FAT/exFAT partition on the ATA bus -> BDM core + ata_bd (dev9 is
            // already resident from IopInitStart). ata_bd owns the ATA controller,
            // so we skip the ps2atad/APA HDD-install stack on an ATA boot (see
            // main.c) to avoid a two-drivers-on-one-controller clash. sleep(1)
            // mirrors R3Z's settle after ata_bd before it is used.
            SifExecModuleBuffer(bdm_irx, size_bdm_irx, 0, NULL, NULL);
            SifExecModuleBuffer(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, NULL);
            SifExecModuleBuffer(ata_bd_irx, size_ata_bd_irx, 0, NULL, NULL);
            sleep(1);
            break;
        case BOOT_DEVICE_MMCE:
            // SD2PSX / MemCard PRO SD (mmce0:). Rides SIO2 via freesio2 and coexists
            // with mcman; loaded only for an MMCE boot so it never clashes with
            // MX4SIO nor probes a real MC during an MC install.
            SifExecModuleBuffer(MMCEMAN_irx, size_MMCEMAN_irx, 0, NULL, NULL);
            break;
        case BOOT_DEVICE_CDFS:
            // Disc (cdfs:).
            SifExecModuleBuffer(CDFS_irx, size_CDFS_irx, 0, NULL, NULL);
            break;
        case BOOT_DEVICE_MC:
        case BOOT_DEVICE_HDD:
        case BOOT_DEVICE_HOST:
        default:
            // mc:/hdd:/pfs: already available; host: is provided by the emulator.
            break;
    }
}

int IopInitStart(unsigned int flags)
{
    ee_sema_t sema;
    static struct SystemInitParams InitThreadParams;
    static unsigned char SysInitThreadStack[SYSTEM_INIT_THREAD_STACK_SIZE] __attribute__((aligned(16)));
    int stat, ret;

    if (!(flags & IOP_REBOOT)) {
        SifInitRpc(0);
    } else {
        PadDeinitPads();
        sceCdInit(SCECdEXIT);
        DeinitMCTOOLS();
        SecrDeinit();
        fileXioExit();
    }

    if (!(flags & IOP_LIBSECR_IMG)) {
        SifIopReset("", 0);
    } else {
        SifIopRebootBuffer(IOPRP_img, size_IOPRP_img);
    }

    // Do something useful while the IOP resets.
    sema.init_count = 0;
    sema.max_count = 1;
    sema.attr = sema.option = 0;
    InitThreadParams.InitCompleteSema = CreateSema(&sema);
    InitThreadParams.flags = flags;

    while (!SifIopSync()) {};

    SifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();

    sbv_patch_enable_lmb();

    SifExecModuleBuffer(IOMANX_irx, size_IOMANX_irx, 0, NULL, NULL);
    SifExecModuleBuffer(FILEXIO_irx, size_FILEXIO_irx, 0, NULL, NULL);

    fileXioInit();
    sceCdInit(SCECdINoD);

    SifExecModuleBuffer(POWEROFF_irx, size_POWEROFF_irx, 0, NULL, NULL);
    ret = SifExecModuleBuffer(DEV9_irx, size_DEV9_irx, 0, NULL, &stat);
    dev9Loaded = (ret >= 0 && stat == 0); // dev9.irx must have loaded successfully and returned RESIDENT END.

    SifExecModuleBuffer(SIO2MAN_irx, size_SIO2MAN_irx, 0, NULL, NULL);
    SifExecModuleBuffer(PADMAN_irx, size_PADMAN_irx, 0, NULL, NULL);
    SifExecModuleBuffer(MCMAN_irx, size_MCMAN_irx, 0, NULL, NULL);
    SifExecModuleBuffer(MCSERV_irx, size_MCSERV_irx, 0, NULL, NULL);

    /* Bring up ONLY the storage backend for the device we booted from (from
       argv[0]/cwd), the way wLaunchELF-R3Z does, so the INSTALL/ payload is
       readable regardless of launcher/device. The memory card (mc:) and internal
       HDD (hdd:/pfs:, loaded in SystemInitThread) are the install *targets* and
       are always available; this only adds the driver to READ the payload. */
    LoadBootDeviceModules();
    sleep(5);

    SysCreateThread(SystemInitThread, SysInitThreadStack, SYSTEM_INIT_THREAD_STACK_SIZE, &InitThreadParams, 0x2);

    poweroffInit();
    poweroffSetCallback(&poweroffCallback, NULL);
    mcInit(MC_TYPE_XMC);
    PadInitPads();

    return InitThreadParams.InitCompleteSema;
}

void IopDeinit(void)
{
    PadDeinitPads();
    sceCdInit(SCECdEXIT);
    DeinitMCTOOLS();
    SecrDeinit();

    fileXioExit();
    SifExitRpc();
}
