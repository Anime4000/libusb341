# libusb341

A drop-in `libusb-1.0.dll` that lets libusb programs such as **SFP-Master** and **IMSProg**
use a CH341A through **WCH's own driver** (CH341PAR: `CH341WDM.SYS` / `CH341W64.SYS`),
instead of requiring the chip to be rebound to libusbK/WinUSB.

The CH341A stays on the vendor driver, so WCH's tools and every other program that uses
`CH341DLL` keep working, and the libusb programs work at the same time. Their sources and
binaries are untouched; only the DLL next to them changes.

## How it works

- **CH341A on the WCH driver (USB `1A86:5512` / `4348:5512`)**: the DLL talks to the
  driver directly with the same `DeviceIoControl` protocol WCH's `CH341DLL.DLL` uses. No
  `CH341DLL`/`CH341DLLA64.DLL` is needed (WCH installs only a 32-bit one).
- **Everything else** (CH347, FT232H, other USB devices, or a CH341A still on
  libusbK/WinUSB) is forwarded to the original libusb. Rename it to `libusb-1.0.orig.dll`
  and keep it next to the shim.
- libusb lets a program queue a long OUT stream plus many IN transfers and leaves the
  interleaving to the USB host. The CH341 only buffers about one reply packet. The shim
  works out how many bytes each 32-byte command packet (SPI `A8`, I2C `AA`, UIO `AB`) will
  return, sends them in batches with the driver's write-then-read call, and completes the
  libusb IN transfers from those replies. This covers both sync and async transfers.

## Drop-in for an existing SFP-Master folder

This works with any SFP-Master build that loads `libusb-1.0.dll`, including the shipped
`SFP-Master_win-amd64`. To build fresh copies instead, see the next section.

