/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_DEVICE_PRIVATE_H
#define DOS_DEVICE_PRIVATE_H
#include "dos_device.h"
#include "dos_block.h"
#define DOS_MAX_DEVICES 32
#define DOS_MAX_DRIVERS 16
#define DOS_CON_DEVICE 1
#define DOS_NUL_DEVICE 2
#define DOS_AUX_DEVICE 3
#define DOS_PRN_DEVICE 4
void device_reset(void);
int device_find(const char *);
/* A driver's name (character or block) in upper case: one to eight printable
 * characters, none of them a name delimiter. Its length; 0 when invalid. */
unsigned device_name(const char *,char[9]);
const DosDeviceSpec *device_spec(unsigned);
int device_register(const DosDeviceSpec *);
int device_info(unsigned,DosDeviceInfo *);
int device_register_block(const DosBlockSpec *);
int device_pending(unsigned);
int device_request(unsigned,DosDeviceRequest *);
void device_reference(unsigned,int);
int device_begin(unsigned);
int device_commit(unsigned,const IoServices *,const char *);
int device_load_error(unsigned);
unsigned device_loading(void);
int device_configure(void);
void device_cancel(unsigned);
int device_finish(unsigned);
int dos_driver_request(const DosDeviceSpec *,DosDeviceRequest *);
int dos_load_driver(const char *,const char *);
int dos_finish_drivers(void);
int dos_drivers_pending(void);
int dos_bind_standard_devices(void);
int platform_module_load(const void *,u32,u64 *);
int platform_module_unload(u64);
const IoServices *platform_io_services(void);
#endif
