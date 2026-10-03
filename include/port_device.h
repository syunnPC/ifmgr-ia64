/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_PORT_DEVICE_H
#define DOS_PORT_DEVICE_H
#include "dos_device.h"
#define DOS_PORT_ABI 1U
#define DOS_PORT_DTR 1U
#define DOS_PORT_RTS 2U
#define DOS_PORT_CTS_FLOW 4U
#define DOS_PORT_LOOPBACK 8U
enum {DOS_PARITY_NONE,DOS_PARITY_ODD,DOS_PARITY_EVEN,DOS_PARITY_MARK,DOS_PARITY_SPACE};
#define DOS_STOP_ONE 1U
#define DOS_STOP_TWO 2U
#define DOS_STOP_ONE_HALF 15U
#define DOS_PORT_RX_READY 1U
#define DOS_PORT_TX_READY 2U
#define DOS_PORT_TX_EMPTY 4U
#define DOS_PORT_CTS 8U
#define DOS_PORT_DSR 16U
#define DOS_PORT_CARRIER 32U
#define DOS_PORT_PAPER_OUT 64U
#define DOS_PORT_SELECTED 128U
#define DOS_PORT_ERROR 256U
#define DOS_PORT_FAULTED 512U
#define DOS_PORT_BUFFERED 1024U /* IO.SYS timer-samples this UART into a ring. */
/* PORTDRV.SYS arguments: COMn=base (2F8/3E8/2E8 UARTs), LPTn=base (378/278)
 * or COMn=EFIu, binding COMn to firmware serial unit u (0-15) published by
 * IO.SYS through EFI Serial I/O. Consoles are never published as units.
 * AH=4402 reads DosPortInfo; AH=4403 writes DosPortConfig. Configuration is
 * shared by all handles of the physical device. Timeouts apply per byte;
 * zero means no waiting. Failed I/O retains its completed byte count.
 * A register-write failure faults the device until it is reloaded.
 * kind is IO_PORT_UART/PRINTER with an I/O base, or IO_PORT_SERIAL with the
 * firmware unit in base. buffered counts received bytes held by IO.SYS or
 * PORTDRV but not yet read. Firmware units report synthesized 16550-style
 * line/modem status; their receive errors carry no cause and set bit 7. */
typedef struct {
    u32 size,version,baud,timeout_us,data_bits,parity,stop_bits,flags;
} DosPortConfig;
typedef struct {
    u32 size,kind,base,status;
    u32 line_status,modem_status,receive_errors,buffered;
    DosPortConfig config;
} DosPortInfo;
#define DOS_PORT_CLEAR_INPUT 0x8000U /* AH=440C, no data: discard input/errors. */
#endif
