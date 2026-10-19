/* plugin.ld - a machine plugin as one image at address 0: the entry table,
   code, read-only data, initialised data, bss. Relocations are kept
   (--emit-relocs) for rmp_pack.py. No C runtime start-up, no init_array. */
ENTRY(rmp_entry)
SECTIONS
{
  . = 0;
  .image : {
    KEEP(*(.rmp_entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
    __rmp_data_start = .;
    *(.data .data.*)
    . = ALIGN(4);
    __rmp_bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(8);
    __rmp_end = .;
  }
  .init_array : { *(.init_array .init_array.*) }   /* must be empty: checked by rmp_pack */
  .ARM.exidx : { *(.ARM.exidx*) }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
