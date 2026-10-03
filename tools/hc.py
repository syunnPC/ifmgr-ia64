#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile Windows 3.0 help from HPJ projects and RTF topics.

Usage: hc.py PROJECT.HPJ [OUTPUT.HLP]

HPJ sections: [OPTIONS] TITLE and CONTENTS/INDEX (default: first topic),
[FILES] ordered RTF inputs, [MAP] numeric contexts (including #define and
#include), [ALIAS] context aliases, [BITMAPS] pictures. ROOT/BMROOT set paths.

RTF topics use \\page separators and #/$/K/+ footnotes for contexts, titles,
semicolon-separated keywords and browse order (group:order or numeric).
Hidden \\v context text after double underline/strike creates jumps;
single underline creates popups, or plain underline without a context.
Support paragraph/layout/font/color controls and bmc/bml inline 1/4-bit BMPs.
Skip other destinations, including stylesheets, metadata and embedded pictures.
Convert twips to half points."""
from pathlib import Path
import re
import struct
import sys
from hlpwrite import hlp_file, hlp_numbers

def parse_rtf(text):
    """Tokens of an RTF text: ('{',), ('}',), ('word', name, parameter or None), ('symbol', c) and
    ('text', string)."""
    tokens=[]; i=0; n=len(text); plain=[]
    def flush():
        if plain: tokens.append(('text',''.join(plain))); plain.clear()
    while i<n:
        c=text[i]
        if c in '{}':
            flush(); tokens.append((c,)); i+=1
        elif c=='\\':
            if i+1<n and text[i+1].isalpha():
                m=re.compile(r'\\([a-zA-Z]+)(-?\d+)? ?').match(text,i)
                flush(); tokens.append(('word',m.group(1),int(m.group(2)) if m.group(2) else None)); i=m.end()
            elif text[i+1:i+2]=="'":
                plain.append(bytes([int(text[i+2:i+4],16)]).decode('cp1252')); i+=4
            elif text[i+1:i+2] in ('\r','\n'):
                flush(); tokens.append(('word','par',None)); i+=2
            else:
                flush(); tokens.append(('symbol',text[i+1:i+2])); i+=2
        elif c in '\r\n':
            i+=1
        else:
            plain.append(c); i+=1
    flush()
    return tokens

class Topic:
    def __init__(self):
        self.contexts=[]; self.title=''; self.keywords=[]; self.browse=None
        self.records=[]  # (paragraph information, [pieces])

def bmp_picture(path):
    """A 1- or 4-bit BMP as the help file's SHG picture (one DIB, as hlpwrite's)."""
    cshort,cushort,clong,culong=hlp_numbers()
    data=Path(path).read_bytes()
    if data[:2]!=b'BM': raise SystemExit(f'{path}: not a BMP')
    offset,=struct.unpack_from('<I',data,10)
    size,width,height,planes,bits,compression=struct.unpack_from('<IiiHHI',data,14)
    if compression or bits not in (1,4) or height<=0: raise SystemExit(f'{path}: only uncompressed 1- and 4-bit BMPs')
    colours=(offset-14-size)//4
    palette=data[14+size:14+size+4*colours]
    row=(width*bits+31)//32*4
    pixels=data[offset:offset+row*height]
    head=bytes([6,0])+culong(96)+culong(96)+cushort(1)+cushort(bits)+culong(width)+culong(height)+culong(colours)+culong(0)+culong(len(pixels))+culong(0)
    head+=struct.pack('<II',len(head)+8+len(palette),0)
    return struct.pack('<HHI',0x506c,1,8)+head+palette+pixels

def compile_project(project):
    project=Path(project); base=project.parent
    sections={}; section=None
    for line in project.read_text(encoding='cp1252').splitlines():
        line=line.split(';',1)[0].strip()
        if not line: continue
        if line.startswith('[') and line.endswith(']'): section=line[1:-1].upper(); sections.setdefault(section,[]); continue
        if section: sections[section].append(line)
    options={}
    for line in sections.get('OPTIONS',[]):
        if '=' in line:
            key,value=line.split('=',1); options[key.strip().upper()]=value.strip()
    root=base/options.get('ROOT','.'); bmroot=base/options.get('BMROOT',options.get('ROOT','.'))
    fonts=[]; topics=[]; pictures=[]; picture_names={}
    def font_index(face,half,attributes,family,colour):
        entry=(face,half,attributes,family,colour)
        if entry not in fonts: fonts.append(entry)
        return fonts.index(entry)
    for name in sections.get('FILES',[]):
        tokens=parse_rtf((root/name).read_text(encoding='cp1252'))
        font_table={}; colour_table=[(0,0,0)]
        # State saved and restored with the groups.
        state={'f':0,'fs':20,'b':0,'i':0,'ul':0,'uldb':0,'v':0,'cf':0,'dest':None}
        paragraph={}
        stack=[]; topic=Topic(); pieces=[]; current_font=[None]
        hotspot=[None]  # ('jump'|'popup', text pieces list) while underlined text runs
        pending_hidden=[None]
        footnote=[None]
        def piece_font():
            face,family=font_table.get(state['f'],('Helv',2))
            attributes=(1 if state['b'] else 0)|(2 if state['i'] else 0)
            n=font_index(face,state['fs'],attributes,family,colour_table[state['cf']] if state['cf']<len(colour_table) else (0,0,0))
            if current_font[0]!=n: pieces.append(('font',n)); current_font[0]=n
        def end_paragraph(kind):
            nonlocal pieces
            if kind=='line': pieces.append(('break',)); return
            # A hotspot's hidden context may end with the paragraph inside it.
            close_hotspot(); drop_hotspot()
            topic.records.append((dict(paragraph),pieces)); pieces=[]; current_font[0]=None
        def add_text(s):
            if not s: return
            if state['v']:
                pending_hidden[0]=(pending_hidden[0] or '')+s; return
            if not (state['uldb'] or state['ul']): drop_hotspot()
            # Literal \{bmc FILE\} picture references in the text.
            while True:
                m=re.search(r'\{bm[clr] ([^}]+)\}',s)
                if not m: break
                add_text(s[:m.start()]); picture(m.group(1)); s=s[m.end():]
            if not s: return
            piece_font()
            kind='jump' if state['uldb'] else 'popup' if state['ul'] else None
            if kind and not hotspot[0]: hotspot[0]=kind; pieces.append([kind,None])
            pieces.append(s.encode('cp1252'))
        def picture(file):
            file=file.strip()
            if file not in picture_names:
                picture_names[file]=len(pictures); pictures.append(bmp_picture(bmroot/file))
            piece_font(); pieces.append(('picture',picture_names[file]))
        def drop_hotspot():
            # Underlined text with no hidden context after it is only underlined.
            if hotspot[0] and pending_hidden[0] is None:
                for k in range(len(pieces)-1,-1,-1):
                    if isinstance(pieces[k],list) and pieces[k][1] is None: del pieces[k]; break
                hotspot[0]=None
        def close_hotspot():
            # Hidden text with no hotspot before it (such as a picture's,
            # which is left out) is dropped.
            if not hotspot[0]: pending_hidden[0]=None
            if hotspot[0] and pending_hidden[0] is not None:
                for k in range(len(pieces)-1,-1,-1):
                    if isinstance(pieces[k],list) and pieces[k][1] is None:
                        pieces[k][1]=pending_hidden[0].strip(); break
                pieces.append(('end',)); hotspot[0]=None; pending_hidden[0]=None
        i=0
        while i<len(tokens):
            t=tokens[i]; i+=1
            kind=t[0]
            if kind=='{':
                stack.append(dict(state)); state['footnote_group']=False
                # Destinations skipped whole.
                if i<len(tokens) and tokens[i][0]=='symbol' and tokens[i][1]=='*' or \
                   i<len(tokens) and tokens[i][0]=='word' and tokens[i][1] in ('stylesheet','info','pict','header','footer'):
                    depth=1
                    while depth and i<len(tokens):
                        depth+=1 if tokens[i][0]=='{' else -1 if tokens[i][0]=='}' else 0; i+=1
                    state=stack.pop()
                continue
            if kind=='}':
                was_hidden=state['v']; was_underlined=state['uldb'] or state['ul']
                # The footnote ends with its own group, not one nested in it.
                if state['dest']=='footnote' and state.get('footnote_group') and footnote[0]:
                    mark,value=footnote[0][0],footnote[0][1].strip()
                    if value[:1]==mark: value=value[1:].strip()
                    if mark=='#': topic.contexts.append(value)
                    elif mark=='$': topic.title=value
                    elif mark=='K': topic.keywords+=[k.strip() for k in value.split(';') if k.strip()]
                    elif mark=='+': topic.browse=value
                    footnote[0]=None
                state=stack.pop() if stack else state
                if was_hidden and not state['v']: close_hotspot()
                elif was_underlined and not (state['uldb'] or state['ul']) and hotspot[0] and pending_hidden[0] is None:
                    pass
                continue
            if kind=='text':
                if state['dest']=='fonttbl':
                    m=re.match(r'\s*(.*?);',t[1])
                    if m: font_table[state['font_number']]=(m.group(1).strip(),state.get('font_family',2))
                    continue
                if state['dest']=='colortbl':
                    for _ in t[1].split(';')[:-1]:
                        colour_table.append((state.get('red',0),state.get('green',0),state.get('blue',0)))
                        state['red']=state['green']=state['blue']=0
                    continue
                if state['dest']=='footnote':
                    footnote[0]=(footnote[0][0],footnote[0][1]+t[1]); continue
                if state['dest']: continue
                text_run=t[1]
                # A footnote mark is the character before {\footnote.
                if i<len(tokens) and tokens[i][0]=='{' and i+1<len(tokens) and tokens[i+1][:2]==('word','footnote'):
                    mark=text_run[-1]; text_run=text_run[:-1]
                    if text_run: add_text(text_run)
                    footnote[0]=(mark,''); continue
                add_text(text_run)
                continue
            if kind=='symbol':
                c=t[1]
                if state['dest']=='footnote' and footnote[0]: footnote[0]=(footnote[0][0],footnote[0][1]+(c if c in '{}\\' else '')); continue
                if state['dest']: continue
                if c in '{}\\': add_text(c)
                elif c=='~': add_text('\xa0')
                elif c=='-': pass
                continue
            name,value=t[1],t[2]
            on=value is None or value!=0
            if name=='fonttbl': state['dest']='fonttbl'
            elif name=='colortbl': state['dest']='colortbl'
            elif name=='footnote': state['dest']='footnote'; state['footnote_group']=True
            elif state['dest']=='fonttbl':
                if name=='f': state['font_number']=value
                elif name in ('fnil','froman','fswiss','fmodern','fscript','fdecor'):
                    state['font_family']={'fnil':0,'froman':1,'fswiss':2,'fmodern':3,'fscript':4,'fdecor':5}[name]
            elif state['dest']=='colortbl':
                if name in ('red','green','blue'): state[name]=value or 0
            elif state['dest']=='footnote':
                if name=='par': footnote[0]=(footnote[0][0],footnote[0][1]+' ')
            elif state['dest']: pass
            elif name=='page':
                if pieces: end_paragraph('par')
                if topic.records or topic.contexts or topic.title: topics.append(topic)
                topic=Topic(); current_font[0]=None
            elif name=='par': end_paragraph('par')
            elif name=='line': end_paragraph('line')
            elif name=='tab': piece_font(); pieces.append(('tab',))
            elif name=='pard': paragraph.clear()
            elif name in ('li','ri','fi','sb','sa','sl'):
                key={'li':'left','ri':'right','fi':'first','sb':'above','sa':'below','sl':'lines'}[name]
                paragraph[key]=(value or 0)//10
            elif name=='qc': paragraph['align']='centre'
            elif name=='tx': paragraph.setdefault('tabs',[]).append((value or 0)//10)
            elif name=='box' or name.startswith('brdr'): paragraph['border']=paragraph.get('border',0)|(0x01 if name=='box' else 0x10 if name=='brdrb' else 0x01)
            elif name=='plain': state.update(b=0,i=0,ul=0,uldb=0,v=0,f=0,fs=20,cf=0)
            elif name=='b': state['b']=on
            elif name=='i': state['i']=on
            elif name in ('uldb','strike'): state['uldb']=on
            elif name=='ul': state['ul']=on
            elif name=='ulnone': state['ul']=state['uldb']=0
            elif name=='v':
                was=state['v']; state['v']=on
                if was and not on: close_hotspot()
            elif name=='f': state['f']=value or 0
            elif name=='fs': state['fs']=value or 20
            elif name=='cf': state['cf']=value or 0
        if pieces: end_paragraph('par')
        if topic.records or topic.contexts or topic.title: topics.append(topic)
    # Contexts to topic numbers; hotspots to commands.
    aliases={}
    for line in sections.get('ALIAS',[]):
        if '=' in line:
            a,b=line.split('=',1); aliases[a.strip().lower()]=b.strip().lower()
    contexts={}
    for n,topic in enumerate(topics):
        for c in topic.contexts: contexts[c.lower()]=n
    def lookup(context):
        c=context.lower(); c=aliases.get(c,c)
        if c not in contexts: raise SystemExit(f'{project}: no topic for context "{context}"')
        return contexts[c]
    # The browse sequences: topics of a group in order, linked both ways.
    groups={}
    for n,topic in enumerate(topics):
        if topic.browse:
            group,_,order=topic.browse.rpartition(':')
            groups.setdefault(group,[]).append((order,n))
    previous=[None]*len(topics); following=[None]*len(topics)
    for members in groups.values():
        members.sort()
        for (_,a),(_,b) in zip(members,members[1:]): following[a]=b; previous[b]=a
    out_topics=[]
    for n,topic in enumerate(topics):
        records=[]
        for info,pieces in topic.records:
            converted=[]
            for p in pieces:
                if isinstance(p,list): converted.append((p[0],lookup(p[1])))
                else: converted.append(p)
            records.append((info,converted))
        out_topics.append((topic.title,previous[n],following[n],records))
    keywords={}
    for n,topic in enumerate(topics):
        for k in topic.keywords: keywords.setdefault(k,[]); keywords[k].append(n)
    maps=[]
    def map_lines(lines):
        for line in lines:
            if line.startswith('#include'):
                header=base/line[len('#include'):].strip().strip('<>"')
                map_lines([l.split('//',1)[0].strip() for l in header.read_text(encoding='cp1252').splitlines()]); continue
            parts=line.replace('#define','').split()
            if len(parts)>=2: maps.append((int(parts[1],0),lookup(parts[0])))
    map_lines(sections.get('MAP',[]))
    # Windows 3.0's INDEX names the contents topic as 3.1's CONTENTS does.
    start=options.get('CONTENTS',options.get('INDEX'))
    contents=lookup(start) if start else 0
    if not fonts: fonts.append(('Helv',20,0,2,(0,0,0)))
    return hlp_file(options.get('TITLE',''),fonts,out_topics,contents=contents,keywords=sorted(keywords.items()),
                    maps=maps,pictures=pictures)

if __name__=='__main__':
    if len(sys.argv) not in (2,3): raise SystemExit('usage: hc.py PROJECT.HPJ [OUTPUT.HLP]')
    output=Path(sys.argv[2]) if len(sys.argv)==3 else Path(sys.argv[1]).with_suffix('.HLP')
    output.write_bytes(compile_project(sys.argv[1]))
