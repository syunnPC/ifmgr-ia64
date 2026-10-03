/* SPDX-License-Identifier: GPL-2.0-or-later
 * CRTTEST: the C run-time library (win/crt) as Windows 3.0 programs use it:
 * formatting (floats too), scanning, conversions, mathematics, the heap,
 * streams and low-level files (text and binary), time and time zones,
 * strings, sorting, paths and directories; and that a program's memory is
 * below 2 GiB. Each check prints "CRTTEST: <name> ok" or FAILED with what it
 * got (printf goes to OutputDebugString); the exit code is the failures.
 * C89. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <dos.h>
#include <malloc.h>
#include <sys/stat.h>
static int failures;
static void check(const char *name,int ok,const char *got) {
    if(ok) printf("CRTTEST: %s ok\n",name);
    else {printf("CRTTEST: %s FAILED (%s)\n",name,got?got:""); failures++;}
}
static int near_to(double a,double b,double tolerance) {double d=a-b; if(d<0) d=-d; return d<=tolerance*(b<0?-b:b>1?b:1);}
static char got[512];
static void formats(void) {
    sprintf(got,"%d|%5d|%-5d|%05d|%+d|% d|%x|%X|%#x|%o|%#o|%u|%c|%s|%.3s|%6s|%-6s|%%|%ld|%hd|%I64d",
        42,42,42,42,42,42,255,255,255,8,8,4000000000U,'A',"str","abcdef","ab","ab",-7L,(short)-3,(__int64)-123456789012);
    check("integers",!strcmp(got,"42|   42|42   |00042|+42| 42|ff|FF|0xff|10|010|4000000000|A|str|abc|    ab|ab    |%|-7|-3|-123456789012"),got);
    sprintf(got,"%f|%.2f|%10.3f|%-8.1f|%#.0f|%.0f|%e|%.2E|%g|%g|%g|%g|%G",
        1.5,3.14159,-3.14159,2.75,3.0,2.75,12345.678,0.000123,0.0001,1e-5,123456789.0,100.0,1e-10);
    check("floats",!strcmp(got,"1.500000|3.14|    -3.142|2.8     |3.|3|1.234568e+004|1.23E-004|0.0001|1e-005|1.23457e+008|100|1E-010"),got);
    sprintf(got,"%f|%.3f|%.10f|%g",1e20,0.0005,1.0/3.0,0.1+0.2);
    check("float digits",!strcmp(got,"100000000000000000000.000000|0.001|0.3333333333|0.3"),got);
    sprintf(got,"%*d|%-*.*s|%Fs|%Np",5,7,6,2,"xyz","far",(void *)0);
    check("width and far",!strcmp(got,"    7|xy    |far|0000000000000000"),got);
    check("snprintf",_snprintf(got,4,"%s","abcdef")==-1 && !strncmp(got,"abcd",4),NULL);
}
static void conversions(void) {
    char *end; double d=strtod("  -1.5xyz",&end);
    check("strtod",d==-1.5 && !strcmp(end,"xyz") && atof("3.25e2")==325.0 && atof(".5")==0.5,NULL);
    errno=0; d=strtod("1e400",NULL);
    check("strtod range",d==HUGE_VAL && errno==ERANGE,NULL);
    check("strtol",strtol("0x1F",&end,0)==31 && strtol("-077",NULL,0)==-63 && strtoul("ffffffff",NULL,16)==0xffffffffUL && atoi(" 12abc")==12,NULL);
    check("itoa",!strcmp(itoa(-255,got,10),"-255") && !strcmp(ltoa(255,got+20,16),"ff") && !strcmp(ultoa(5,got+40,2),"101"),NULL);
}
static void scanning(void) {
    int a=0,b=0,n; char word[16],letters[16],c=0; float f=0; double g=0;
    n=sscanf("12 ff hello 2.5 X abcDEF 1e3","%d %x %s %f %c %[a-z]%*s %lf",&a,&b,word,&f,&c,letters,&g);
    sprintf(got,"%d %d %d %s %g %c %s %g",n,a,b,word,f,c,letters,g);
    check("sscanf",n==7 && a==12 && b==255 && !strcmp(word,"hello") && f==2.5f && c=='X' && !strcmp(letters,"abc") && g==1000,got);
    check("sscanf end",sscanf("","%d",&a)==EOF && sscanf("x","%d",&a)==0,NULL);
}
static void mathematics(void) {
    const double pi=3.14159265358979323846; volatile double minus; int e;
    check("sqrt",near_to(sqrt(2.0),1.4142135623730951,1e-15) && sqrt(16.0)==4.0 && near_to(sqrt(1e300),1e150,1e-15),NULL);
    check("sin cos tan",near_to(sin(pi/6),0.5,1e-14) && near_to(cos(pi/3),0.5,1e-14) && near_to(tan(pi/4),1,1e-14) && near_to(sin(100.0),-0.50636564110975879,1e-13),NULL);
    check("atan",near_to(atan(1.0)*4,pi,1e-15) && near_to(atan2(-1,-1),-3*pi/4,1e-15) && near_to(asin(1.0),pi/2,1e-15) && near_to(acos(0.0),pi/2,1e-15),NULL);
    check("exp log",near_to(exp(1.0),2.7182818284590452,1e-15) && near_to(log(2.7182818284590452),1,1e-15) && near_to(log10(1000.0),3,1e-15) && near_to(exp(-20.0),2.061153622438558e-9,1e-14),NULL);
    check("pow",pow(2,10)==1024 && near_to(pow(2,0.5),1.4142135623730951,1e-15) && pow(-2,3)==-8 && near_to(pow(10,-2),0.01,1e-15),NULL);
    check("floor fmod",floor(-2.5)==-3 && ceil(-2.5)==-2 && fmod(10.5,3)==1.5 && fmod(-7,2)==-1 && near_to(frexp(10.0,&e),0.625,0) && e==4 && ldexp(0.625,4)==10,NULL);
    check("hyperbolic",near_to(sinh(1.0),1.1752011936438014,1e-14) && near_to(cosh(1.0),1.5430806348152437,1e-14) && near_to(tanh(0.5),0.46211715726000974,1e-14) && hypot(3,4)==5,NULL);
    errno=0; minus=-1; check("domain",log(minus)!=log(minus) && errno==EDOM,NULL);
}
static void heap(void) {
    char *blocks[200]; int i,ok=1; char *p;
    for(i=0;i<200;i++) {blocks[i]=(char *)malloc((size_t)(i*37%500+1)); if(!blocks[i]) ok=0; else memset(blocks[i],i,(size_t)(i*37%500+1));}
    for(i=0;i<200;i+=2) free(blocks[i]);
    for(i=1;i<200;i+=2) {p=(char *)realloc(blocks[i],1000); if(!p || p[0]!=(char)i) ok=0; blocks[i]=p;}
    for(i=1;i<200;i+=2) free(blocks[i]);
    p=(char *)calloc(100,10); for(i=0;i<1000;i++) if(p[i]) ok=0;
    free(p);
    p=(char *)malloc(300000); if(!p || _msize(p)<300000) ok=0;
    free(p);
    check("heap",ok,NULL);
}
static void streams(void) {
    FILE *f; char line[64]; int a=0; long pos; unsigned char raw[32]; size_t n;
    f=fopen("C:\\CRTTEST.TXT","w");
    check("fopen",f!=NULL,NULL);
    if(!f) return;
    fprintf(f,"first %d\nsecond\n",7); fputs("third\n",f); fclose(f);
    f=fopen("C:\\CRTTEST.TXT","rb"); n=fread(raw,1,sizeof(raw),f); fclose(f);
    check("text written",n==24 && !memcmp(raw,"first 7\r\nsecond\r\nthird\r\n",24),NULL);
    f=fopen("C:\\CRTTEST.TXT","r");
    fgets(line,sizeof(line),f);
    check("text read",!strcmp(line,"first 7\n"),line);
    pos=ftell(f); fscanf(f,"%s",line);
    check("ftell",pos==9 && !strcmp(line,"second"),NULL);
    fseek(f,0,SEEK_SET); fscanf(f,"first %d",&a);
    check("fseek",a==7,NULL);
    while(fgets(line,sizeof(line),f)) {}
    check("feof",feof(f) && !ferror(f),NULL);
    fclose(f);
    f=fopen("C:\\CRTTEST.TXT","a"); fputs("fourth\n",f); fclose(f);
    f=fopen("C:\\CRTTEST.TXT","rb"); fseek(f,0,SEEK_END); pos=ftell(f); fclose(f);
    check("append",pos==32,NULL);
    check("remove",!remove("C:\\CRTTEST.TXT") && fopen("C:\\CRTTEST.TXT","r")==NULL && errno==ENOENT,NULL);
}
static void handles(void) {
    int fd=open("C:\\CRTTEST.BIN",O_CREAT|O_TRUNC|O_RDWR|O_BINARY,0); char b[16]; struct stat st;
    check("open",fd>=0,NULL);
    if(fd<0) return;
    write(fd,"0123456789",10);
    check("lseek",lseek(fd,4,SEEK_SET)==4 && read(fd,b,3)==3 && !memcmp(b,"456",3) && tell(fd)==7 && filelength(fd)==10,NULL);
    close(fd);
    check("stat",!stat("C:\\CRTTEST.BIN",&st) && st.st_size==10 && (st.st_mode&S_IFREG) && !access("C:\\CRTTEST.BIN",0),NULL);
    check("unlink",!unlink("C:\\CRTTEST.BIN") && access("C:\\CRTTEST.BIN",0)==-1,NULL);
}
/* Local time (a noon, or 12:34:56 on 29 February 2000) to UTC and back. */
static time_t local(int year,int month,int day,struct tm *t) {
    memset(t,0,sizeof(*t)); t->tm_year=year-1900; t->tm_mon=month-1; t->tm_mday=day; t->tm_hour=12; t->tm_isdst=-1;
    if(year==2000) {t->tm_min=34; t->tm_sec=56;}
    return mktime(t);
}
static void times(void) {
    struct tm t,*u; time_t s; char text[64]; int summer,winter;
    /* UTC. */
    putenv("TZ=UTC0"); tzset();
    s=local(2000,2,29,&t); u=gmtime(&s);
    strftime(text,sizeof(text),"%Y-%m-%d %H:%M:%S %a %j",u);
    check("mktime",s==951827696L && t.tm_wday==2 && !strcmp(text,"2000-02-29 12:34:56 Tue 060"),text);
    check("asctime",!strcmp(asctime(u),"Tue Feb 29 12:34:56 2000\n"),NULL);
    check("time",time(NULL)>946684800L,NULL);
    /* Japan's: nine hours east, no daylight saving time. */
    putenv("TZ=JST-9"); tzset();
    s=local(2000,2,29,&t); u=localtime(&s);
    strftime(text,sizeof(text),"%H:%M:%S %Z",u);
    check("TZ",timezone==-32400L && !daylight && s==951827696L-32400L && !strcmp(text,"12:34:56 JST") && gmtime(&s)->tm_hour==3,text);
    /* Without TZ, Microsoft C's Pacific time and the United States' daylight
     * saving time: from the second Sunday of March since 2007 (the first of
     * April before) to the first Sunday of November. */
    putenv("TZ="); tzset();
    s=local(2007,3,11,&t); summer=t.tm_isdst==1 && gmtime(&s)->tm_hour==19;
    local(2007,11,3,&t); summer=summer && t.tm_isdst==1;
    local(2006,4,2,&t); summer=summer && t.tm_isdst==1;
    s=local(2007,3,10,&t); winter=t.tm_isdst==0 && gmtime(&s)->tm_hour==20;
    local(2007,11,4,&t); winter=winter && t.tm_isdst==0;
    local(2006,3,12,&t); winter=winter && t.tm_isdst==0;
    local(2007,7,1,&t); strftime(text,sizeof(text),"%Z",&t);
    check("daylight",timezone==28800L && daylight && summer && winter && !strcmp(text,"PDT"),text);
}
static int compare(const void *a,const void *b) {return *(const int *)a-*(const int *)b;}
static void misc(void) {
    int v[8]={5,3,9,1,7,2,8,0},key=7,i,sorted=1; char s[64],drive[4],dir[64],name[16],ext[8]; char *t;
    qsort(v,8,sizeof(int),compare);
    for(i=1;i<8;i++) if(v[i-1]>v[i]) sorted=0;
    check("qsort",sorted && bsearch(&key,v,8,sizeof(int),compare)==&v[5],NULL);
    strcpy(s,"a,b;;c");
    t=strtok(s,",;"); strcpy(got,t); while((t=strtok(NULL,",;"))!=NULL) strcat(got,t);
    check("strings",!strcmp(got,"abc") && !stricmp("Hello","hELLO") && strstr("haystack","st")!=NULL && !strcmp(strupr(strcpy(s,"abc")),"ABC") && !strcmp(strrev(strcpy(s,"abc")),"cba"),got);
    _splitpath("C:\\DIR\\SUB\\FILE.TXT",drive,dir,name,ext); _makepath(s,"D","\\X","NAME",".EXT");
    check("paths",!strcmp(drive,"C:") && !strcmp(dir,"\\DIR\\SUB\\") && !strcmp(name,"FILE") && !strcmp(ext,".TXT") && !strcmp(s,"D:\\X\\NAME.EXT"),NULL);
    check("ctype",isalpha('q') && !isalpha('1') && isspace('\t') && toupper('a')=='A' && isxdigit('F') && ispunct('!'),NULL);
    check("getenv",getenv("COMSPEC")!=NULL && !putenv("CRTTEST=yes") && !strcmp(getenv("crttest"),"yes"),NULL);
}
static void directories(void) {
    char here[128],there[128];
    getcwd(here,sizeof(here));
    check("mkdir",!mkdir("C:\\CRTDIR") && !chdir("C:\\CRTDIR") && getcwd(there,sizeof(there)) && !stricmp(there,"C:\\CRTDIR"),there);
    chdir(here);
    check("rmdir",!rmdir("C:\\CRTDIR") && chdir("C:\\CRTDIR")==-1,NULL);
}
/* What a program addresses is below 2 GiB, so that a pointer kept in a LONG survives. */
static void addresses(void) {
    int local; void *heap=malloc(16); HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,16); void *global=GlobalLock(g);
    sprintf(got,"stack %p heap %p image %p global %p handle %p",(void *)&local,heap,(void *)&failures,global,(void *)g);
    check("below 2 GiB",(ULONG_PTR)&local<0x80000000U && (ULONG_PTR)heap<0x80000000U && (ULONG_PTR)&failures<0x80000000U &&
          (ULONG_PTR)global<0x80000000U && (ULONG_PTR)g<0x80000000U,got);
    free(heap); GlobalUnlock(g); GlobalFree(g);
}
int PASCAL WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show) {
    (void)instance; (void)previous; (void)command; (void)show;
    addresses(); formats(); conversions(); scanning(); mathematics(); heap(); streams(); handles(); times(); misc(); directories();
    printf("CRTTEST: %d failures\n",failures);
    return failures;
}