1. Make sure the programmer uses the WCH driver. In Device Manager it shows under
   **Interface** as **USB-EPP/I2C... CH341A** (see [Driver setup](#driver-setup)).
2. In the SFP-Master folder:
   - rename `libusb-1.0.dll` to `libusb-1.0.orig.dll`
   - copy `build\libusb-1.0.dll` into the folder
3. Start `SFP-Master.exe` as usual.

The `driver\` folder and the `InstallDriver.exe` step from SFP-Master's README are no
longer needed. They are what replaces the WCH driver.

## Building IMSProg and SFP-Master with the shim

The older `IMSProg_win-amd64` build does **not** use the `libusb-1.0.dll` next to it:
libusb and libftdi are statically linked into that EXE (its import table lists only
KERNEL32, msvcrt and Qt5), so no DLL can intercept it.

`tools/build-ch341-apps.sh` rebuilds both programs from their unmodified sources with
libusb linked as a DLL. On any MSYS2 install, download it to your home folder and run it
from the MINGW64 (or UCRT64) shell:

```bash
curl -fsSL https://raw.githubusercontent.com/Anime4000/libusb341/main/tools/build-ch341-apps.sh -o ~/build-ch341-apps.sh
```

```bash
bash ~/build-ch341-apps.sh
```

After the first run, just run `~/build-ch341-apps.sh` again to update and rebuild
everything. When `libusb341` carries a newer version of the script (a higher
`SCRIPT_VERSION`), the script replaces itself with it and restarts.

It installs the MSYS2 packages it needs (Qt6, libusb, libftdi, CMake, Ninja). It also
clones `~/IMSProg` and `~/SFP-Master`, or fast-forwards them if they already exist, and
builds the shim in `~/libusb341`. Each program is built out of tree, in
`~/build-imsprog` and `~/build-sfp-master`. The results are two self-contained folders:

- `~/IMSProg_win-amd64-shim`: IMSProg, the chip editor, the database updater, the chip
  database and translations
- `~/SFP-Master_win-amd64-shim`

Copy either folder to any 64-bit Windows PC and run the EXE; MSYS2 is not needed there.
In each folder, `libusb-1.0.dll` is this shim and `libusb-1.0.orig.dll` is MSYS2's libusb.
So a CH341A on the WCH driver goes through the shim, while FT232H (through
`libftdi1.dll`) and CH347 go through the original libusb.

The script clones this repository to `~/libusb341` if it is missing (set
`LIBUSB341_URL` to use another URL). Other settings (`ONLY`, `SKIP_PACMAN`,
`SKIP_UPDATE`, `RUN_TESTS`, the IMSProg/SFP-Master URLs) are listed at the top of the
script.

## Driver setup

Windows picks the driver per USB port. On this machine the WCH driver (`oem197.inf`) is
installed, but the libusbK packages made by SFP-Master's/IMSProg's `InstallDriver.exe`
(`oem223.inf`, `oem224.inf`) take over the CH341A on ports where they were used before.
To make the WCH driver win on every port, remove them from an **administrator** prompt:

```powershell
pnputil /delete-driver oem223.inf /uninstall
```

```powershell
pnputil /delete-driver oem224.inf /uninstall
```

Then unplug and replug the programmer. Keep `oem225.inf` (CH347T) if you use a CH347.
The published names (`oemNNN.inf`) differ from PC to PC; list them with
`pnputil /enum-drivers`.

## Checking the hardware

`build\ch341probe.exe` (copy it next to the shim) never writes to the chip:

```powershell
ch341probe
```

```powershell
ch341probe spi i2c
```

It lists the devices and shows which ones go through the WCH driver, opens the CH341A,
reads the SPI flash JEDEC ID and 64 KiB, and reads 16 bytes from I2C addresses
0x50/0x51.

## Settings (environment variables)

| Variable | Effect |
|---|---|
| `LIBUSB341_LOG=C:\path\file.log` | Write a log (useful for GUI programs). Turns on level 3. |
| `LIBUSB341_DEBUG=1..4` | Log level: 1 errors, 2 warnings, 3 info, 4 every packet. |
| `LIBUSB341_BATCH=1..127` | Command packets per driver call (default 127). |
| `LIBUSB341_DISABLE=1` | Ignore WCH-driver devices; forward everything to the original libusb. |
| `LIBUSB341_ORIG=C:\path\libusb-1.0.dll` | Use another original libusb instead of `libusb-1.0.orig.dll`. |

## What has been verified

- **Simulated chip (`make test`):** the unmodified CH341 code of IMSProg (async SPI,
  64 KiB sectors, odd lengths, erase/program) and SFP-Master (current sync I2C and the
  async I2C code inside the shipped `SFP-Master.exe`) runs against a simulated CH341A,
  25-series flash and 24C02 EEPROMs. The simulator stalls like the real chip if a program
  relies on it buffering more than it can.
- **Real CH341A on `CH341W64.SYS` (this PC):**
  - Enumeration, descriptors and the IC version (`0x30`) read correctly.
  - SPI 64 KiB read at about 128 KiB/s with batches of 127 packets (about 102 KiB/s with
    one packet per call).
  - I2C reads return the right lengths.
  - The shipped `SFP-Master.exe` connects and runs **Read from SFP** through the shim
    with no errors.
  - The rebuilt `IMSProg.exe` starts with only Windows on the PATH, connects, and runs
    **Detect** through the shim with no errors.
  - The socket was empty, so all data read as `FF`; reads from a real chip still need
    checking.
- **Rebuilt IMSProg GUI against the simulated chip:** Detect identifies a W25Q128 (JEDEC
  `EF 40 18`), and a full 16 MiB Read gives the expected CRC32 (`DADAA6FB`).
- The shim exports every function of libusb 1.0.30.

## Limitations

- Supports the CH341's EP2 bulk endpoints and control IN requests (descriptors, vendor
  reads), plus control OUT requests **without** a data stage. Control OUT with data and
  the interrupt endpoint return `LIBUSB_ERROR_NOT_SUPPORTED`.
- A CH341A on the WCH driver appears twice in `libusb_get_device_list` when passthrough
  is on: once from the shim (usable), and once from the original libusb (which cannot
  open it). `libusb_open_device_with_vid_pid` always picks the shim's entry first.
- `libusb_set_option(ctx, LIBUSB_OPTION_LOG_CB, ...)` is ignored, because SFP-Master
  passes `3` meaning "log level" with no callback. `libusb_set_log_cb()` still works.

## Build and test

From the MSYS2 MINGW64 shell (use MINGW32 for a 32-bit DLL):

```bash
make
```

```bash
make test
```

`make test` compiles the CH341 sources straight from `../IMSProg` and `../SFP-Master`
(and the shipped version from SFP-Master's `v1.0.6-win64` tag) without changing them.
