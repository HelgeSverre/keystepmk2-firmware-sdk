# KeyStep mk2 SDK. Usage:
#   make                 build examples/hello
#   make APP=myapp       build examples/myapp
#   make sim             run the build in the emulator and print the display
#   make panel           run it in the KeyStep window
#   make flash-sim       rehearse flashing it, in the emulator
#   make fonts           write include/keystep_fonts.h again from third_party/x11-adobe (it is committed)
#   make new APP=myapp   start examples/myapp from examples/hello
# Flashing a unit is `just flash myapp` in the repository root (tools/ksflash.py, docs/FLASHING.md).
APP     ?= hello
# The version in the image header. The bootloader does not compare versions (docs/BOOTLOADER.md).
VERSION ?= 1.0.9
BUILD   := build/$(APP)
CC      := arm-none-eabi-gcc
CFLAGS  := -mcpu=cortex-m0plus -mthumb -Os -g -ffreestanding -ffunction-sections -fdata-sections \
           -Wall -Wextra -Werror -Wno-override-init -Iinclude
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,ld/keystep.ld -Wl,-Map,$(BUILD)/$(APP).map
SRC     := $(wildcard src/*.c) $(wildcard examples/$(APP)/*.c)
MS      ?= 3000

all: $(BUILD)/$(APP).img

$(BUILD)/$(APP).elf: $(SRC) include/*.h src/*.h $(wildcard examples/$(APP)/*.h) ld/keystep.ld
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -lgcc -o $@
	arm-none-eabi-size $@

$(BUILD)/$(APP).bin: $(BUILD)/$(APP).elf
	arm-none-eabi-objcopy -O binary $< $@

# The 64-byte header the stock images carry, with both XOR sums.
$(BUILD)/$(APP).img: $(BUILD)/$(APP).bin
	cd .. && uv run tools/fwpack.py image sdk/$< --version $(VERSION) -o sdk/$@

fonts:
	cd .. && uv run tools/bdffont.py header -o sdk/include/keystep_fonts.h

sim: $(BUILD)/$(APP).img
	cd .. && uv run --with unicorn tools/emu.py sdk/$< --ms $(MS) --quiet --flash sdk/$(BUILD)/flash.bin $(if $(SCRIPT),--script "$(SCRIPT)")

panel: $(BUILD)/$(APP).img
	cd .. && uv run --with unicorn --with pygame-ce tools/panel.py sdk/$< --flash sdk/$(BUILD)/flash.bin

# The update as tools/ksflash.py would do it, in the emulator against a model of the bootloader.
flash-sim: $(BUILD)/$(APP).img
	cd .. && uv run --with unicorn tools/ksflash.py sim sdk/$<

new:
	@test ! -e examples/$(APP) || { echo "examples/$(APP) already exists"; exit 1; }
	cp -R examples/hello examples/$(APP)
	@echo "created examples/$(APP); build it with: make APP=$(APP)"

clean:
	rm -rf build

.PHONY: all fonts sim panel flash-sim new clean
