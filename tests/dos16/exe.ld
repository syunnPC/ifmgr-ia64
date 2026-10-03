/* SPDX-License-Identifier: GPL-2.0-or-later
 * An MZ executable: the .hdr section (header and relocation table) is the
 * start of the file, and the program, linked at offset 0, follows it. */
OUTPUT_FORMAT(binary)
ENTRY(start)
SECTIONS {
    .hdr 0 : { *(.hdr) }
    .text 0 : AT(SIZEOF(.hdr)) { *(.text) *(.data) }
    _image = SIZEOF(.hdr) + SIZEOF(.text);
    _last = _image % 512;
    _pages = (_image + 511) / 512;
    _header_paras = SIZEOF(.hdr) / 16;
    _code2_seg = code2 / 16;
    /DISCARD/ : { *(.note*) *(.comment) }
}
