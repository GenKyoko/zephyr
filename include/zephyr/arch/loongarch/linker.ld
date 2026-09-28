/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linker command/script file for the LoongArch platform.
 */

#include <zephyr/linker/sections.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/linker-tool.h>

#define ROMABLE_REGION              RAM
#define RAMABLE_REGION              RAM

#define _EXCEPTION_SECTION_NAME     exceptions

MEMORY
{
    RAM (rwx) : ORIGIN = CONFIG_SRAM_BASE_ADDRESS, LENGTH = KB(CONFIG_SRAM_SIZE)
    /* Used by and documented in include/linker/intlist.ld */
    IDT_LIST  (wx)      : ORIGIN = 0xFFFFF7FF, LENGTH = 2K
}

REGION_ALIAS("REGION_TEXT", RAM);
REGION_ALIAS("REGION_RODATA", RAM);
REGION_ALIAS("REGION_DATA_VMA", RAM);
REGION_ALIAS("REGION_DATA_LMA", RAM);
REGION_ALIAS("REGION_BSS", RAM);

ENTRY(CONFIG_KERNEL_ENTRY)

PROVIDE (__memory_base = CONFIG_SRAM_BASE_ADDRESS);
PROVIDE (__memory_size = CONFIG_SRAM_SIZE * 1024);
PROVIDE (__stack = (CONFIG_SRAM_BASE_ADDRESS + KB(CONFIG_SRAM_SIZE)) & ~0xf);

SECTIONS
{

#include <zephyr/linker/rel-sections.ld>
#include <zephyr/linker/llext-sections.ld>

    /*
     * Exception entry, must be 4 KiB aligned because CSR.EENTRY only
     * keeps VPN[63:12] (see core/isr.S).
     */
    SECTION_PROLOGUE(_EXCEPTION_SECTION_NAME,,)
    {
		. = ALIGN(0x1000);
		KEEP(*(.exception.entry))
		*(".exception.other.*")
    } GROUP_LINK_IN(ROMABLE_REGION)

    SECTION_PROLOGUE(_TEXT_SECTION_NAME,,)
	{
		. = ALIGN(4);

		*(.text)
		*(".text.*")
	} GROUP_LINK_IN(REGION_TEXT)

    __rodata_region_start = .;
#include <zephyr/linker/common-rom.ld>
/* Located in generated directory. This file is populated by calling
 * zephyr_linker_sources(ROM_SECTIONS ...). Useful for grouping iterable RO structs.
 */
#include <snippets-rom-sections.ld>

    SECTION_PROLOGUE(_RODATA_SECTION_NAME,,)
	{
		 . = ALIGN(8);
		 *(.rodata)
		 *(.rodata.*)
		 *(.gnu.linkonce.r.*)
		 *(.rodata1)
		 *(.srodata)
		 *(.srodata.*)

/* Located in generated directory. This file is populated by the
 * zephyr_linker_sources() Cmake function.
 */
#include <snippets-rodata.ld>

	} GROUP_LINK_IN(REGION_RODATA)

#include <zephyr/linker/cplusplus-rom.ld>
    __rodata_region_end = .;

    SECTION_DATA_PROLOGUE(_DATA_SECTION_NAME,,)
	{
		 . = ALIGN(8);
		 _image_ram_start = .;
		 __data_ram_start = .;

		 *(.data)
		 *(.data.*)
		 *(.gnu.linkonce.d.*)
		 *(.sdata)
		 *(.sdata.*)
		 *(.got)
		 *(.got.*)
		 *(.igot.*)
		 . = ALIGN(8);
		SORT(CONSTRUCTORS)

/* Located in generated directory. This file is populated by the
 * zephyr_linker_sources() Cmake function.
 */
#include <snippets-rwdata.ld>

	} GROUP_DATA_LINK_IN(REGION_DATA_VMA, REGION_DATA_LMA)

#include <zephyr/linker/common-ram.ld>

/* Located in generated directory. This file is populated by the
 * zephyr_linker_sources() Cmake function.
 */
#include <snippets-ram-sections.ld>

/* Located in generated directory. This file is populated by the
 * zephyr_linker_sources() Cmake function.
 */
#include <snippets-data-sections.ld>

    __data_ram_end = .;

    SECTION_DATA_PROLOGUE(_BSS_SECTION_NAME,(NOLOAD),)
	{
		 . = ALIGN(8);
		 __bss_start = .;
		 *(.dynbss)
		 *(.sbss)
		 *(.sbss.*)
		 *(.bss)
		 *(.bss.*)
		 *(.gnu.linkonce.b.*)
		 *(.scommon)
		 COMMON_SYMBOLS
		  __bss_end = ALIGN(8);
	}  GROUP_LINK_IN(REGION_BSS)

    SECTION_PROLOGUE(_NOINIT_SECTION_NAME,(NOLOAD),)
	{
		 *(.noinit)
		 *(.noinit.*)

/* Located in generated directory. This file is populated by the
 * zephyr_linker_sources() Cmake function.
 */
#include <snippets-noinit.ld>

	} GROUP_LINK_IN(REGION_BSS)

#include <zephyr/linker/cplusplus-ram.ld>

/* Located in generated directory. This file is populated by the
 * zephyr_linker_sources() Cmake function.
 */
#include <snippets-sections.ld>

#include <zephyr/linker/ram-end.ld>

    GROUP_END(RAMABLE_REGION)

#include <zephyr/linker/debug-sections.ld>

    SECTION_PROLOGUE(.gnu.attributes, 0,)
	{
		KEEP(*(.gnu.attributes))
	}

	/DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
