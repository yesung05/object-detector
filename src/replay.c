#include "replay.h"
#include "../third_party/sqlite/sqlite3.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
/* Budget is per event database, and covers coordinate JSON payloads, not other logs. */
#define REPLAY_BUDGET (1024LL*1024*1024)
static const char *schema=
 "CREATE TABLE IF NOT EXISTS replay_sessions(id INTEGER PRIMARY KEY,created TEXT DEFAULT CURRENT_TIMESTAMP,metadata TEXT NOT NULL);"
 "CREATE TABLE IF NOT EXISTS replay_frames(session INTEGER,mono REAL,payload TEXT NOT NULL,PRIMARY KEY(session,mono));"
 "CREATE TABLE IF NOT EXISTS replay_events(event_id INTEGER PRIMARY KEY,session INTEGER,mono REAL,review TEXT DEFAULT 'unreviewed',note TEXT DEFAULT '');"
 "CREATE TABLE IF NOT EXISTS replay_geometry(session INTEGER,revision INTEGER,payload TEXT,PRIMARY KEY(session,revision));";
int replay_open(ReplayLog *r,EventLog *log,const char *metadata) {
 sqlite3_stmt *st=NULL;memset(r,0,sizeof(*r));if(!log||!log->db)return -1;r->db=log->db;
 if(sqlite3_exec(r->db,schema,NULL,NULL,NULL)!=SQLITE_OK)goto fail;
 if(sqlite3_prepare_v2(r->db,"SELECT coalesce(sum(length(payload)),0) FROM replay_frames",-1,&st,NULL)!=SQLITE_OK)goto fail;
 if(sqlite3_step(st)==SQLITE_ROW)r->bytes=sqlite3_column_int64(st,0);sqlite3_finalize(st);st=NULL;
 if(sqlite3_prepare_v2(r->db,"INSERT INTO replay_sessions(metadata) VALUES(?)",-1,&st,NULL)!=SQLITE_OK)goto fail;
 sqlite3_bind_text(st,1,metadata,-1,SQLITE_TRANSIENT);if(sqlite3_step(st)!=SQLITE_DONE)goto fail;
 r->session=sqlite3_last_insert_rowid(r->db);sqlite3_finalize(st);st=NULL;
 if(sqlite3_prepare_v2(r->db,"INSERT OR IGNORE INTO replay_frames VALUES(?,?,?)",-1,&r->insert,NULL)!=SQLITE_OK)goto fail;
 r->ring=(ReplayFrame*)calloc(REPLAY_RING,sizeof(ReplayFrame));if(!r->ring)goto fail;
 log->observer=replay_observe;log->observer_context=r;return 0;
fail:
 if(st)sqlite3_finalize(st);replay_close(r,log);return -1;
}
void replay_close(ReplayLog *r,EventLog *log) {
 if(log&&log->observer_context==r){log->observer=NULL;log->observer_context=NULL;}
 if(r->insert)sqlite3_finalize(r->insert);free(r->ring);r->insert=NULL;r->ring=NULL;r->db=NULL;
}
void replay_observe(void *context,long long id,LogLevel level,const char *module,const char *message) {
 ReplayLog *r=(ReplayLog*)context;sqlite3_stmt *st=NULL;
 if(!r||!r->db||!r->ring||r->now<=0)return;
 if(strcmp(module,"rules")&&strcmp(module,"door")&&strcmp(module,"surface")&&strcmp(module,"camera"))return;
 if(level==LOG_INFO&&!strstr(message,"probably_ordered")&&!strstr(message,"door_closed")&&!strstr(message,"camera_ok"))return;
 if(sqlite3_prepare_v2(r->db,"INSERT OR IGNORE INTO replay_events(event_id,session,mono) VALUES(?,?,?)",-1,&st,NULL)!=SQLITE_OK){r->errors++;return;}
 sqlite3_bind_int64(st,1,id);sqlite3_bind_int64(st,2,r->session);sqlite3_bind_double(st,3,r->now);
 if(sqlite3_step(st)!=SQLITE_DONE)r->errors++;sqlite3_finalize(st);
 r->pending=1;r->until=r->now+10;
}
int replay_due(const ReplayLog *r,double now) {return r&&r->ring&&!r->full&&(!r->last_sample||now-r->last_sample>=.2);}
static void persist(ReplayLog *r,const ReplayFrame *f) {
 if(!f->length)return;
 if(r->bytes+f->length>REPLAY_BUDGET){r->full=1;return;}
 sqlite3_reset(r->insert);sqlite3_bind_int64(r->insert,1,r->session);sqlite3_bind_double(r->insert,2,f->mono);
 sqlite3_bind_text(r->insert,3,f->json,f->length,SQLITE_STATIC);
 if(sqlite3_step(r->insert)!=SQLITE_DONE){r->errors++;return;}
 if(sqlite3_changes(r->db)){r->bytes+=f->length;r->written++;}
}
void replay_sample(ReplayLog *r,double now,const char *json) {
 ReplayFrame *f;size_t n;
 if(!replay_due(r,now)||!json)return;n=strlen(json);
 if(n>=REPLAY_JSON_MAX){r->errors++;return;}
 f=&r->ring[r->next];f->mono=now;f->length=(int)n;memcpy(f->json,json,n+1);
 r->next=(r->next+1)%REPLAY_RING;if(r->count<REPLAY_RING)r->count++;r->last_sample=now;
 if(r->pending||now<=r->until||!r->last_regular||now-r->last_regular>=10) {
  long long saved_bytes=r->bytes,saved_written=r->written;
  if(sqlite3_exec(r->db,"BEGIN",NULL,NULL,NULL)!=SQLITE_OK){r->errors++;return;}
  if(r->pending)for(int i=0;i<r->count;i++) {
   const ReplayFrame *p=&r->ring[(r->next-r->count+i+REPLAY_RING)%REPLAY_RING];
   if(p->mono>=now-10)persist(r,p);
  }
  persist(r,f);
  if(sqlite3_exec(r->db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(r->db,"ROLLBACK",NULL,NULL,NULL);r->bytes=saved_bytes;r->written=saved_written;r->errors++;}
  r->pending=0;r->last_regular=now;
 }
}
void replay_geometry(ReplayLog *r,int revision,const char *json) {
 sqlite3_stmt *st=NULL;if(!r||!r->db||!json||strlen(json)>65536)return;
 if(sqlite3_prepare_v2(r->db,"INSERT OR IGNORE INTO replay_geometry VALUES(?,?,?)",-1,&st,NULL)!=SQLITE_OK){r->errors++;return;}
 sqlite3_bind_int64(st,1,r->session);sqlite3_bind_int(st,2,revision);sqlite3_bind_text(st,3,json,-1,SQLITE_TRANSIENT);
 if(sqlite3_step(st)!=SQLITE_DONE)r->errors++;sqlite3_finalize(st);
}
