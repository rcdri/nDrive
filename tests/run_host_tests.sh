#!/bin/sh
# Host tests. First nDrive's MTP responder driven by real libmtp (the library behind
# KDE's and GNOME's MTP support and the mtp-tools) through a stand-in libusb.
# Needs gcc, libmtp-dev (with mtp-tools) and libusb-1.0 headers.
set -e
cd "$(dirname "$0")/.."
B=${TMPDIR:-/tmp}/ndrive-host-tests
rm -rf "$B"
mkdir -p "$B/root/documents/Examples" "$B/root/phoenix" "$B/work"
gcc -O1 -g -Wall -Wextra -fPIC -shared -Isrc -Itests/mtp -o "$B/fakeusb.so" \
    tests/mtp/fakeusb.c tests/mtp/posix_fs.c src/mtp.c
gcc -O1 -Wall -o "$B/libmtp_test" tests/mtp/libmtp_test.c $(pkg-config --cflags --libs libmtp)
echo hello > "$B/root/documents/a.tns"
head -c 100000 /dev/urandom > "$B/root/documents/Examples/big.tns"
head -c 1012 /dev/urandom > "$B/root/documents/exact.tns"     # 12 + 1012 = two full packets
echo sys > "$B/root/phoenix/sys.bin"
export NDRIVE_ROOT="$B/root" NDRIVE_RECEIVED_LOG="$B/work/received.log"
LD_PRELOAD="$B/fakeusb.so" "$B/libmtp_test" "$B/root" "$B/work" 2>/dev/null | grep -v '^$'
# The stock tools read the whole device at once (a different code path).
n=$(LD_PRELOAD="$B/fakeusb.so" mtp-files 2>/dev/null | grep -c 'File ID')
[ "$n" -ge 10 ] && echo "PASS mtp-files lists the whole device ($n files)" || { echo "FAIL mtp-files ($n)"; exit 1; }
echo test > "$B/work/t.txt"
LD_PRELOAD="$B/fakeusb.so" mtp-sendfile "$B/work/t.txt" t.txt >/dev/null 2>&1
cmp -s "$B/work/t.txt" "$B/root/t.txt" && echo "PASS mtp-sendfile (to the root folder)" || { echo "FAIL mtp-sendfile"; exit 1; }
# A write that fails on the calculator (storage full) must be reported, not swallowed.
make_big() { head -c 300000 /dev/urandom > "$B/work/full.bin"; }
make_big
if NDRIVE_FAIL_WRITES=3 LD_PRELOAD="$B/fakeusb.so" mtp-sendfile "$B/work/full.bin" full.bin 2>&1 | grep -qi 'error\|fail'; then
    [ ! -e "$B/root/full.bin" ] && echo "PASS failed write is reported and leaves no partial file" || { echo "FAIL partial file left"; exit 1; }
else echo "FAIL failed write not reported"; exit 1; fi

# ---- USB drive mode: virtual FAT volume (fsck.fat + mtools) and the SCSI layer.
# Needs mtools and dosfstools.
mkdir -p build
CFLAGS="-std=gnu99 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Isrc -Itests/fat -Ithird_party/fatfs"
SRC="tests/fat/posix_backend.c src/vfat.c third_party/fatfs/ff.c third_party/fatfs/ffunicode.c"
gcc $CFLAGS tests/fat/vfat_tool.c $SRC -o build/vfat_tool
gcc $CFLAGS tests/fat/test_msc.c src/msc.c $SRC -o build/test_msc
unset NDRIVE_ROOT NDRIVE_RECEIVED_LOG
T=$(mktemp -d); mkdir -p "$T/sub"; head -c 100000 /dev/urandom > "$T/sub/x.tns"
./build/test_msc "$T" | tail -1; rm -rf "$T"
echo "-- virtual FAT, RAM only"
python3 tests/fat/run_vfat_tests.py | tail -1
python3 tests/fat/run_session_tests.py | tail -1
echo "-- virtual FAT, 256 KB of RAM and a scratch file"
VFAT_RAM_KB=256 VFAT_FREE_MB=40 python3 tests/fat/run_vfat_tests.py | tail -1
VFAT_RAM_KB=256 VFAT_FREE_MB=40 python3 tests/fat/run_session_tests.py | tail -1
echo "HOST TESTS DONE"
