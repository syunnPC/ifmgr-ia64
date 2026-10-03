/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_DEVICE_API_H
#define DOS_DEVICE_API_H
#include "io.h"
#define DOS_DEVICE_ABI 1U
#define DOS_DEVICE_CHAR 0x8000U
#define DOS_DEVICE_IOCTL 0x4000U
#define DOS_DEVICE_OPEN_CLOSE 0x0800U
#define DOS_DEVICE_GENERIC 0x0040U
#define DOS_DEVICE_CAN_READ 1U
#define DOS_DEVICE_CAN_WRITE 2U
#define DOS_DEVICE_WAIT 1U
/* DOS request command numbers; FINISH is a native lifetime extension. */
enum {DOS_DEV_INIT=0,DOS_DEV_IOCTL_READ=3,DOS_DEV_READ=4,DOS_DEV_PEEK=5,
      DOS_DEV_INPUT_STATUS=6,DOS_DEV_INPUT_FLUSH=7,DOS_DEV_WRITE=8,
      DOS_DEV_OUTPUT_STATUS=10,DOS_DEV_OUTPUT_FLUSH=11,DOS_DEV_IOCTL_WRITE=12,
      DOS_DEV_OPEN=13,DOS_DEV_CLOSE=14,DOS_DEV_GENERIC_IOCTL=19,DOS_DEV_FINISH=256};
typedef struct {
    u32 size,command,pid,mode;
    void *cookie; /* OPEN sets it; duplicates and inherited handles share it. */
    void *buffer;
    u32 count,transferred,flags,ready;
    u64 control,argument;
    const IoServices *io; /* INIT only; valid until FINISH returns. */
    const char *arguments; /* INIT only; copy any retained text. */
} DosDeviceRequest;
typedef struct {
    u32 version,size,attributes,capabilities;
    char name[9]; u8 reserved[7]; /* Uppercase ASCII, one to eight characters. */
    void *context;
    int (*request)(void *,DosDeviceRequest *);
} DosDeviceSpec;
typedef struct {
    u32 size,index,attributes,capabilities;
    u32 open_descriptions,reserved;
    char name[9]; u8 padding[7];
} DosDeviceInfo;
/* Entry shims register specs only while DEVICE is loading. DOS copies them,
 * then sends INIT after the image is resident. On failure all its registrations
 * are removed. Callbacks run synchronously inside DOS and must not reenter it.
 * They may use IO.SYS. READ/WRITE must report partial transfers even on error;
 * PEEK must not consume input. FINISH releases every resource, even on error.
 * OPEN failure must clean up its own partial allocation. CLOSE is sent once
 * when the last reference disappears, including task destruction and abort.
 */
#endif
