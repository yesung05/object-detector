#include "replay.h"
#include "../third_party/sqlite/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int scalar(sqlite3 *db,const char *sql){sqlite3_stmt *st=NULL;int v=-1;CHECK(sqlite3_prepare_v2(db,sql,-1,&st,NULL)==SQLITE_OK);if(sqlite3_step(st)==SQLITE_ROW)v=sqlite3_column_int(st,0);sqlite3_finalize(st);return v;}
int main(void) {
 EventLog log;ReplayLog r;char json[100];long long first_session;
 CHECK(event_log_open(&log,":memory:",LOG_INFO,0)==0);CHECK(replay_open(&r,&log,"{\"test\":true}")==0);
 first_session=r.session;
 for(int i=4;i<=92;i++) {
  double now=i*.25;r.now=now;
  if(i==48)event_log_write(&log,LOG_ERROR,"rules","person_fallen track=1 hold=5.0s");
  snprintf(json,sizeof(json),"{\"t\":%.2f,\"people\":[]}",now);replay_sample(&r,now,json);
 }
 CHECK(scalar(log.db,"SELECT count(*) FROM replay_events")==1);
 CHECK(scalar(log.db,"SELECT count(*) FROM replay_frames WHERE mono BETWEEN 2 AND 22")>=80);
 CHECK(scalar(log.db,"SELECT count(*) FROM replay_frames WHERE mono>22")==0);
 CHECK(scalar(log.db,"SELECT count(*) FROM replay_frames")==r.written);
 CHECK(r.errors==0);
 r.now=24;event_log_write(&log,LOG_INFO,"access","stream opened");
 CHECK(scalar(log.db,"SELECT count(*) FROM replay_events")==1);
 replay_geometry(&r,1,"{\"surfaces\":[]}");replay_geometry(&r,1,"{\"surfaces\":[]}");
 CHECK(scalar(log.db,"SELECT count(*) FROM replay_geometry")==1);
 r.bytes=1024LL*1024*1024;r.now=40;replay_sample(&r,40,"{}");CHECK(r.full);CHECK(!replay_due(&r,41));
 replay_close(&r,&log);CHECK(log.observer==NULL);
 CHECK(replay_open(&r,&log,"{}")==0);CHECK(r.session>first_session);
 CHECK(!r.full);replay_close(&r,&log);event_log_close(&log);
 puts("replay pre/post window, deduplication, metadata, quota and session tests passed");return 0;
}
