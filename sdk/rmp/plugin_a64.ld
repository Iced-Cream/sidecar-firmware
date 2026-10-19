/* plugin_a64.ld - an AArch64 machine plugin (an RMPA package, docs/user-machines-pi.md) as plugin.ld's one image
   at address 0, with its writable part on a page of its own: [0, __rmp_data_start) the entry table, code and
   read-only data (placed in executable memory), [__rmp_data_start, __rmp_end) initialised data and bss (placed
   in ordinary RW memory, anywhere 4096-aligned: the loader moves the code's ADRPs to it). So the code is never
   written while it runs (W^X: the arm64 Mac requires it, the Pi gets it). Relocations kept (--emit-relocs). */
ENTRY(rmp_entry)
SECTIONS
{
  . = 0;
  .image : {
    KEEP(*(.rmp_entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4096);
    __rmp_data_start = .;
    *(.data .data.*)
    . = ALIGN(8);
    __rmp_bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(8);
    __rmp_end = .;
  }
  .init_array : { *(.init_array .init_array.*) }   /* must be empty: checked by rmp_pack */
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
