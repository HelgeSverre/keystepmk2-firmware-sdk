/* KeyStep mk2 application. The factory bootloader owns the first 32 KiB of flash and is never
 * written. The first 32 bytes of RAM are a block shared with it (the update-mode marker), so
 * nothing is placed there. Both facts are read from the stock firmware. */
MEMORY {
    FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 480K
    RAM   (rwx) : ORIGIN = 0x20000020, LENGTH = 144K - 0x20
}
_estack = 0x20024000;   /* the stock firmware's value */
ENTRY(Reset_Handler)
SECTIONS {
    .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
    _sidata = LOADADDR(.data);
    .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
    .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
