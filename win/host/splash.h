/* SPDX-License-Identifier: GPL-2.0-or-later
 * WIN.COM's start-up screen (splash.c). */
#ifndef SPLASH_H
#define SPLASH_H
#include "runtime.h"
/* Draws the screen on the display claimed; USER's desktop replaces it. */
void splash_show(const IoServices *io);
#endif
