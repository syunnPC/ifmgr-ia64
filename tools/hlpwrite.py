#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows 3.0 help files (.HLP) as HC30 lays them out, after helpdeco's helpfile.txt: the
internal file system, |SYSTEM, |FONT, |TOPIC in 2 KiB blocks with plain text, |TOMAP, |CTXOMAP,
the keyword B+ tree and |KWDATA, and bm0... pictures. Used by tools/hc.py and the tests."""
import struct

def hlp_numbers():
    """The compressed numbers of a help file's paragraph records: signed and unsigned, short and long."""
    def cshort(v):
        return bytes([(v+64)*2]) if -64<=v<64 else bytes([((v+16384)&127)*2+1,(v+16384)>>7])
    def cushort(v):
        return bytes([v*2]) if v<128 else bytes([(v&127)*2+1,v>>7])
    def clong(v):
        return struct.pack('<H',(v+16384)*2) if -16384<=v<16384 else struct.pack('<HH',((v+67108864)&32767)*2+1,(v+67108864)>>15)
    def culong(v):
        return struct.pack('<H',v*2) if v<32768 else struct.pack('<HH',(v&32767)*2+1,v>>15)
    return cshort,cushort,clong,culong

def hlp_btree(entries, page_size, key_size=None):
    """A B+ tree: the 38-byte header, then its pages. Entries that fit go in one leaf page; more
    are spread over leaves, linked both ways, under one index page (two levels), whose entries
    are each later leaf's first key (key_size(entry) bytes long) and that leaf's number."""
    leaves=[[]]; used=8
    for entry in entries:
        if used+len(entry)>page_size and leaves[-1]:
            leaves.append([]); used=8
        leaves[-1].append(entry); used+=len(entry)
    def leaf(i):
        page=struct.pack('<Hhhh',0,len(leaves[i]),i-1 if i else -1,i+1 if i+1<len(leaves) else -1)+b''.join(leaves[i])
        assert len(page)<=page_size, 'an entry larger than a page'
        return page+bytes(page_size-len(page))
    pages=[leaf(i) for i in range(len(leaves))]
    root,levels=0,1
    if len(leaves)>1:
        assert key_size, 'a B+ tree of more than one page needs its keys'
        index=struct.pack('<Hhh',0,len(leaves)-1,0)+b''.join(l[0][:key_size(l[0])]+struct.pack('<h',i) for i,l in enumerate(leaves) if i)
        assert len(index)<=page_size, 'too many leaves for one index page'
        pages.append(index+bytes(page_size-len(index))); root,levels=len(pages)-1,2
    return struct.pack('<HHH16shhhhhhI',0x293b,2,page_size,b'z4',0,0,root,-1,len(pages),levels,len(entries))+b''.join(pages)

