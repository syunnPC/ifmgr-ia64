/* SPDX-License-Identifier: GPL-2.0-or-later
 * PEBAD.EXE: imports a symbol PEDLL.DLL does not export.
 */
__declspec(dllimport) int pedll_missing(void);
int pebad_main(void) {return pedll_missing();}
