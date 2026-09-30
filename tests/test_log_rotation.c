#include "log_rotation.h"
#include "../third_party/sqlite/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static int scalar(sqlite3 *db,const char *sql) {
    sqlite3_stmt *s=NULL;int result=-1;
    CHECK(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK);
    if(sqlite3_step(s)==SQLITE_ROW)result=sqlite3_column_int(s,0);
    sqlite3_finalize(s);return result;
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--crash")) {
        EventLog crash={0};
        CHECK(!event_log_open(&crash,"rotation-crash.db",LOG_INFO,0));
        event_log_write(&crash,LOG_ERROR,"test","committed before abrupt exit");
        _Exit(0); /* Deliberately bypass all SQLite close/checkpoint cleanup. */
    }
    char command[2048];
    remove("rotation-crash.db");remove("rotation-crash.db-wal");remove("rotation-crash.db-shm");
    snprintf(command,sizeof(command),"\"%s\" --crash",argv[0]);
    CHECK(system(command)==0);
    sqlite3 *recovered=NULL;
    CHECK(sqlite3_open("rotation-crash.db",&recovered)==SQLITE_OK);
    CHECK(scalar(recovered,"SELECT count(*) FROM events")==1);
    sqlite3_close(recovered);remove("rotation-crash.db");
    EventLog e={0};ReplayLog r={0};PerfLog p={0};LogRotation rot;
    remove("rotation-test.db");remove("rotation-test_perf.db");
    remove("rotation-test_part000001.db");remove("rotation-test_part000001_perf.db");
    CHECK(!event_log_open(&e,"rotation-test.db",LOG_INFO,0));
    CHECK(!replay_open(&r,&e,"{\"test\":true}"));
    CHECK(!perf_log_open(&p,"rotation-test_perf.db"));
    CHECK(scalar(e.db,"PRAGMA synchronous")==2);
    log_rotation_init(&rot,"rotation-test.db","{\"test\":true}",100);
    r.now=3699;replay_sample(&r,3699,"{\"before\":true}");
    replay_geometry(&r,7,"{}");
    event_log_write(&e,LOG_ERROR,"rules","person_fallen");
    CHECK(!log_rotation_tick(&rot,&e,&r,&p,3699));
    sqlite3 *reader=NULL;CHECK(sqlite3_open("rotation-test.db",&reader)==SQLITE_OK);
    /* A separate connection sees committed data before the writer closes. */
    CHECK(scalar(reader,"SELECT count(*) FROM events")==1);
    CHECK(log_rotation_tick(&rot,&e,&r,&p,3700)==1);
    CHECK(scalar(reader,"SELECT count(*) FROM events")==1);sqlite3_close(reader);
    CHECK(e.observer_context==&r);CHECK(r.until==3709);
    CHECK(scalar(e.db,"SELECT count(*) FROM replay_geometry WHERE revision=7")==1);
    r.now=3700;event_log_write(&e,LOG_ERROR,"rules","person_fallen");replay_sample(&r,3700,"{}");
    CHECK(scalar(e.db,"SELECT count(*) FROM replay_events")==1);
    CHECK(scalar(e.db,"SELECT count(*) FROM replay_frames WHERE mono=3699")==1);
    CHECK(scalar(e.db,"PRAGMA synchronous")==2);
    PerfMetrics metrics={0};perf_log_write(&p,&metrics);
    CHECK(scalar(p.db,"SELECT count(*) FROM perf")==1);
    sqlite3 *original=e.db;
    snprintf(rot.base,sizeof(rot.base),"missing-rotation-directory/file");
    CHECK(log_rotation_tick(&rot,&e,&r,&p,7300)==-1);CHECK(e.db==original);
    event_log_write(&e,LOG_INFO,"test","still recording");
    CHECK(scalar(e.db,"SELECT count(*) FROM events")==3);
    replay_close(&r,&e);event_log_close(&e);perf_log_close(&p);
    remove("rotation-test.db");remove("rotation-test_perf.db");
    remove("rotation-test_part000001.db");remove("rotation-test_part000001_perf.db");
    puts("hourly rotation, live persistence, replay continuity and failure fallback passed");return 0;
}
