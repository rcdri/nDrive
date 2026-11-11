# nDrive: the TI-Nspire CX II as an MTP device or a USB drive

nDrive is an Ndless background service. With it the calculator shows up on a
computer **next to** the normal TI link, so files can be copied with an
ordinary file manager and no TI software. It has two modes, chosen on the
calculator:

| | MTP device (default) | USB drive |
|---|---|---|
| Looks like | a phone | a USB stick (FAT) |
| Files are saved | as they are copied | about 1.5 s after the computer stops writing |
| Free space shown | the calculator's real figure | about half of it (see below) |
| `NspireLogs.zip` | works (built on request) | empty, as on the calculator |

Either mode can show the **whole calculator** or only **My Documents**.

## Using it

1. Copy `ndrive.tns` into **My Documents > ndless > startup** and restart the
   calculator (or open the file once). From the startup folder it starts
   silently.
2. Plug the calculator into a computer.
3. A file copied into My Documents is announced with the OS's own **Document
   Received** box. Files that arrive another way (TI link, saved on the
   calculator) show up when the folder is refreshed (MTP) or a few seconds
   later (drive).

**ctrl** then **U** shows a status box with counters and diagnostics. Its
**Options** button asks two questions: *MTP device or USB drive*, and *Whole
calculator or My Documents*. The calculator reconnects after a change, and the
choice is kept in `/documents/ndless/ndrive.cfg.tns`
(`mode=mtp|drive`, `root=all|documents`, `notify=on|off`). Opening
`ndrive.tns` by hand shows an info box with a **Notifications: on/off** button.

**Nothing is protected.** With the whole calculator shown, every file and
folder of the OS can be changed, renamed or deleted from the computer,
including `phoenix`: that can leave the calculator unable to start until the
OS is reinstalled. (The USB drive still asks on the calculator before applying
a mass deletion.)

## Notes on the two modes

- **MTP** (`src/mtp.c`) reads and writes calculator files directly: sessions,
  folder-by-folder listing, GetObject / GetPartialObject, SendObjectInfo /
  SendObject, delete, move, rename, object properties. Storage size and free
  space come from the OS's file system driver.
- **`NspireLogs.zip`** in My Documents is an empty placeholder; the OS fills it
  only when the TI link asks for that name. Over MTP nDrive calls the same OS
  routine when the folder is listed, so the file downloads as the real archive
  of the OS's logs.
- **USB drive** (`src/vfat.c`, `src/msc.c`) is a FAT16 volume generated from a
  snapshot of the files. What the computer writes is buffered (RAM first, 2 to
  12 MB, then a scratch file in `/tmp` on the calculator) and applied when the
  computer goes quiet: new and changed files, folders, renames, moves and
  deletions. While data waits in the scratch file and is being saved it exists
  twice, which is why the drive offers about half of the calculator's free
  space. Deleting many files at once, or formatting, asks on the calculator
  first. macOS/Windows housekeeping files are not copied to the calculator.
- With no computer connected nDrive does no work (one memory comparison per OS
  timer tick) and never delays power-down; during transfers it holds automatic
  power-down off.

## How it attaches to USB

The TI OS keeps running its own USB stack and the TI link on endpoint 1.
nDrive hooks the OS's USB register helpers and appends a second interface to
the OS's configuration descriptor: still-image class (MTP; bulk IN/OUT on
endpoint 2 plus the interrupt endpoint the class requires, never used; named
"MTP", which is how libmtp recognises an unknown device) or mass storage
(bulk-only, SCSI). It serves endpoint 2 from the OS's timer and GUI tasks with
its own FIFOs (2/3) and DMA channels (3/4). USB buffers live in the megabyte
of RAM the OS maps uncached for its own USB buffers; packet size follows the
bus speed (512 at high speed on real hardware). Data is sent one packet per
DMA transfer; a line `dma=big` in the settings file uses long transfers
(faster, unproven on hardware).

## Status

Tested in the Firebird emulator (CX II-T, OS 6.4.0.74, Ndless r2023), against
real libmtp, and against `fsck.fat`/mtools. Earlier USB-drive builds ran on a
real CX II-T. **Not yet confirmed on hardware:** the MTP mode (interface name
string, zero-length packets, the unused interrupt endpoint), the scratch file,
and the OS log-archive call. If MTP does not show up, ctrl+U > Options > USB
drive is the proven fallback. Other OS versions are refused.

n-link and the TI software next to nDrive: on Linux n-link sets the USB
configuration when it connects, which the kernel refuses while another driver
or program holds an interface (always the case for the USB drive; for MTP only
while a file manager has it open); `tools/libnspire-composite.patch` makes
n-link's library tolerate that (untested). On Windows a driver already bound
to the calculator's USB ID keeps the whole device; uninstall it so Windows
sees a composite device.

If something goes wrong: rename `ndless/startup/ndrive.tns` and reset.

## Building and testing

```sh
cmake -S . -B build-calc -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDLESS_SDK/share/ndless/cmake/TI-Nspire-Toolchain.cmake \
  -DCMAKE_PREFIX_PATH=$NDLESS_SDK -DPython3_EXECUTABLE=python3
cmake --build build-calc                 # -> build-calc/ndrive.tns
tests/run_host_tests.sh                  # libmtp, fsck.fat, mtools (PC)
python3 tests/e2e_mtp.py EMU_DIR build-calc/ndrive.tns   # MTP mode and the options
python3 tests/e2e_fat.py EMU_DIR build-calc/ndrive.tns   # USB drive mode
```

The emulator tests need Firebird (commit b10f3b5) with
`tools/firebird-msc-host.patch` and the headless `fbctl` with
`tools/fbctl-msc.patch`. Firebird's own TI link drops a request now and then
(with or without nDrive); the tests then start over by themselves.

## Licence

GNU GPL v3 or later (see LICENSE).

- `src/resident.*`: from [nKits](https://github.com/Ziyang-Bai/nKits),
  (C) 2026 Ziyang-Bai, GPL v3, modified.
- `third_party/fatfs`: FatFs R0.16 (C) ChaN, BSD-style 1-clause licence,
  configured read-only.
