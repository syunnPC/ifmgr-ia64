/* SPDX-License-Identifier: GPL-2.0-or-later
 * Dynamic data exchange: messages, their data structures, and (as in
 * Win32) the packing of the two values of WM_DDE_ACK, WM_DDE_ADVISE,
 * WM_DDE_DATA and WM_DDE_POKE into lParam, which here is too narrow for a
 * handle beside an atom. WM_DDE_EXECUTE's lParam is the command handle.
 */
#ifndef DDE_H
#define DDE_H
#include <windows.h>
#define WM_DDE_FIRST 0x03E0
#define WM_DDE_INITIATE (WM_DDE_FIRST)
#define WM_DDE_TERMINATE (WM_DDE_FIRST+1)
#define WM_DDE_ADVISE (WM_DDE_FIRST+2)
#define WM_DDE_UNADVISE (WM_DDE_FIRST+3)
#define WM_DDE_ACK (WM_DDE_FIRST+4)
#define WM_DDE_DATA (WM_DDE_FIRST+5)
#define WM_DDE_REQUEST (WM_DDE_FIRST+6)
#define WM_DDE_POKE (WM_DDE_FIRST+7)
#define WM_DDE_EXECUTE (WM_DDE_FIRST+8)
#define WM_DDE_LAST (WM_DDE_FIRST+8)
typedef struct {unsigned short bAppReturnCode:8,reserved:6,fBusy:1,fAck:1;} DDEACK;
typedef struct {unsigned short reserved:14,fDeferUpd:1,fAckReq:1; short cfFormat;} DDEADVISE;
typedef struct {unsigned short unused:12,fResponse:1,fRelease:1,reserved:1,fAckReq:1; short cfFormat; BYTE Value[1];} DDEDATA;
typedef struct {unsigned short unused:13,fRelease:1,fReserved:2; short cfFormat; BYTE Value[1];} DDEPOKE;
typedef UINT_PTR *PUINT_PTR;
WINUSERAPI LPARAM WINAPI PackDDElParam(UINT,UINT_PTR,UINT_PTR);
WINUSERAPI BOOL WINAPI UnpackDDElParam(UINT,LPARAM,PUINT_PTR,PUINT_PTR);
WINUSERAPI BOOL WINAPI FreeDDElParam(UINT,LPARAM);
WINUSERAPI LPARAM WINAPI ReuseDDElParam(LPARAM,UINT,UINT,UINT_PTR,UINT_PTR);
#endif
