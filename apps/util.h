/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * Shared pieces of the file utilities (the Makefile's FILE_APPS): the
 * messages of the MS-DOS 4 common parser and extended errors as those
 * utilities give them, file specifications split into a directory and a
 * name, directory walks and lines from a handle.
 */
#ifndef APP_UTIL_H
#define APP_UTIL_H
#include "runtime.h"
/* v4.0 MESSAGES/USA-MS.MSG, class PARSE. */
enum {PARSE_TOO_MANY=1,PARSE_MISSING,PARSE_SWITCH,PARSE_KEYWORD,PARSE_RANGE=6,PARSE_VALUE,
      PARSE_FORMAT=9,PARSE_PARAMETER,PARSE_COMBINATION};
/* The message, then " - " and the argument when there is one, to standard error. */
void parse_error(unsigned,const char *);
/* An extended error's message (dos_error), the same way. */
void extended_error(int,const char *);
/* print to standard error between the two calls. */
void to_stderr(int);
/* APPEND /X off for this program: utilities that look at the files
 * themselves (ATTRIB, TREE, REPLACE, XCOPY, BACKUP, RESTORE) must not find
 * others. */
void append_files_only(void);
/* The next word of a command line (blanks, tabs, commas, semicolons and '='
 * separate them, as COMMAND.COM's do), copied to out; NULL at the end. A
 * switch ends at the next '/', so "/S/V" are two words. */
const char *next_arg(const char *,char *,unsigned);
/* A file specification as DOS resolves it: its directory (with the drive and
 * a final backslash) and its last component, *.* when the specification
 * names a directory and dir_ok is set. */
int file_spec(const char *,char dir[DOS_PATH_MAX],char name[13],int dir_ok);
/* A target name from a pattern whose wildcards take the source name's
 * characters, as COPY's do (*.BAK for NAME.TXT is NAME.BAK). */
void map_name(const char *pattern,const char *name,char out[13]);
/* Each entry of dir matching pattern with attributes among attrs (and
 * none other than archive and read-only unless asked for), "." and ".."
 * left out; with recurse, every subdirectory's too, before or after the
 * directory's own entries. fn's nonzero result ends the walk. */
typedef int (*WalkFn)(void *,const char *dir,const DosFind *);
int walk(const char *dir,const char *pattern,u8 attrs,int recurse,int subdirs_first,WalkFn,void *);
/* A file's bytes, date and time to a file made anew (replacing one there)
 * with the archive attribute; nothing is left of it after an error, which
 * is DE_FULL when the disk fills. */
int copy_file(const char *from,const char *to);
/* A directory and those above it made where missing. */
int make_dirs(const char *dir);
/* A date in the country's order (separated by '-', '/' or '.', a two-digit
 * year being 1980-2079) or a time of day (hh:mm[:ss], ':' or the country's
 * separator) as a directory entry has it; 0 when it is not one. */
int parse_date(const char *,u16 *);
int parse_time(const char *,u16 *);
/* Lines read through a handle, CR LF or LF ended (without them); a last
 * line without one counts too. 1 for a line, 0 at the end. */
typedef struct {unsigned handle; u32 at,end; int eof; u8 data[4096];} LineReader;
void lines_open(LineReader *,unsigned);
int lines_next(LineReader *,char *,unsigned,unsigned *);
#endif
