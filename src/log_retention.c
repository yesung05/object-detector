#include "log_retention.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct {
    char path[6][1024]; int count,active;
    unsigned long long bytes; double modified;
} LogGroup;
static int ends(const char *s,const char *suffix) {
    size_t n=strlen(s),m=strlen(suffix);return n>=m && !strcmp(s+n-m,suffix);
}
static int generated_db(const char *name) {
    size_t n=strlen(name);
    if(n<18 || name[8]!='_')return 0;
    for(int i=0;i<15;i++)if(i!=8 && (name[i]<'0'||name[i]>'9'))return 0;
    if(!strcmp(name+15,".db"))return 1;
    if(strncmp(name+15,"_part",5))return 0;
    const char *p=name+20,*start=p;while(*p>='0'&&*p<='9')p++;
    return p>start && !strcmp(p,".db");
}
static int generated_text(const char *name) {
    if(!strcmp(name,"restarts.log"))return 1;
    if(strncmp(name,"launcher-",9) || !ends(name,".log"))return 0;
    for(const char *p=name+9;p<name+strlen(name)-4;p++)
        if((*p<'0'||*p>'9') && *p!='-')return 0;
    return 1;
}
static void add_file(LogGroup *g,const char *path,const char *a,const char *b) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if(!GetFileAttributesExA(path,GetFileExInfoStandard,&info))return;
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))return;
    snprintf(g->path[g->count++],1024,"%s",path);
    g->bytes+=((unsigned long long)info.nFileSizeHigh<<32)|info.nFileSizeLow;
    double modified=(double)(((unsigned long long)info.ftLastWriteTime.dwHighDateTime<<32)|info.ftLastWriteTime.dwLowDateTime)/10000000.0-11644473600.0;
    if(modified>g->modified)g->modified=modified;
    if((a && !_stricmp(path,a)) || (b && !_stricmp(path,b)))g->active=1;
}
static int oldest(const void *a,const void *b) {
    double delta=((const LogGroup*)a)->modified-((const LogGroup*)b)->modified;
    return delta<0?-1:delta>0?1:0;
}
static int delete_group(LogGroup *g) {
    HANDLE handles[6];int opened=0,ok=1;
    /* Exclusive handles protect files opened by another process, including WAL. */
    for(int i=0;i<g->count;i++) {
        handles[i]=CreateFileA(g->path[i],DELETE|GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(handles[i]==INVALID_HANDLE_VALUE){ok=0;break;}
        opened++;
        BY_HANDLE_FILE_INFORMATION info;
        if(!GetFileInformationByHandle(handles[i],&info) ||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){ok=0;break;}
    }
    if(ok) {
        FILE_DISPOSITION_INFO disposition;disposition.DeleteFile=TRUE;
        for(int i=0;i<opened;i++)
            if(!SetFileInformationByHandle(handles[i],FileDispositionInfo,&disposition,sizeof(disposition)))ok=0;
    }
    for(int i=0;i<opened;i++)CloseHandle(handles[i]);
    return ok;
}
#endif
LogRetentionResult log_retention_clean(const char *directory,const char *active_event,
    const char *active_perf,unsigned long long limit_bytes,double unix_now) {
    LogRetentionResult result={0};
#ifdef _WIN32
    char root[1024],pattern[1100];WIN32_FIND_DATAA item;
    if(!directory || !GetFullPathNameA(directory,sizeof(root),root,NULL))return result;
    DWORD attrs=GetFileAttributesA(root);
    if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&FILE_ATTRIBUTE_REPARSE_POINT))return result;
    char a[1024]="",b[1024]="";
    if(active_event)GetFullPathNameA(active_event,sizeof(a),a,NULL);
    if(active_perf)GetFullPathNameA(active_perf,sizeof(b),b,NULL);
    snprintf(pattern,sizeof(pattern),"%s\\*",root);
    HANDLE find=FindFirstFileA(pattern,&item);if(find==INVALID_HANDLE_VALUE)return result;
    LogGroup *groups=NULL;size_t count=0,capacity=0;
    do {
        int db=generated_db(item.cFileName);
        if(!db && !generated_text(item.cFileName))continue;
        if(item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
        if(count==capacity){size_t next=capacity?capacity*2:64;LogGroup *p=realloc(groups,next*sizeof(*p));if(!p)break;groups=p;capacity=next;}
        LogGroup *g=&groups[count++];memset(g,0,sizeof(*g));
        char path[1024],side[1024];
        if(snprintf(path,sizeof(path),"%s\\%s",root,item.cFileName)>=(int)sizeof(path)){count--;continue;}
        add_file(g,path,a,b);
        if(db) {
            snprintf(side,sizeof(side),"%s-wal",path);add_file(g,side,a,b);
            snprintf(side,sizeof(side),"%s-shm",path);add_file(g,side,a,b);
            path[strlen(path)-3]=0;strcat_s(path,sizeof(path),"_perf.db");add_file(g,path,a,b);
            snprintf(side,sizeof(side),"%s-wal",path);add_file(g,side,a,b);
            snprintf(side,sizeof(side),"%s-shm",path);add_file(g,side,a,b);
        }
        result.bytes+=g->bytes;
    }while(FindNextFileA(find,&item));
    FindClose(find);qsort(groups,count,sizeof(*groups),oldest);
    for(size_t i=0;i<count;i++) {
        LogGroup *g=&groups[i];
        if(g->active)continue;
        if(unix_now-g->modified<30.0*86400 && (!limit_bytes || result.bytes<=limit_bytes))continue;
        if(delete_group(g)){result.bytes-=g->bytes;result.removed++;}else result.failed++;
    }
    result.over_limit=limit_bytes && result.bytes>limit_bytes;
    free(groups);
#endif
    return result;
}
