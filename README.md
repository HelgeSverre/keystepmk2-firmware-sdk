# KeyStep mk2 custom firmware SDK

![KeyStep MK2000 running in the emulator's KeyStep window](docs/panel-mk2000.png)

A C SDK, a USB flasher and an emulator for writing your own firmware for the Arturia KeyStep
mk2, the 32-key model. Firmware is flashed over the unit's own USB port, read back and verified
before it starts, and every way back to Arturia's firmware has been tried on a unit.

This repository holds three things:

- **`sdk/`**: a small C SDK. Display and two display fonts, the 13 button lights, buttons,
  encoder, keys with velocity, touch strips, aftertouch and pedal, MIDI on DIN and USB, pitch
  and mod CV, gate and sync, and storage in the part of the external flash Arturia's firmware
  never uses. A self-test firmware checks it on a unit.
- **KeyStep MK2000** (`sdk/examples/mk2000`): a four-track sequencer firmware built on it, with
  the stock firmware's gestures, its arpeggiator, chords, scales, Mutate and Spice, chains and
  external clock. [User guide](docs/USER-GUIDE.md) and [design](docs/MK2000.md).
- **The reverse-engineering work behind it** (`docs/`, `tools/`, `ghidra/`): the image format,
  the bootloader and its update protocol, the SysEx commands, the hardware map, and an emulator
  that runs the stock firmware and SDK firmware alike.

This project is unofficial and is not affiliated with or endorsed by Arturia. You flash your unit
at your own risk: flashing has been done on one 32-key unit, with a recovery path proven there;
other units and the 37-key model are untried. Read [FLASHING](docs/FLASHING.md) before you flash
anything.

## Quick start

Requires `arm-none-eabi-gcc`, [`uv`](https://docs.astral.sh/uv/) and
[`just`](https://github.com/casey/just) 1.27 or later, and libusb to flash. It builds and passes
`just test` on macOS, Linux and Windows; [Linux and Windows](#linux-and-windows) lists what
differs there.

```sh
brew install arm-none-eabi-gcc uv just libusb     # macOS
```

All commands run from the repository root. `just` on its own lists them.

```sh
just host mk2000      # MK2000 in a window that looks and plays like the KeyStep
just flash mk2000     # build it and flash it to a connected unit
just new mything      # start your own app in sdk/examples/mything, copied from hello
just fetch            # download Arturia's published firmware: to go back, and for the stock checks
just flash-stock      # flash Arturia's 1.0.2 again
```

`just flash` asks the running firmware for update mode, sends the image, reads it back and only
then starts it. Before the first flash, copy your unit's external flash as
[FLASHING](docs/FLASHING.md) describes: it holds the unit's own calibration.

## Requirements and recipes in full

| For | You need |
|---|---|
| Every recipe | [`just`](https://github.com/casey/just) 1.27 or later and [`uv`](https://docs.astral.sh/uv/) (it installs Python and the packages) |
| Building SDK apps | `arm-none-eabi-gcc` (macOS: `brew install arm-none-eabi-gcc`; Debian and Ubuntu: `gcc-arm-none-eabi`) |
| `just test` | also a host C compiler (clang or gcc); the sanitizers are used where the compiler has them |
| `just host`, `just shot` | a desktop: the window is pygame |
| `just flash`, `just probe`, `just verify` | libusb (macOS: `brew install libusb`; Linux: `libusb-1.0-0`) |
| `just analyze` | Ghidra and a JDK; set `GHIDRA_HOME` and `JAVA_HOME` if they are not where Homebrew puts them |

Arturia's firmware is not in this repository. Every SDK app, MK2000 included, builds from a fresh
clone; `just fetch` downloads the published packages from Arturia, which `just test` (it runs the
stock firmware too), `just host stock` and the analysis recipes need.

```bash
just fetch            # download and unpack all known versions
just selftest         # rebuild every stock image header and compare (not the self-test firmware)
just host stock       # the stock firmware in a window you can click
just host hello       # build an app and open it in the same window
just host mk2000      # KeyStep MK2000: the four-track firmware (docs/MK2000.md)
just shot mk2000      # a picture of it after 3 s, as mk2000.png
just flash-sim hello  # rehearse flashing it: emulated device, model of the bootloader
just test             # emulator checks of the stock firmware and the SDK examples, MK2000's host tests
just probe            # read a connected KeyStep's USB descriptors (sends nothing else)
just sim stock --ms 5000 --script "2500 tap=seq; 4000 show"   # the same without a window
just check            # lint and build every app
just analyze 1.0.4    # decompile one (needs Ghidra and a JDK)
```

### Linux and Windows

Tested on Ubuntu 24.04 (arm64) and Windows Server 2022 (x64).

**Linux.** Ubuntu's `just` is too old; install it through uv.

```sh
sudo apt install git make curl gcc-arm-none-eabi libnewlib-arm-none-eabi clang libusb-1.0-0
curl -LsSf https://astral.sh/uv/install.sh | sh
uv tool install rust-just
```

For `just flash` and `just probe`, either run them with `sudo` or give your user the device:

```sh
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="1c75", MODE="0666"' | sudo tee /etc/udev/rules.d/50-keystep.rules
sudo udevadm control --reload
```

The MIDI recipes need the ALSA sequencer (`sudo modprobe snd-seq-midi`) and your user in the
`audio` group. On Linux, MK2000 was built, flashed to a unit and run, and the unit was flashed
back to stock.

**Windows.** Build tools come from [MSYS2](https://www.msys2.org); Python and `just` come from
uv. Run the recipes from PowerShell or cmd with MSYS2's `ucrt64\bin` and `usr\bin` on `PATH`
(`just` needs `sh` and `make`).

```sh
pacman -S make git mingw-w64-ucrt-x86_64-arm-none-eabi-gcc mingw-w64-ucrt-x86_64-arm-none-eabi-newlib mingw-w64-ucrt-x86_64-clang
```

MinGW has no address sanitizer, so on Windows MK2000's host tests run without one. The repository
pins Python 3.14 in `.python-version` because `littlefs-python` has no Windows build for newer
versions yet.

## What is where

| Path | Contents |
|------|----------|
| `docs/ROADMAP.md` | The plan, the gating questions and the safety rules |
| `docs/FIRMWARE.md` | Where the firmware comes from, the package and image format, the processor |
| `docs/SYSEX.md` | The SysEx dispatcher: all 14 operations and the parameter id layout |
| `docs/VERSIONS.md` | What differs between the published versions |
| `docs/HARDWARE.md` | Clock, pin table, buses, display, LEDs, keybed, CV, external flash |
| `docs/user-guide.html`, `docs/USER-GUIDE.md` | The MK2000 user guide: a designed, standalone page, and the same facts as plain text |
| `docs/MK2000.md` | The MK2000 design: what the code is built to |
| `docs/SELFTEST.md` | The self-test firmware: what it checks on a unit and how to read its report |
| `docs/FLASHING.md` | How to flash a unit, check it and get back to stock; what never to send |
| `docs/BOOTLOADER.md` | Update mode and the bootloader: what a unit did, and the bootloader's code read |
| `docs/UPDATER.md` | How MIDI Control Center installs firmware (USB DFU with Arturia requests) |
| `firmware/` | Where `just fetch` puts the four published packages, unpacked; their hashes are in `listing.json` |
| `tools/fwfetch.py` | Downloads and unpacks the packages |
| `tools/fwpack.py` | Reads, checks and builds images and packages |
| `tools/emu.py` | Runs an image on the host: Cortex-M0+ with models of the display, flash, LEDs, keys, buttons, encoder and touch strips |
| `tools/usbmodel.py` | The USB device controller and a host for it, used by the emulator |
| `tools/ksflash.py` | The firmware update as MIDI Control Center does it: flash, read back, verify. Used on a unit ([FLASHING.md](docs/FLASHING.md)) |
| `tools/flashdump.py` | Reassembles the external flash from the `flashdump` firmware's SysEx |
| `tools/emutest.py` | Checks of the stock firmware and the SDK examples in the emulator |
| `tools/selftest.py` | Receives and decodes the self-test firmware's report ([SELFTEST.md](docs/SELFTEST.md)) |
| `tools/selftest_emu.py` | Runs the self-test firmware in the emulator, doing what a person would |
| `tools/screentext.py` | Reads text off the emulated display, for the checks |
| `tools/fontextract.py` | Reads the stock display fonts out of an image, for analysis and the display reader |
| `tools/bdffont.py` | Makes the SDK's two display fonts from the X11 bitmaps in `sdk/third_party/x11-adobe` |
| `tools/panel.py` | A window that looks like the KeyStep, on top of the emulator |
| `sdk/` | C SDK: start-up code, board drivers and examples, for the emulator and a unit ([README](sdk/README.md)) |
| `ghidra/names-1.0.2.tsv` | Names for identified functions |
| `ghidra/` | Headless Ghidra pipeline: import, function seeding, export, version diff |

## What is known

- The package is a zip with `info.json` and one image.
- The image is **not encrypted**: a 64-byte header, then a plain ARM Cortex-M0+ binary loaded at
  `0x08008000`. The header's two integrity fields are XOR sums.
- The processor is an STM32G0B1-class part.
- The SysEx dispatcher handles 14 operations; MIDI Control Center uses 9 of them. The version
  request and per-step parameter reads were confirmed on a device running 1.0.2; steps 1-56 of a
  pattern are readable as individual bytes.
- Updates go over USB DFU with three Arturia requests. MIDI Control Center does not inspect the
  image, so every check is the bootloader's. Arturia documents a recovery entry: hold Tempo while
  switching on.
- The board: 64 MHz, a 128×32 OLED, 54 addressable LEDs, a 512 KiB SPI flash with littlefs, a
  touch controller on I2C, MIDI DIN on USART1.
- The stock 1.0.2 image runs in the emulator and responds to every control: buttons, keys,
  encoder and touch strips. It sends the right MIDI on its DIN output, enumerates over USB and
  answers the SysEx version request there with the reply a real unit gave.
- The external flash holds 64 patterns, the settings, a file system with the factory
  calibration, and 124 KiB the stock firmware never uses; the SDK stores only there.
- In the emulator the stock firmware also plays its sequencer, follows an external clock, drives
  the CV, gate and sync pins and runs as the 37-key model. A pattern slot in the flash is the
  same 4096 bytes as a pattern in a `.keystep2` file.
- The SDK builds firmware with drivers for every mapped part including USB MIDI, CV, gate and
  sync, and a four-track sequencer (MK2000) runs on it. It has two display fonts laid out like
  the stock ones, made from the X11 Helvetica bitmaps. `flashdump`, `bootdump`, `selftest` and MK2000 have run on a unit:
  what was checked there is in [SELFTEST.md](docs/SELFTEST.md) and at the top of
  [MK2000.md](docs/MK2000.md). MIDI DIN, CV, gate and sync have not been tried on a unit.
- 1.0.1 and 1.0.2 contain identical code. 1.0.4 keeps the same SysEx operations and pattern
  parameter layout as 1.0.2 and adds global settings.

## Caution

Flashing has been done on one 32-key unit, with a recovery path proven there
([FLASHING](docs/FLASHING.md)). Other units, the 37-key model and other bootloader revisions are
untried: read that file first, take the copy of the external flash it describes, and keep to the
"never send" list.

## Licence

The tools, the SDK, the examples and the notes are under the MIT licence ([LICENSE](LICENSE)).
That does not cover Arturia's firmware, which is not in this repository: `just fetch` downloads
the published packages from Arturia, and anything read from a unit stays in the git-ignored
`firmware/device/`. Nothing of Arturia's goes into a firmware built with the SDK.

Third-party material in the repository:

- The Barlow fonts in `tools/assets`, and the subsets of them embedded in `docs/user-guide.html`:
  the Barlow Project Authors, SIL Open Font License 1.1 (`tools/assets/OFL.txt`).
- `sdk/third_party/x11-adobe`, and the SDK's two display fonts made from it
  (`sdk/include/keystep_fonts.h`): Adobe Systems and Digital Equipment, under the X11 notice in
  `sdk/third_party/x11-adobe/COPYING`.
- `sdk/third_party/font8x8_basic.h`: public domain (Daniel Hepper, from Marcel Sondaar's and
  IBM's VGA fonts).

