EE_BIN = $(EE_BIN_DIR)UNC_FMCBInstaller.elf
EE_BIN_PACKED = $(EE_BIN_DIR)FMCBInstaller.elf
#IOP modules
EE_IOP_OBJS = IOPRP_img.o IOMANX_irx.o FILEXIO_irx.o SIO2MAN_irx.o PADMAN_irx.o MCMAN_irx.o MCSERV_irx.o MMCEMAN_irx.o SECRSIF_irx.o MCTOOLS_irx.o USBD_irx.o POWEROFF_irx.o DEV9_irx.o ATAD_irx.o HDD_irx.o PFS_irx.o
IRX_DIR = irx/compiled

EE_RES_OBJS = background.o buttons.o
EE_OBJS = main.o iop.o UI.o menu.o libsecr.o pad.o system_fhdb.o graphics.o ReqSpaceCalc.o font.o $(EE_RES_OBJS) $(EE_IOP_OBJS) mctools_rpc.o

EE_INCS := -I$(PS2SDK)/ports/include -I$(PS2SDK)/ports/include/freetype2 -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I./irx/source/secrsif/src -I./irx/source/mctools/src/
EE_LDFLAGS := -L$(PS2SDK)/ports/lib -L$(PS2SDK)/ee/lib -L$(PS2DEV)/ee/ee/lib -Tlinkfile -s
EE_LIBS = -lgs -lpng -lz -lm -lfreetype -lpoweroff -lcdvd -lmc -lpadx -lhdd -lfileXio -lpatches -liopreboot -lc -lkernel
# Modern ps2sdk (unified toolchain) needs -D_EE and -DNEWLIB_PORT_AWARE (this code
# uses fileXio directly), and uses -G0 instead of the old small-data -mgpopt/-G8192.
EE_CFLAGS += -D_EE -G0 -DNEWLIB_PORT_AWARE -include sioprintf.h -Os $(EXTRA_CFLAGS)

# Single "any device" build: embed the storage backends. iop.c loads only the one
# matching the launch path at runtime (R3Z-style): BDM core (bdm/bdmfs_fatfs) + one
# of usbmass_bd (USB) / mx4sio_bd (MX4SIO SD) / ata_bd (ATA), legacy usbhdfsd for
# bare "mass:", mmceman (EE_IOP_OBJS above) for SD2PSX/MemCard PRO, and cdfs for disc.
# The BDM core + usbmass_bd + mx4sio_bd modules are the wLaunchELF-R3Z matched set
# (bdm/bdmfs_fatfs/usbmass_bd are byte-identical to R3Z's), so MX4SIO works with the
# same core that boots USB; ata_bd rides that core exactly as R3Z pairs it.
EE_OBJS += bdm_irx.o bdmfs_fatfs_irx.o usbmass_bd_irx.o mx4sio_bd_irx.o ata_bd_irx.o USBHDFSD_irx.o CDFS_irx.o

ifeq ($(DEBUG),1)
    EE_CFLAGS += -DDEBUG_TTY_FEEDBACK
endif

ifdef FMCB_INSTALLER_VERSION
    EE_CFLAGS += -DFMCB_INSTALLER_VERSION=\"$(FMCB_INSTALLER_VERSION)\"
endif

%.o : %.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

%.o : %.S
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

%.o : %.s
	$(EE_AS) $(EE_ASFLAGS) $< -o $@

# Generate a reviewable FHDB-only replacement section from the large legacy
# system.c. The generator validates its source markers and fails if they drift.
system_fhdb.c: system.c tools/prepare-fhdb-system-source.py
	python3 tools/prepare-fhdb-system-source.py $< $@

# Modern ps2sdk no longer ships a prebuilt crt0.o; the toolchain links its own
# crt0 (entry __start, see linkfile), so we drop -nostartfiles / the explicit crt0.o.
$(EE_BIN) : $(EE_OBJS)
	$(EE_CC) $(EE_CFLAGS) $(EE_LDFLAGS) -o $(EE_BIN) $(EE_OBJS) $(EE_LIBS)

all: $(EE_BIN)

$(EE_BIN_PACKED): $(EE_BIN)
	@echo " -- Compressing"
	ps2-packer -v $< $@

