// SPDX-License-Identifier: GPL-2.0-or-later
// __chkstk for WDK-built IA-64 code: called with br.call b7 and the frame
// size in r26 before a large frame is allocated. Task stacks are committed
// memory without guard pages, so there is nothing to probe.
	.text
	.global __chkstk
	.proc __chkstk
__chkstk:
	br.ret.sptk.few b7
	.endp __chkstk
