/* SPDX-License-Identifier: GPL-2.0-or-later
 * The accessories' Help menu (winapp.h), through WinHelp: Index (also F1)
 * the file's contents, Keyboard, Commands and Procedures its [MAP] topics,
 * Using Help WINHELP.HLP. C89.
 */
#include "winapp.h"
BOOL HelpCommand(HWND owner,UINT id,LPCSTR file) {
    switch(id) {
    case IDM_HELPINDEX: WinHelp(owner,file,HELP_INDEX,0); return TRUE;
    case IDM_HELPKEYBOARD: WinHelp(owner,file,HELP_CONTEXT,HELPMAP_KEYBOARD); return TRUE;
    case IDM_HELPCOMMANDS: WinHelp(owner,file,HELP_CONTEXT,HELPMAP_COMMANDS); return TRUE;
    case IDM_HELPPROCEDURES: WinHelp(owner,file,HELP_CONTEXT,HELPMAP_PROCEDURES); return TRUE;
    case IDM_HELPUSING: WinHelp(owner,"WINHELP.HLP",HELP_HELPONHELP,0); return TRUE;
    }
    return FALSE;
}
