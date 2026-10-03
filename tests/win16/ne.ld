/* SPDX-License-Identifier: GPL-2.0-or-later
 * A Windows 3.0 NE executable: the MZ stub, the NE header and tables, then
 * the code segment followed by its relocation records, the data segment
 * and the nonresident names. Every section is linked at offset 0, so labels
 * are offsets within the NE header or within their segment. File offsets
 * are in 16-byte units (alignment shift 4). */
OUTPUT_FORMAT(binary)
ENTRY(start)
SECTIONS {
    .mz 0 : { *(.mz) }
    .ne 0 : AT(SIZEOF(.mz)) { *(.ne) }
    .code 0 : AT(ALIGN(LOADADDR(.ne) + SIZEOF(.ne), 16)) { *(.code) }
    .coderel 0 : AT(LOADADDR(.code) + SIZEOF(.code)) { *(.coderel) }
    .data 0 : AT(ALIGN(LOADADDR(.coderel) + SIZEOF(.coderel), 16)) { *(.data) }
    .nonres 0 : AT(LOADADDR(.data) + SIZEOF(.data)) { *(.nonres) }
    _mz_last = SIZEOF(.mz) % 512;
    _mz_pages = (SIZEOF(.mz) + 511) / 512;
    _ne_file = LOADADDR(.ne);
    _code_sector = LOADADDR(.code) / 16;
    _code_size = SIZEOF(.code);
    _data_sector = LOADADDR(.data) / 16;
    _data_size = SIZEOF(.data);
    _nonres_file = LOADADDR(.nonres);
    _nonres_size = SIZEOF(.nonres);
    /DISCARD/ : { *(.note*) *(.comment) *(.text) }
}