clean:
	rm -f $(EE_BIN) $(EE_BIN_REL) $(EE_OBJS) system_fhdb.c *_irx.c background.c buttons.c IOPRP_img.c

rebuild: clean $(EE_BIN_PACKED)

background.c: resources/background.png
	bin2c $< $@ background

buttons.c: resources/buttons.png
	bin2c $< $@ buttons

POWEROFF_irx.c: $(PS2SDK)/iop/irx/poweroff.irx
	bin2c $< $@ POWEROFF_irx

DEV9_irx.c: $(PS2SDK)/iop/irx/ps2dev9.irx
	bin2c $< $@ DEV9_irx

IOMANX_irx.c: $(PS2SDK)/iop/irx/iomanX.irx
	bin2c $< $@ IOMANX_irx

FILEXIO_irx.c: $(PS2SDK)/iop/irx/fileXio.irx
	bin2c $< $@ FILEXIO_irx

SIO2MAN_irx.c: $(PS2SDK)/iop/irx/freesio2.irx
	bin2c $< $@ SIO2MAN_irx

PADMAN_irx.c: $(PS2SDK)/iop/irx/freepad.irx
	bin2c $< $@ PADMAN_irx

MCMAN_irx.c: $(PS2SDK)/iop/irx/mcman.irx
	bin2c $< $@ MCMAN_irx

MCSERV_irx.c: $(PS2SDK)/iop/irx/mcserv.irx
	bin2c $< $@ MCSERV_irx

MMCEMAN_irx.c: $(PS2SDK)/iop/irx/mmceman.irx
	bin2c $< $@ MMCEMAN_irx

SECRSIF_irx.c: $(IRX_DIR)/secrsif.irx
	bin2c $< $@ SECRSIF_irx

MCTOOLS_irx.c: $(IRX_DIR)/mctools.irx
	bin2c $< $@ MCTOOLS_irx

USBD_irx.c: $(PS2SDK)/iop/irx/usbd.irx
	bin2c $< $@ USBD_irx

# BDM USB stack pulled from the project's committed irx/compiled copies -- the
# exact modules the shipping exFAT installer used and that boot on real hardware.
# (The SDK copies black-screened; probing means we no longer need their typed names.)
bdm_irx.c: $(IRX_DIR)/bdm.irx
	bin2c $< $@ bdm_irx

bdmfs_fatfs_irx.c: $(IRX_DIR)/bdmfs_fatfs.irx
	bin2c $< $@ bdmfs_fatfs_irx

usbmass_bd_irx.c: $(IRX_DIR)/usbmass_bd.irx
	bin2c $< $@ usbmass_bd_irx

# mx4sio_bd comes from irx/compiled: it is wLaunchELF-R3Z's precompiled module,
# matched to our (= R3Z's, byte-identical) bdm/bdmfs_fatfs core -> hardware-proven.
mx4sio_bd_irx.c: $(IRX_DIR)/mx4sio_bd.irx
	bin2c $< $@ mx4sio_bd_irx

# ata_bd only exists in the SDK (R3Z pulls it from the SDK too) and rides the same
# committed BDM core, exactly as R3Z pairs them.
ata_bd_irx.c: $(PS2SDK)/iop/irx/ata_bd.irx
	bin2c $< $@ ata_bd_irx

USBHDFSD_irx.c: $(PS2SDK)/iop/irx/usbhdfsd.irx
	bin2c $< $@ USBHDFSD_irx

CDFS_irx.c: $(PS2SDK)/iop/irx/cdfs.irx
	bin2c $< $@ CDFS_irx

IOPRP_img.c: $(IRX_DIR)/IOPRP.img
	bin2c $< $@ IOPRP_img

ATAD_irx.c: $(PS2SDK)/iop/irx/ps2atad.irx
	bin2c $< $@ ATAD_irx

HDD_irx.c: $(PS2SDK)/iop/irx/ps2hdd-osd.irx
	bin2c $< $@ HDD_irx

PFS_irx.c: $(PS2SDK)/iop/irx/ps2fs.irx
	bin2c $< $@ PFS_irx

include $(PS2SDK)/Defs.make
