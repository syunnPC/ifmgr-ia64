/* SPDX-License-Identifier: GPL-2.0-or-later
 * Mathematical functions in double precision (win/crt/math.c). */
#ifndef _MATH_H
#define _MATH_H
typedef union {unsigned __int64 _bits; double _value;} _HUGE_T;
extern const _HUGE_T _HUGE_U; /* an infinity */
#define HUGE_VAL (_HUGE_U._value)
#define _HUGE HUGE_VAL
#define M_PI 3.14159265358979323846
#define M_E 2.7182818284590452354
struct _exception {int type; char *name; double arg1,arg2,retval;};
struct _complex {double x,y;};
#define complex _complex
#define exception _exception
double acos(double);
double asin(double);
double atan(double);
double atan2(double,double);
double cos(double);
double sin(double);
double tan(double);
double cosh(double);
double sinh(double);
double tanh(double);
double exp(double);
double frexp(double,int *);
double ldexp(double,int);
double log(double);
double log10(double);
double modf(double,double *);
double pow(double,double);
double sqrt(double);
double ceil(double);
double fabs(double);
double floor(double);
double fmod(double,double);
double hypot(double,double);
double cabs(struct _complex);
#define _hypot hypot
#define _cabs cabs
#endif
