#!/usr/bin/env python3
"""Generate the FHDB-hardened system translation unit.

The upstream installer keeps the FHDB implementation in a very large legacy
system.c.  Keep the hardware-test patch reviewable by replacing only the FHDB
MBR/PFS I/O section at build time.  The script fails closed if the expected
source markers move, so an upstream edit cannot silently build an unpatched or
partially patched installer.
"""

from __future__ import annotations

import pathlib
import sys


START_MARKER = "/* Don't set this to be too large, as FILEXIO's RPC receive buffer is only about 0x4C00 bytes large */"
END_MARKER = "static int CreateBasicFoldersOnHDD"

REPLACEMENT = r'''/* FHDB writes are intentionally kept on fileXio instead of crossing between
   modern newlib stdio and directly mounted PFS handles.  Every critical write
   is read back before the installer is allowed to report success. */
#define MBR_SECTOR_SIZE 512
#define MBR_VERIFY_OFFSET 0x800

static int ReportFHDBIOError(const char *stage, int result)
{
    char message[192];

    DEBUG_PRINTF("FHDB I/O failure: %s (%d)\n", stage, result);
    snprintf(message, sizeof(message), "FHDB %s failed.\nError: %d", stage, result);
    ShowMessageBox(SYS_UI_LBL_OK, -1, -1, -1, message, SYS_UI_LBL_WARNING);

    return result;
}

static u32 UpdateFHDBHash(u32 hash, const void *buffer, unsigned int size)
{
    const u8 *bytes = buffer;
    unsigned int i;

    for (i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }

    return hash;
}

static int FileXioWriteAll(int fd, const void *buffer, unsigned int size)
{
    const u8 *position = buffer;
    unsigned int remaining = size;
    int written;

    while (remaining > 0) {
        written = fileXioWrite(fd, position, remaining);
        if (written <= 0)
            return written < 0 ? written : -EIO;

        position += written;
        remaining -= written;
    }

    return 0;
}

static int VerifyPFSFile(const char *path, unsigned int expectedSize, u32 expectedHash, void *buffer)
{
    iox_stat_t stat;
    unsigned int remaining, length;
    u32 hash;
    int fd, result, bytesRead;

    if ((result = fileXioGetStat(path, &stat)) < 0)
        return result;
    if (stat.hisize != 0 || stat.size != expectedSize)
        return -EIO;

    if ((fd = fileXioOpen(path, FIO_O_RDONLY)) < 0)
        return fd;

    hash = 2166136261u;
    result = 0;
    for (remaining = expectedSize; remaining > 0;) {
        length = remaining > IO_BLOCK_SIZE ? IO_BLOCK_SIZE : remaining;
        bytesRead = fileXioRead(fd, buffer, length);
        if (bytesRead <= 0) {
            result = bytesRead < 0 ? bytesRead : -EIO;
            break;
        }

        hash = UpdateFHDBHash(hash, buffer, bytesRead);
        remaining -= bytesRead;
    }

    if (fileXioClose(fd) < 0 && result >= 0)
        result = -EIO;
    if (result >= 0 && (remaining != 0 || hash != expectedHash))
        result = -EIO;

    return result;
}

static int InstallMBRToHDD(FILE *file, void *IOBuffer, unsigned int size)
{
    hddAtaTransfer_t *transfer = IOBuffer;
    hddSetOsdMBR_t OSDData;
    iox_stat_t stat;
    u8 *verifyBuffer = (u8 *)IOBuffer + MBR_VERIFY_OFFSET;
    unsigned int MBR_Sector, MBR_NumSectors, sectorIndex, bytesThisSector;
    int result;

    if ((result = fileXioGetStat("hdd0:__mbr", &stat)) < 0)
        return ReportFHDBIOError("MBR partition lookup", result);

    MBR_Sector = stat.private_5 + 0x2000;
    MBR_NumSectors = (size + MBR_SECTOR_SIZE - 1) / MBR_SECTOR_SIZE;
    if (MBR_NumSectors == 0)
        return ReportFHDBIOError("MBR size validation", -EINVAL);

    for (sectorIndex = 0; sectorIndex < MBR_NumSectors; sectorIndex++) {
        bytesThisSector = size - sectorIndex * MBR_SECTOR_SIZE;
        if (bytesThisSector > MBR_SECTOR_SIZE)
            bytesThisSector = MBR_SECTOR_SIZE;

        transfer->lba = MBR_Sector + sectorIndex;
        transfer->size = 1;

        /* Preserve bytes beyond a partial final sector instead of zeroing data
           that does not belong to the MBR payload. */
        if (bytesThisSector < MBR_SECTOR_SIZE) {
            result = fileXioDevctl("hdd0:", APA_DEVCTL_ATA_READ,
                                   transfer, sizeof(hddAtaTransfer_t),
                                   transfer->data, MBR_SECTOR_SIZE);
            if (result < 0)
                return ReportFHDBIOError("MBR final-sector read", result);
        }

        if (fread(transfer->data, 1, bytesThisSector, file) != bytesThisSector)
            return ReportFHDBIOError("MBR source read", -EIO);

        result = fileXioDevctl("hdd0:", APA_DEVCTL_ATA_WRITE,
                               transfer, sizeof(hddAtaTransfer_t) + MBR_SECTOR_SIZE,
                               NULL, 0);
        if (result < 0)
            return ReportFHDBIOError("MBR sector write", result);

        result = fileXioDevctl("hdd0:", APA_DEVCTL_ATA_READ,
                               transfer, sizeof(hddAtaTransfer_t),
                               verifyBuffer, MBR_SECTOR_SIZE);
        if (result < 0)
            return ReportFHDBIOError("MBR read-back", result);
        if (memcmp(transfer->data, verifyBuffer, MBR_SECTOR_SIZE) != 0)
            return ReportFHDBIOError("MBR verification", -EIO);
    }

    OSDData.start = MBR_Sector;
    OSDData.size = MBR_NumSectors;
    result = fileXioDevctl("hdd0:", APA_DEVCTL_SET_OSDMBR,
                           &OSDData, sizeof(OSDData), NULL, 0);
    if (result < 0)
        return ReportFHDBIOError("OSD MBR metadata", result);

    /* SET_OSDMBR flushes the APA header cache in ps2hdd-osd.  Sync as an
       additional barrier where the driver supports it. */
    fileXioSync("hdd0:", 0);

    return 0;
}

static int CopyFilesToHDD(const char *RootFolder, const struct FileCopyTarget *FileCopyList, unsigned int NumFilesEntries, unsigned int TotalNumBytes, unsigned int flags)
{
    unsigned int i, BytesCopied, remaining, CopyLength;
    int result, size, DestFd, closeResult, unmountResult;
    FILE *file;
    char *path, CurrentlyMountedBlockDeviceName[40], BlockDeviceToMount[40];
    const char *MountPath;
    void *buffer;
    u32 sourceHash;

    (void)flags;
    InitProgressScreen(SYS_UI_LBL_INSTALLING);

    if ((buffer = memalign(64, IO_BLOCK_SIZE)) == NULL)
        return -ENOMEM;

    result = 0;
    CurrentlyMountedBlockDeviceName[0] = '\0';
    for (i = 0, BytesCopied = 0; i < NumFilesEntries && result >= 0; i++) {
        DrawFileCopyProgressScreen((float)((double)BytesCopied / TotalNumBytes));

        if (FIO_S_ISDIR(FileCopyList[i].mode)) {
            DEBUG_PRINTF("mkdir: %s\n", FileCopyList[i].target);
            MountPath = GetMountParams(FileCopyList[i].target, BlockDeviceToMount);
            if (MountPath == NULL) {
                result = ReportFHDBIOError("directory target parsing", -EINVAL);
                break;
            }

            if (strcmp(BlockDeviceToMount, CurrentlyMountedBlockDeviceName)) {
                if (CurrentlyMountedBlockDeviceName[0] != '\0') {
                    fileXioSync("pfs0:", 0);
                    if ((unmountResult = fileXioUmount("pfs0:")) < 0) {
                        result = ReportFHDBIOError("PFS unmount", unmountResult);
                        break;
                    }
                }

                if ((result = fileXioMount("pfs0:", BlockDeviceToMount, FIO_MT_RDWR)) < 0) {
                    result = ReportFHDBIOError("PFS mount", result);
                    break;
                }
                strcpy(CurrentlyMountedBlockDeviceName, BlockDeviceToMount);
            }

            result = fileXioMkdir(MountPath, 0777);
            if (result == -EEXIST)
                result = 0;
            else if (result < 0)
                result = ReportFHDBIOError("PFS directory creation", result);
            continue;
        }

        path = malloc(strlen(RootFolder) + strlen(FileCopyList[i].source) + 2);
        if (path == NULL) {
            result = -ENOMEM;
            break;
        }
        sprintf(path, "%s/%s", RootFolder, FileCopyList[i].source);
        DEBUG_PRINTF("Copying %s -> %s...\n", FileCopyList[i].source, FileCopyList[i].target);

        file = fopen(path, "rb");
        free(path);
        if (file == NULL) {
            result = (-errno) | ERROR_SIDE_SRC;
            break;
        }

        size = FileCopyList[i].size;
        if (!strcmp(FileCopyList[i].target, "hdd0:__mbr")) {
            result = InstallMBRToHDD(file, buffer, size);
            if (result < 0)
                result |= ERROR_SIDE_DST;
            else
                BytesCopied += size;
        } else {
            MountPath = GetMountParams(FileCopyList[i].target, BlockDeviceToMount);
            if (MountPath == NULL) {
                result = ReportFHDBIOError("file target parsing", -EINVAL);
            } else {
                if (strcmp(BlockDeviceToMount, CurrentlyMountedBlockDeviceName)) {
                    if (CurrentlyMountedBlockDeviceName[0] != '\0') {
                        fileXioSync("pfs0:", 0);
                        if ((unmountResult = fileXioUmount("pfs0:")) < 0) {
                            result = ReportFHDBIOError("PFS unmount", unmountResult);
                        }
                    }

                    if (result >= 0) {
                        result = fileXioMount("pfs0:", BlockDeviceToMount, FIO_MT_RDWR);
                        if (result < 0)
                            result = ReportFHDBIOError("PFS mount", result);
                        else
                            strcpy(CurrentlyMountedBlockDeviceName, BlockDeviceToMount);
                    }
                }

                if (result >= 0) {
                    DestFd = fileXioOpen(MountPath, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
                    if (DestFd < 0) {
                        result = ReportFHDBIOError("PFS destination open", DestFd);
                    } else {
                        sourceHash = 2166136261u;
                        for (remaining = size; remaining > 0;) {
                            DrawFileCopyProgressScreen((float)((double)BytesCopied / TotalNumBytes));
                            CopyLength = remaining > IO_BLOCK_SIZE ? IO_BLOCK_SIZE : remaining;

                            if (fread(buffer, 1, CopyLength, file) != CopyLength) {
                                result = ERROR_SIDE_SRC | -EIO;
                                break;
                            }
                            sourceHash = UpdateFHDBHash(sourceHash, buffer, CopyLength);

                            if ((result = FileXioWriteAll(DestFd, buffer, CopyLength)) < 0) {
                                result = ReportFHDBIOError("PFS file write", result);
                                break;
                            }

                            BytesCopied += CopyLength;
                            remaining -= CopyLength;
                        }

                        closeResult = fileXioClose(DestFd);
                        if (result >= 0 && closeResult < 0)
                            result = ReportFHDBIOError("PFS destination close", closeResult);

                        if (result >= 0) {
                            fileXioSync("pfs0:", 0);
                            result = VerifyPFSFile(MountPath, size, sourceHash, buffer);
                            if (result < 0)
                                result = ReportFHDBIOError("PFS read-back verification", result);
                        }
                    }
                }
            }
        }

        fclose(file);
        if (result < 0 && !(result & ERROR_SIDE_SRC))
            result |= ERROR_SIDE_DST;
    }

    if (CurrentlyMountedBlockDeviceName[0] != '\0') {
        fileXioSync("pfs0:", 0);
        unmountResult = fileXioUmount("pfs0:");
        if (result >= 0 && unmountResult < 0)
            result = ReportFHDBIOError("final PFS unmount", unmountResult) | ERROR_SIDE_DST;
    }

    free(buffer);
    return result;
}

'''


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT OUTPUT", file=sys.stderr)
        return 2

    source_path = pathlib.Path(sys.argv[1])
    output_path = pathlib.Path(sys.argv[2])
    source = source_path.read_text(encoding="utf-8")

    if source.count(START_MARKER) != 1 or source.count(END_MARKER) != 2:
        # END_MARKER appears once in the prototype and once in the definition.
        raise RuntimeError("system.c FHDB markers changed; refusing an unsafe generated patch")

    start = source.index(START_MARKER)
    end = source.index(END_MARKER, start)
    output_path.write_text(source[:start] + REPLACEMENT + source[end:], encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
