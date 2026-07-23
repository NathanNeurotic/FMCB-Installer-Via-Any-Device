#!/bin/sh
set -eu

src=${1:?source file required}
dst=${2:?destination file required}
tmp="${dst}.tmp"

trap 'rm -f "$tmp"' EXIT HUP INT TERM

awk '
BEGIN {
    in_mbr_branch = 0
    removed_double_close = 0
    checked_osdmbr = 0
}
{
    if ($0 ~ /if \(!strcmp\(FileCopyList\[i\]\.target, "hdd0:__mbr"\)\) \{/) {
        in_mbr_branch = 1
    }

    if (in_mbr_branch && $0 ~ /^[[:space:]]*fclose\(file\);[[:space:]]*$/) {
        print "                    /*"
        print "                     * The common cleanup below closes this source stream."
        print "                     * Closing it here as well is undefined behavior and can"
        print "                     * corrupt modern newlib state after the FHDB MBR write."
        print "                     */"
        removed_double_close++
        next
    }

    if (index($0, "fileXioDevctl(\"hdd0:\", APA_DEVCTL_SET_OSDMBR, &OSDData, sizeof(OSDData), NULL, 0);") != 0) {
        print "            result = fileXioDevctl(\"hdd0:\", APA_DEVCTL_SET_OSDMBR, &OSDData, sizeof(OSDData), NULL, 0);"
        print "            if (result >= 0)"
        print "                result = fileXioDevctl(\"hdd0:\", APA_DEVCTL_FLUSH_CACHE, NULL, 0, NULL, 0);"
        checked_osdmbr++
        next
    }

    print

    if (in_mbr_branch && $0 ~ /^[[:space:]]*} else {[[:space:]]*$/) {
        in_mbr_branch = 0
    }
}
END {
    if (removed_double_close != 1 || checked_osdmbr != 1) {
        print "FHDB source transform refused to continue: expected one double-close and one unchecked SET_OSDMBR call; found " removed_double_close " and " checked_osdmbr "." > "/dev/stderr"
        exit 1
    }
}
' "$src" > "$tmp"

mv "$tmp" "$dst"
trap - EXIT HUP INT TERM
