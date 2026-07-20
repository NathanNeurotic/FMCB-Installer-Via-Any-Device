EE_BIN = $(EE_BIN_DIR)UNC_FMCBInstaller.elf
EE_BIN_PACKED = $(EE_BIN_DIR)FMCBInstaller.elf
#IOP modules
EE_IOP_OBJS = IOPRP_img.o IOMANX_irx.o FILEXIO_irx.o SIO2MAN_irx.o PADMAN_irx.o MCMAN_irx.o MCSERV_irx.o MMCEMAN_irx.o SECRSIF_irx.o MCTOOLS_irx.o USBD_irx.o POWEROFF_irx.o DEV9_irx.o ATAD_irx.o HDD_irx.o PFS_irx.o
IRX_DIR = irx/compiled

EE_RES_OBJS = background.o buttons.o
EE_OBJS = main.o iop.o UI.o menu.o libsecr.o pad.o system.o graphics.o ReqSpaceCalc.o font.o $(EE_RES_OBJS) $(EE_IOP_OBJS) mctools_rpc.o

EE_INCS := -I$(PS2SDK)/ports/include -I$(PS2SDK)/ports/include/freetype2 -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I./irx/source/secrsif/src -I./irx/source/mctools/src/
EE_LDFLAGS := -L$(PS2SDK)/ports/lib -L$(PS2SDK)/ee/lib -L$(PS2DEV)/ee/ee/lib -Tlinkfile -s
EE_LIBS = -lgs -lpng -lz -lm -lfreetype -lpoweroff -lcdvd -lmc -lpadx -lhdd -lfileXio -lpatches -liopreboot -lc -lkernel
# Modern ps2sdk (unified toolchain) needs -D_EE and -DNEWLIB_PORT_AWARE (this code
# uses fileXio directly), and uses -G0 instead of the old small-data -mgpopt/-G8192.
EE_CFLAGS += -D_EE -G0 -DNEWLIB_PORT_AWARE -include sioprintf.h -Os

# Single "any device" build: embed BOTH USB filesystem stacks. iop.c loads only
# one at runtime, chosen from the launch path -- the BDM stack (FAT32 + exFAT,
# plus MX4SIO/iLink) by default, or the legacy usbhdfsd driver for bare "mass:".
EE_OBJS += usbmass_bd_irx.o bdm_irx.o bdmfs_fatfs_irx.o USBHDFSD_irx.o

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

# Modern ps2sdk no longer ships a prebuilt crt0.o; the toolchain links its own
# crt0 (entry __start, see linkfile), so we drop -nostartfiles / the explicit crt0.o.
$(EE_BIN) : $(EE_OBJS)
	$(EE_CC) $(EE_CFLAGS) $(EE_LDFLAGS) -o $(EE_BIN) $(EE_OBJS) $(EE_LIBS)

all: $(EE_BIN)

$(EE_BIN_PACKED): $(EE_BIN)
	@echo " -- Compressing"
	ps2-packer -v $< $@

clean:
	rm -f $(EE_BIN) $(EE_BIN_REL) $(EE_OBJS) *_irx.c background.c buttons.c IOPRP_img.c

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

usbmass_bd_irx.c: $(IRX_DIR)/usbmass_bd.irx
	bin2c $< $@ usbmass_bd_irx

bdm_irx.c: $(IRX_DIR)/bdm.irx
	bin2c $< $@ bdm_irx

bdmfs_fatfs_irx.c: $(IRX_DIR)/bdmfs_fatfs.irx
	bin2c $< $@ bdmfs_fatfs_irx

USBHDFSD_irx.c: $(PS2SDK)/iop/irx/usbhdfsd.irx
	bin2c $< $@ USBHDFSD_irx

IOPRP_img.c: $(IRX_DIR)/IOPRP.img
	bin2c $< $@ IOPRP_img

ATAD_irx.c: $(PS2SDK)/iop/irx/ps2atad.irx
	bin2c $< $@ ATAD_irx

HDD_irx.c: $(PS2SDK)/iop/irx/ps2hdd-osd.irx
	bin2c $< $@ HDD_irx

PFS_irx.c: $(PS2SDK)/iop/irx/ps2fs.irx
	bin2c $< $@ PFS_irx

include $(PS2SDK)/Defs.make