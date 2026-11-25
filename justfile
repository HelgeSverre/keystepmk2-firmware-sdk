# KeyStep mk2 firmware research. Only the recipes in the device group talk to a device, and they only read.
# Needs: uv; arm-none-eabi-gcc for the SDK; Ghidra 12 and a JDK for the analysis recipes.

uv := "uv"
emu := "uv run --with unicorn tools/emu.py"
panel := "uv run --with unicorn --with pygame-ce tools/panel.py"
version := "1.0.2"

# List recipes
default:
    @just --list

# === Firmware ===

# List apps
[group('firmware')]
apps:
    @ls sdk/examples

# Build an app
[group('firmware')]
build app="hello":
    make -C sdk APP={{ app }}

# New app from hello
[group('firmware')]
new app:
    make -C sdk APP={{ app }} new

# Rehearse flashing an app: emulated device, model of the bootloader (nothing real is flashed yet)
[group('firmware')]
flash-sim app="hello":
    make -C sdk APP={{ app }} flash-sim

# Download and unpack the stock packages
[group('firmware')]
fetch *args:
    {{ uv }} run tools/fwfetch.py {{ args }}

# Print an image's header and run the checks
[group('firmware')]
info file=("firmware/keystep_mk2__" + replace(version, ".", "_") + ".ks2fw"):
    {{ uv }} run tools/fwpack.py info {{ file }}

# Strip the header: the bytes loaded at 0x08008000
[group('firmware')]
body file out:
    {{ uv }} run tools/fwpack.py body {{ file }} -o {{ out }}

# Write the stock fonts' header for the SDK, or preview a text: just fonts show 1 "Text"
[group('firmware')]
fonts *args="header -o sdk/build/generated/keystep_fonts.h":
    {{ uv }} run tools/fontextract.py {{ args }}

# Remove build output
[group('firmware')]
clean:
    make -C sdk clean

# === Preview ===
# An app is a folder in sdk/examples, `stock`, or a stock version such as 1.0.4.

# Run an app in the KeyStep window
[group('preview')]
[positional-arguments]
host app="hello" *args:
    shift $(( $# > 0 )); {{ panel }} $(just _image {{ app }}) "$@"

# Screenshot an app: <app>.png, or pass --shot FILE; --script "500 tap=play" presses things first
[group('preview')]
[positional-arguments]
shot app="hello" ms="3000" *args:
    shift $(( $# > 2 ? 2 : $# )); {{ panel }} $(just _image {{ app }}) --ms {{ ms }} --shot {{ app }}.png "$@"

# Run an app in the emulator without a window: --ms 5000 --script "2500 tap=seq; 4000 show"
[group('preview')]
[positional-arguments]
sim app="hello" *args:
    shift $(( $# > 0 )); {{ emu }} $(just _image {{ app }}) --quiet "$@"

# The image to run for an app name, built first, with its flash file
[private]
_image app:
    #!/bin/sh
    set -e
    case "{{ app }}" in
        stock) echo "firmware/keystep_mk2__{{ replace(version, ".", "_") }}.ks2fw" ;;
        [0-9]*) echo "firmware/keystep_mk2__$(echo "{{ app }}" | tr . _).ks2fw" ;;
        *) make -s -C sdk APP="{{ app }}" >&2
           echo "sdk/build/{{ app }}/{{ app }}.img --flash sdk/build/{{ app }}/flash.bin" ;;
    esac

# === Device (reads only) ===

# Print the USB descriptors of a connected KeyStep: application or update mode
[group('device')]
probe:
    {{ uv }} run --with pyusb tools/ksflash.py probe

# Flash an SDK app to the connected unit: asks for update mode, sends, reads back, starts it
[group('device')]
flash app="hello": (build app)
    {{ uv }} run --with pyusb tools/ksflash.py flash sdk/build/{{ app }}/{{ app }}.img --real

# Flash a stock package back
[group('device')]
flash-stock file=("firmware/keystep_mk2__" + replace(version, ".", "_") + ".ks2fw"):
    {{ uv }} run --with pyusb tools/ksflash.py flash {{ file }} --real

# Compare the unit's flash with an image. Reads only; a running firmware is left in update mode
[group('device')]
verify file=("firmware/keystep_mk2__" + replace(version, ".", "_") + ".ks2fw"):
    {{ uv }} run --with pyusb tools/ksflash.py verify {{ file }}

# Listen for the self-test's report (flash it first: just flash selftest). Reads only
[group('device')]
selftest-receive *args:
    {{ uv }} run --with mido --with python-rtmidi tools/selftest.py receive --port "KeyStep mk2" {{ args }}

# Receive a flash dump from the flashdump firmware over MIDI
[group('device')]
dump-receive out="flash.bin" *args:
    {{ uv }} run --with mido --with python-rtmidi tools/flashdump.py receive -o {{ out }} {{ args }}

# === Analysis ===

# Import a version into Ghidra and export decompiled C
[group('analysis')]
analyze v=version:
    ghidra/analyze.sh {{ v }}

# Re-export from the existing Ghidra project
[group('analysis')]
reexport v=version:
    ghidra/reexport.sh {{ v }}

# Function-level diff of two exported versions
[group('analysis')]
diff a="1.0.2" b="1.0.4":
    {{ uv }} run python ghidra/scripts/verdiff.py {{ a }} {{ b }}

# Disassemble an address range of a version
[group('analysis')]
disasm start stop v=version:
    arm-none-eabi-objdump -D -b binary -marm -Mforce-thumb --adjust-vma=0x08008000 \
        --start-address={{ start }} --stop-address={{ stop }} ghidra/work/ks2_{{ v }}.bin | tail -n +8

# === QA ===

# Build every app
[group('qa')]
build-all:
    for app in $(ls sdk/examples); do make -s -C sdk APP=$app || exit 1; done

# Run the stock firmware and the apps through the emulator checks
[group('qa')]
test *args: build-all
    make -s -C sdk/examples/mk2000/tests
    {{ uv }} run --with unicorn tools/emutest.py {{ args }}
    {{ uv }} run --with unicorn tools/selftest_emu.py

# Rebuild every stock header and compare
[group('qa')]
selftest:
    {{ uv }} run tools/fwpack.py selftest

# The flasher's update and refusals, in the emulator
[group('qa')]
flash-selftest:
    make -s -C sdk APP=hello
    {{ uv }} run --with unicorn tools/ksflash.py selftest

# Lint, no changes
[group('qa')]
lint:
    {{ uv }} run ruff check .
    {{ uv }} run ruff format --check .

# Format Python
[group('qa')]
format:
    {{ uv }} run ruff format .
    {{ uv }} run ruff check --fix .

# Lint and build all
[group('qa')]
check: lint build-all
