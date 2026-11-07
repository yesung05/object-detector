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
/* 훅이 불리는 시점의 이벤트 DB 파일 이름을 기록한다(어느 파일에 쓰는 훅인지 확인용). */
struct HookSeen {int before,after;char before_file[64],after_file[64];};
static void note_file(char *out,size_t n,EventLog *e) {
    const char *f=e&&e->db?sqlite3_db_filename(e->db,"main"):"";
    const char *slash=f?strrchr(f,'/'):NULL,*back=f?strrchr(f,'\\'):NULL;
    if(back&&(!slash||back>slash))slash=back;
    snprintf(out,n,"%s",slash?slash+1:(f?f:""));
}
static void hook_before(void *c,EventLog *e) {struct HookSeen *s=(struct HookSeen*)c;s->before++;note_file(s->before_file,sizeof(s->before_file),e);}
static void hook_after(void *c,EventLog *e) {struct HookSeen *s=(struct HookSeen*)c;s->after++;note_file(s->after_file,sizeof(s->after_file),e);}
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
    /* 로깅 v2: 훅은 새 파일을 연 뒤·옛 파일을 닫기 전에(before), 새 파일로 바뀐 직후에(after) 한 번씩 불린다. */
    {
        EventLog e2={0};ReplayLog r2={0};PerfLog p2={0};LogRotation rot2;
        struct HookSeen seen={0};
        remove("hook-test.db");remove("hook-test_perf.db");remove("hook-test_part000001.db");remove("hook-test_part000001_perf.db");
        CHECK(!event_log_open(&e2,"hook-test.db",LOG_INFO,0));CHECK(!replay_open(&r2,&e2,"{}"));CHECK(!perf_log_open(&p2,"hook-test_perf.db"));
        event_log_set_run(&e2,4242);event_log_set_context(&e2,9,1.5);
        log_rotation_init(&rot2,"hook-test.db","{}",100);
        log_rotation_set_hooks(&rot2,hook_before,hook_after,&seen);
        r2.now=3701;
        CHECK(log_rotation_tick(&rot2,&e2,&r2,&p2,3701)==1);
        CHECK(seen.before==1 && seen.after==1);
        CHECK(strstr(seen.before_file,"hook-test.db") && !strstr(seen.before_file,"part"));   /* 요약은 옛 파일에 */
        CHECK(strstr(seen.after_file,"hook-test_part000001.db"));                            /* 실행 정보는 새 파일에 */
        CHECK(e2.run_id==4242 && e2.ctx_valid==1 && e2.ctx_frame==9);                         /* 문맥은 새 로그로 이어진다 */
        replay_close(&r2,&e2);event_log_close(&e2);perf_log_close(&p2);
        remove("hook-test.db");remove("hook-test_perf.db");remove("hook-test_part000001.db");remove("hook-test_part000001_perf.db");
    }
    /* 로깅 v2: 옛 perf DB 이전과 요약 집계(중앙값·합계) */
    {
        sqlite3 *old=NULL;PerfLog pl={0};PerfMetrics m={0};PerfSummary s;int i;
        remove("perf-old.db");remove("perf-old.db-wal");remove("perf-old.db-shm");
        CHECK(sqlite3_open("perf-old.db",&old)==SQLITE_OK);
        CHECK(sqlite3_exec(old,"CREATE TABLE perf(id INTEGER PRIMARY KEY,ts_iso TEXT NOT NULL,cpu_pct INTEGER,cpu_temp INTEGER,cam_fps REAL,inf_fps REAL,mem_kb INTEGER,"
            "gray_ms REAL,door_ms REAL,residue_ms REAL,stream_ms REAL,draw_ms REAL,gate_l0 INTEGER,gate_l1 INTEGER,gate_l2 INTEGER,gate_l3 INTEGER,inf_runs INTEGER,obj_runs INTEGER);",NULL,NULL,NULL)==SQLITE_OK);
        sqlite3_close(old);
        CHECK(!perf_log_open(&pl,"perf-old.db"));
        CHECK(scalar(pl.db,"SELECT count(*) FROM pragma_table_info('perf') WHERE name IN ('pre_ms','inf_ms','post_ms','t2_ms','track_ms','frames','cam_bad_s','wait_frames','dropped')")==9);
        for(i=0;i<5;++i){m.cpu_pct=10+i*10;m.cam_fps=20.0+i;m.inf_runs=100;m.inf_ms=6000.0;m.frames=1800;m.cam_bad_s=i==4?60.0:0.0;m.wait_frames=i;perf_log_write(&pl,&m);}
        CHECK(!perf_log_summary(&pl,0,&s));
        CHECK(s.rows==5 && s.cpu_med==30 && s.cpu_max==50 && s.fps_med==22.0);
        CHECK(s.inf_runs==500 && s.inf_ms==30000.0 && s.cam_bad_s==60.0 && s.wait_frames==10 && s.frames==9000);
        CHECK(!perf_log_summary(&pl,s.last_id,&s) && s.rows==0);                               /* 기준점 이후만 */
        perf_log_close(&pl);
        remove("perf-old.db");remove("perf-old.db-wal");remove("perf-old.db-shm");
    }
    puts("hourly rotation, live persistence, replay continuity and failure fallback passed");return 0;
}