def hlp_picture_dib(width, height, palette, pixel):
    """An SHG picture (a bm file): one 4-bit DIB, stored as it is, its colour table RGBQUADs."""
    cshort,cushort,clong,culong=hlp_numbers()
    row=(width*4+31)//32*4
    bits=b''
    for y in reversed(range(height)):
        line=bytearray(row)
        for x in range(width): line[x//2]|=pixel(x,y)<<(0 if x&1 else 4)
        bits+=bytes(line)
    colours=b''.join(struct.pack('<BBBB',b,g,r,0) for r,g,b in palette)
    head=bytes([6,0])+culong(96)+culong(96)+cushort(1)+cushort(4)+culong(width)+culong(height)+culong(len(palette))+culong(0)+culong(len(bits))+culong(0)
    head+=struct.pack('<II',len(head)+8+len(colours),0)
    return struct.pack('<HHI',0x506c,1,8)+head+colours+bits

def hlp_file(title, fonts, topics, contents=0, keywords=(), maps=(), pictures=()):
    """A Windows 3.0 help file (as HC30 lays one out): |SYSTEM, |FONT, |TOPIC in 2 KiB blocks with
    the text stored plain, |TOMAP (the contents first, topic numbers from 16), |CTXOMAP, the
    keyword B+ tree and its |KWDATA, and bm0... pictures. fonts are (face, half points, attributes,
    family, (r,g,b)); a topic is (title, previous, next, records) with the browse sequence as topic
    indexes or None, a record (paragraph information, [pieces]) and a piece either text or a
    command: ('font', n), ('jump', topic), ('popup', topic), ('end',) of a hotspot, ('tab',),
    ('break',), ('picture', n) in the line, or ('paragraph',) ending one (the record's format kept)."""
    cshort,cushort,clong,culong=hlp_numbers()
    def paragraph_info(info):
        bits=0; values=b''
        for bit,key,encode in ((0x02,'above',cshort),(0x04,'below',cshort),(0x08,'lines',cshort),(0x10,'left',cshort),
                               (0x20,'right',cshort),(0x40,'first',cshort)):
            if key in info: bits|=bit; values+=encode(info[key])
        if 'border' in info: bits|=0x100; values+=struct.pack('<Bh',info['border'],1)
        if 'tabs' in info:
            bits|=0x200; values+=cshort(len(info['tabs']))+b''.join(cushort(t) for t in info['tabs'])
        if info.get('align')=='centre': bits|=0x800
        return struct.pack('<H',bits)+values
    def record(info, pieces):
        strings=[b'']; commands=b''
        for piece in pieces:
            if isinstance(piece,bytes): strings[-1]+=piece; continue
            kind=piece[0]
            commands+={'font':lambda: b'\x80'+struct.pack('<H',piece[1]),'break':lambda: b'\x81','paragraph':lambda: b'\x82',
                       'tab':lambda: b'\x83','end':lambda: b'\x89',
                       'jump':lambda: b'\xe1'+struct.pack('<I',16+piece[1]),'popup':lambda: b'\xe0'+struct.pack('<I',16+piece[1]),
                       'picture':lambda: b'\x86\x03'+clong(4)+struct.pack('<HH',0,piece[1])}[kind]()
            strings.append(b'')
        data2=b'\0'.join(strings)+b'\0'
        return clong(len(data2))+bytes([0,0x80,0,0])+paragraph_info(info)+commands+b'\xff',data2
    browse=lambda n: 0xffffffff if n is None else 16+n
    links=[]; heads=[]
    for name,previous,following,records in topics:
        heads.append(len(links))
        body=[[1,*record(info,pieces)] for info,pieces in records]
        title_bytes=name.encode()+b'\0'
        size=21+12+len(title_bytes)+sum(21+len(d1)+len(d2) for _,d1,d2 in body)
        links.append([2,struct.pack('<III',size,browse(previous),browse(following)),title_bytes])
        links+=body
    links.append([2,bytes(12),b''])  # the chain's end
    # The links one after another, then cut into blocks of 2036 bytes behind 12-byte headers; a
    # link's distance to the next counts the headers between (the last one's leads past the end).
    sizes=[21+len(d1)+len(d2) for _,d1,d2 in links]
    starts=[sum(sizes[:i]) for i in range(len(links))]; end=sum(sizes)
    raw=lambda o: o//2036*2048+12+o%2036
    stream=b''
    for i,(kind,d1,d2) in enumerate(links):
        following=raw(starts[i+1] if i+1<len(links) else end)-raw(starts[i])
        previous=raw(starts[i])-raw(starts[i-1]) if i else 0xffffffff
        stream+=struct.pack('<IIIIIB',sizes[i],len(d2),previous,following,21+len(d1),kind)+d1+d2
    topic_at=[raw(starts[h]) for h in heads]
    topic=b''
    for b in range(0,len(stream),2036):
        firsts=[raw(s) for s in starts if b<=s<b+2036]
        topic+=struct.pack('<iii',max((raw(s) for s in starts if s<b),default=-1),firsts[0] if firsts else -1,
                           max((t for t in topic_at if t<raw(b+2036)),default=0))+stream[b:b+2036]
    faces=[]
    for face,*_ in fonts:
        if face not in faces: faces.append(face)
    font=struct.pack('<HHHH',len(faces),len(fonts),8,8+20*len(faces))+b''.join(f.encode().ljust(20,b'\0') for f in faces)
    font+=b''.join(struct.pack('<BBBH3s3s',attributes,points,family,faces.index(face),bytes(colour),b'\xff\xff\xff')
                   for face,points,attributes,family,colour in fonts)
    tomap=struct.pack('<I',topic_at[contents])+bytes(60)+b''.join(struct.pack('<I',t) for t in topic_at)
    ctxomap=struct.pack('<H',len(maps))+b''.join(struct.pack('<iI',number,topic_at[t]) for number,t in maps)
    kwdata=b''; kwentries=[]
    for word,targets in sorted(keywords,key=lambda k: k[0].lower()):
        kwentries.append(word.encode()+b'\0'+struct.pack('<hI',len(targets),len(kwdata)))
        kwdata+=b''.join(struct.pack('<I',topic_at[t]) for t in targets)
    files={'|SYSTEM':struct.pack('<HHHIH',0x036c,15,1,0,0)+title.encode()+b'\0','|FONT':font,'|TOPIC':topic,'|TOMAP':tomap,
           '|CTXOMAP':ctxomap,'|KWBTREE':hlp_btree(kwentries,2048,lambda e: e.index(b'\0')+1),'|KWDATA':kwdata}
    for n,picture in enumerate(pictures): files[f'bm{n}']=picture
    data=bytearray(16); entries=[]
    for name in sorted(files):
        entries.append(name.encode()+b'\0'+struct.pack('<I',len(data)))
        data+=struct.pack('<IIB',len(files[name])+9,len(files[name]),4)+files[name]
    directory=hlp_btree(entries,1024,lambda e: e.index(b'\0')+1); at=len(data)
    data+=struct.pack('<IIB',len(directory)+9,len(directory),4)+directory
    data[0:16]=struct.pack('<IiiI',0x00035f3f,at,-1,len(data))
    return bytes(data)
