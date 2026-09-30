#include "log_retention.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char root[MAX_PATH];static double now;
static void path(char *out,const char *name){snprintf(out,MAX_PATH,"%s\\%s",root,name);}
static void make(const char *name,int age) {
 char file[MAX_PATH];path(file,name);
 HANDLE h=CreateFileA(file,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(h!=INVALID_HANDLE_VALUE);
 char data[100]={0};DWORD written;CHECK(WriteFile(h,data,100,&written,NULL));
 ULARGE_INTEGER time;time.QuadPart=(unsigned long long)((now-age*86400+11644473600.0)*10000000.0);
 FILETIME ft={time.LowPart,time.HighPart};CHECK(SetFileTime(h,NULL,NULL,&ft));CloseHandle(h);
}
static int exists(const char *name){char file[MAX_PATH];path(file,name);return GetFileAttributesA(file)!=INVALID_FILE_ATTRIBUTES;}
int main(void) {
 char cwd[MAX_PATH];GetCurrentDirectoryA(MAX_PATH,cwd);
 snprintf(root,sizeof(root),"%s\\retention-test-%lu-%llu",cwd,GetCurrentProcessId(),GetTickCount64());
 CHECK(CreateDirectoryA(root,NULL));now=(double)time(NULL);
 make("20260101_000000.db",31);make("20260101_000000_perf.db",31);
 make("20260101_000000.db-wal",31);make("20260101_000000.db-shm",31);
 make("20260102_000000.db",29);make("20260103_000000.db",1);
 make("20260104_000000.db",40);make("notes.txt",40);make("custom.db",40);
 char active[MAX_PATH];path(active,"20260104_000000.db");
 LogRetentionResult r=log_retention_clean(root,active,NULL,0,now);
 CHECK(r.removed==1);CHECK(!exists("20260101_000000.db"));CHECK(!exists("20260101_000000_perf.db"));CHECK(!exists("20260101_000000.db-wal"));
 CHECK(exists("20260102_000000.db"));CHECK(exists("20260104_000000.db"));CHECK(exists("notes.txt"));CHECK(exists("custom.db"));
 r=log_retention_clean(root,active,NULL,200,now);
 CHECK(r.removed==1);CHECK(!exists("20260102_000000.db"));CHECK(exists("20260103_000000.db"));CHECK(!r.over_limit);
 make("20260105_000000.db",40);make("20260105_000000_perf.db",40);
 char locked[MAX_PATH];path(locked,"20260105_000000_perf.db");
 HANDLE h=CreateFileA(locked,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);
 r=log_retention_clean(root,active,NULL,50,now);
 CHECK(r.over_limit);CHECK(exists("20260105_000000.db"));CHECK(exists("20260105_000000_perf.db"));CHECK(exists("20260104_000000.db"));
 CloseHandle(h);r=log_retention_clean(root,active,NULL,0,now);CHECK(!exists("20260105_000000.db"));
 puts("30-day retention, oldest-first quota, active protection, group lock and unrelated-file preservation passed");return 0;
}
