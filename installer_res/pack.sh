
PKG_DATE=$(date '+[%Y-%m-%d]')
cp ../Changelog.md __base/Changelog.md

# One package: the payload is split by destination (INSTALL/MC, INSTALL/HDD,
# INSTALL/SYSTEM), not by FreeMcBoot version.
NEWDIR="FMCBinst-ISR-RIP-$PKG_DATE"
echo "packing $NEWDIR into ../FMCB-ISR-RIP.7z"

cp -r __base/ $NEWDIR/
cp -r INSTALL/ $NEWDIR/INSTALL/
echo $SHA8>$NEWDIR/lang/commit.txt
echo "title=FMCB/FHDB Installer ISR-RIP $PKG_DATE" >$NEWDIR/title.cfg
echo "boot=FMCBInstaller.elf">>$NEWDIR/title.cfg
cp FMCBInstaller.elf $NEWDIR/

7z a -t7z -r ../FMCB-ISR-RIP.7z $NEWDIR/*
