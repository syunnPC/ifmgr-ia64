/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FP_H
#define FP_H
#define FP_SIGNIFICANT 0
#define FP_FIXED 1
#define FP_FINITE 0
#define FP_INF 1
#define FP_NAN 2
int _fp_digits(double,int,int,char *,int *);
int _fp_class(double);
#endif
