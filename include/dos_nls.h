/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_NLS_H
#define DOS_NLS_H
#include "base.h"
#define DOS_NLS_CURRENT 65535U
/* Native country information has no 16:16 case-map callback. Use AH=65h.
 * date_order: 0=MDY, 1=DMY, 2=YMD; time_format: 0=12h, 1=24h.
 * Currency flags retain DOS bits: suffix, space, symbol replaces decimal.
 * Strings contain OEM bytes; country/page come from the directory record. */
typedef struct {
    u32 size;
    u16 country,code_page,date_order;
    u8 currency[5],thousands[2],decimal[2],date_separator[2],time_separator[2];
    u8 currency_flags,currency_digits,time_format,list_separator[2],reserved[12];
} DosCountryInfo;
_Static_assert(sizeof(DosCountryInfo)==40,"native country ABI");
/* AH=38h: AL=0 current, 1..254 country, FFh country in BX.
 * Get: DX=DosCountryInfo, CX=capacity; BX=country, CX=bytes on success.
 * Set: DX=FFFFh (or UINT64_MAX), retains the active code page.
 * AH=65h: BX=page, DX=country (FFFFh=current); DI=buffer, CX=capacity.
 * AL=1 copies DosCountryInfo. AL=2/4/5/6/7 copies the original uppercase/
 * filename-uppercase/filename-characters/collation/DBCS table payload.
 * CX returns its byte length, including on DE_FUNCTION for insufficient
 * capacity; no partial copy. An empty DBCS table is two zero bytes.
 * AL=20h/A0h uppercases DL (general/filename); AL=21h/A1h uppercases
 * CX bytes at DX. AL=22h/A2h uses a NUL string at DX with CX as its bounded
 * capacity. Case calls use the current profile and preserve DBCS pairs.
 * AH=63h/AL=0: SI points to the active DBCS lead ranges, CX=byte length.
 * The read-only SI view has kernel lifetime; its contents change on switch.
 * AH=66h/AL=1: BX=active page, DX=boot default. AL=2 selects BX for both NLS
 * and CON; it fails with DOS_CP_SYSTEM_NOT_PREPARED and changes neither when
 * CON has not prepared the page. Country/page are global across tasks.
 */
/* CON code pages (DOS 4 DISPLAY.SYS model, built into MSDOS.SYS). AH=440Ch
 * with CH=03h (00h accepted, as NLSFUNC sends) and CL=function; DX=packet,
 * SI=packet bytes; AX returns bytes stored by queries. Packets keep the DOS
 * layout of little-endian words:
 *   4Ch prepare start: flags 0, length 2+2n, n, page[n]. FFFFh keeps that
 *       slot; n=0 is REFRESH. Then stream the table file with AH=4403h.
 *   4Dh prepare end: validates and commits every requested page atomically.
 *   4Ah select: length 2, page. 6Ah query selected: returns length 2, page.
 *   6Bh query list: length, 1, 437, DOS_CP_PREPARED_MAX, slot pages (FFFFh empty).
 * Selecting, preparing and 4403h writes require a writable CON handle.
 * Page 437 is the hardware page. Prepared pages come from a native table
 * file (EFI.CPI): DOS_CP_FILE_MAGIC, version 1, count, then 12-byte entries
 * {page, kind 1=SBCS/2=DBCS, 0, offset, size}. SBCS data is 256 UTF-16 code
 * units; DBCS data is 16 bytes of DOS lead ranges, 256 single-byte units and
 * 256 units per lead byte. Zero means unmapped (byte 0 maps to U+0000). */
#define DOS_CP_CATEGORY 0x03U
#define DOS_CP_SELECT 0x4aU
#define DOS_CP_PREPARE_START 0x4cU
#define DOS_CP_PREPARE_END 0x4dU
#define DOS_CP_QUERY 0x6aU
#define DOS_CP_QUERY_LIST 0x6bU
#define DOS_CP_HARDWARE 437U
#define DOS_CP_PREPARED_MAX 8U
#define DOS_CP_FILE_MAGIC "\xffUNICODE"
/* DOS 4 device status codes as INT 21h errors (19 + status). */
#define DOS_CP_NOT_PREPARED 26U /* status 07h: page not prepared */
#define DOS_CP_NOT_IN_FILE 27U  /* status 08h: page missing from the table file */
#define DOS_CP_DEVICE_ERROR 29U /* status 0Ah: invalid prepare list */
#define DOS_CP_BAD_FILE 31U     /* status 0Ch: invalid data or no prepare start */
#define DOS_CP_SYSTEM_NOT_PREPARED 65U /* AH=6602h, as NLSFUNC reports */
#endif
