/* SPDX-License-Identifier: GPL-2.0-or-later
 * WINHELP: reading Windows 3.0 and 3.1 help files (hlpfile.c). A topic
 * comes out as paragraphs of runs (text in a font, tabs, line breaks,
 * pictures), some of them in hotspots that jump to a topic, pop one up or
 * run a macro. Topics are numbered in the file's order; jumps, keywords and
 * the [MAP] numbers lead to those numbers.
 */
#ifndef HLPFILE_H
#define HLPFILE_H
#define HELP_NO_TOPIC 0xffffffffUL

/* A font of |FONT: the attributes are bold 1, italic 2, underline 4,
 * strikeout 8, double underline 16, small capitals 32. */
typedef struct {char face[32]; BYTE attributes,half_points,family; COLORREF color;} HelpFont;
#define HELP_BOLD 1
#define HELP_ITALIC 2
#define HELP_UNDERLINE 4
#define HELP_STRIKEOUT 8
#define HELP_DOUBLE 16

#define RUN_TEXT 0    /* text[at..at+length) */
#define RUN_TAB 1
#define RUN_BREAK 2   /* a line break within the paragraph */
#define RUN_PICTURE 3 /* pictures[at]; place: 0 in the line, 1 at the left, 2 at the right */
typedef struct {BYTE kind,place; WORD font; short spot; DWORD at,length;} HelpRun;
/* Paragraph metrics are in half points; tab kinds 0 left, 1 right, 2 centre. */
#define HELP_LEFT 0
#define HELP_RIGHT 1
#define HELP_CENTER 2
#define HELP_TABS 32
typedef struct {
    int first_run,runs;
    BYTE align,border; short above,below,lines,left,right,first;
    int tabs; WORD tab[HELP_TABS]; BYTE tab_kind[HELP_TABS];
    short column; /* a table's column (its cells side by side), or -1 */
    BYTE new_cell,relative; /* the cell's first paragraph; its place in 32767ths of the width */
    short cell_left,cell_width; /* the cell's place in half points (or 32767ths) */
} HelpPara;
#define SPOT_JUMP 0
#define SPOT_POPUP 1
#define SPOT_MACRO 2
/* A hotspot: a jump or popup to a topic (its number; for another file's,
 * named in file, the target as the file has it: HelpByTarget finds it there),
 * or a macro. Invisible ones have no colour. */
typedef struct {BYTE kind; BOOL visible; DWORD topic; char file[80]; char macro[128];} HelpSpot;
/* A picture: a packed DIB (aligned: the header, colours and bits) or a
 * metafile's bits with its mapping mode and size in hundredths of a millimetre. */
typedef struct {BOOL metafile; int mm,width,height; BYTE *data; DWORD size;} HelpPicture;
typedef struct {
    char title[128]; DWORD number,previous,next; /* browse sequence: topic numbers or HELP_NO_TOPIC */
    HelpPara *paras; int para_count;
    HelpRun *runs; int run_count;
    char *text; DWORD text_length;
    HelpSpot *spots; int spot_count;
    HelpPicture *pictures; int picture_count;
} HelpTopic;

typedef struct HelpFile HelpFile;
HelpFile *HelpOpen(LPCSTR path);
void HelpClose(HelpFile *);
LPCSTR HelpTitle(const HelpFile *);
LPCSTR HelpPath(const HelpFile *);
int HelpFontCount(const HelpFile *);
const HelpFont *HelpFontAt(const HelpFile *,int);
DWORD HelpTopicCount(const HelpFile *);
LPCSTR HelpTopicTitle(const HelpFile *,DWORD number);
DWORD HelpContents(const HelpFile *);
DWORD HelpMapped(const HelpFile *,DWORD map);   /* HELP_CONTEXT's number, through [MAP] */
DWORD HelpByTarget(const HelpFile *,DWORD target); /* a jump's target (3.0 topic number, 3.1 context hash) */
DWORD HelpNamed(const HelpFile *,LPCSTR context); /* a context name */
HelpTopic *HelpReadTopic(HelpFile *,DWORD number);
void HelpFreeTopic(HelpTopic *);
/* Keywords in order, and the topics each leads to. */
int HelpKeywordCount(const HelpFile *);
LPCSTR HelpKeyword(const HelpFile *,int);
int HelpKeywordTopics(const HelpFile *,int keyword,DWORD *topics,int max);
int HelpFindKeyword(const HelpFile *,LPCSTR prefix); /* the first one starting with it, or -1 */
#endif
