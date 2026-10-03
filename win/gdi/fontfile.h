/* SPDX-License-Identifier: GPL-2.0-or-later
 * Font files (fontfile.c): the raster fonts of a Windows font resource file.
 */
#ifndef FONTFILE_H
#define FONTFILE_H
/* A raster font: its face name, family (LOGFONT's: FF_ and the pitch),
 * character set and style; the cell's height, the ascent and the internal
 * leading above the characters, the average and largest widths; per
 * character the advance and the rows, words 32-bit words each, the most
 * significant bit leftmost. Characters the font lacks are its default one. */
typedef struct {
    char name[32]; BYTE family,charset; BOOL italic,bold,proportional;
    int points,height,ascent,leading,avg,max,words;
    unsigned char widths[256];
    unsigned int *bits; /* 256 x height x words */
} FileFont;
/* The raster fonts of a .FON (an NE module whose FONT resources are fonts
 * in the FNT format, versions 2.0 and 3.0) or of a bare .FNT file: how many
 * were read into fonts[0..max), each one's bits from alloc (NULL: no memory).
 * Vector fonts are left out. */
int ReadFontFile(const BYTE *data,DWORD size,FileFont *fonts,int max,void *(*alloc)(DWORD));
#endif
