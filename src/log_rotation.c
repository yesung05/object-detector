#include "log_rotation.h"
#include "log_retention.h"
#include <time.h>
#include "../third_party/sqlite/sqlite3.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void log_rotation_init(LogRotation *r,const char *path,const char *metadata,double now) {
    memset(r,0,sizeof(*r));
    if(!path || !strcmp(path,":memory:") || strlen(path)>=sizeof(r->base)-40)return;
    snprintf(r->base,sizeof(r->base),"%s",path);
    char *dot=strrchr(r->base,'.');
    if(dot && !strcmp(dot,".db"))*dot=0;
    snprintf(r->metadata,sizeof(r->metadata),"%s",metadata);
    r->next=now+3600;
}

int log_rotation_tick(LogRotation *r,EventLog *events,ReplayLog *replay,PerfLog *perf,double now) {
    EventLog next_events={0}; ReplayLog next_replay={0}; PerfLog next_perf={0};
    char path[750],perf_path[750];
    if(!r->base[0] || now<r->next)return 0;
    if(!replay->ring || !replay->db){r->next=now+60;return -1;}
    /* Never append to an earlier part after a supervisor restart. */
    for(;;) {
        FILE *existing;
        snprintf(path,sizeof(path),"%s_part%06u.db",r->base,++r->part);
        existing=fopen(path,"rb");
        if(!existing)break;
        fclose(existing);
    }
    snprintf(perf_path,sizeof(perf_path),"%s_part%06u_perf.db",r->base,r->part);
    if(event_log_open(&next_events,path,events->min_level,events->echo_stderr) ||
       replay_open(&next_replay,&next_events,r->metadata) ||
       perf_log_open(&next_perf,perf_path))goto fail;
    /* Copy geometry so pre-boundary ring samples remain interpretable. */
    sqlite3_stmt *geometry=NULL;
    if(sqlite3_prepare_v2(replay->db,"SELECT revision,payload FROM replay_geometry WHERE session=?",-1,&geometry,NULL)!=SQLITE_OK)goto fail;
    sqlite3_bind_int64(geometry,1,replay->session);
    int rc;
    while((rc=sqlite3_step(geometry))==SQLITE_ROW)
        replay_geometry(&next_replay,sqlite3_column_int(geometry,0),(const char*)sqlite3_column_text(geometry,1));
    sqlite3_finalize(geometry);
    if(rc!=SQLITE_DONE || next_replay.errors)goto fail;
    memcpy(next_replay.ring,replay->ring,sizeof(ReplayFrame)*REPLAY_RING);
    next_replay.next=replay->next;next_replay.count=replay->count;
    next_replay.now=replay->now;next_replay.until=replay->until;
    next_replay.pending=replay->pending;
    /* Counters/quota restart per file; event IDs remain local to each database. */
    replay_close(replay,events);event_log_close(events);perf_log_close(perf);
    *events=next_events;*replay=next_replay;*perf=next_perf;
    events->observer_context=replay;
    r->next=now+3600;
    fprintf(stderr,"[log] hourly rotation: %s\n",path);fflush(stderr);
    event_log_write(events,LOG_INFO,"logging","hourly log segment started");
    return 1;
fail:
    replay_close(&next_replay,&next_events);event_log_close(&next_events);perf_log_close(&next_perf);
    r->next=now+60;
    fprintf(stderr,"[log] hourly rotation failed; continuing previous file, retry in 60s\n");fflush(stderr);
    return -1;
}

void log_rotation_prune(EventLog *events,PerfLog *perf,unsigned long long limit_bytes) {
    const char *event_path=events->db?sqlite3_db_filename(events->db,"main"):NULL;
    const char *perf_path=perf->db?sqlite3_db_filename(perf->db,"main"):NULL;
    char directory[1024];
    if(!event_path || !*event_path || strlen(event_path)>=sizeof(directory))return;
    snprintf(directory,sizeof(directory),"%s",event_path);
    char *slash=strrchr(directory,'/'),*back=strrchr(directory,'\\');
    if(back && (!slash || back>slash))slash=back;
    if(!slash)return;*slash=0;
    LogRetentionResult result=log_retention_clean(directory,event_path,perf_path,limit_bytes,(double)time(NULL));
    if(result.removed || result.failed || result.over_limit) {
        char message[240];
        snprintf(message,sizeof(message),"retention days=30 removed_groups=%u failed_groups=%u remaining_bytes=%llu limit_bytes=%llu over_limit=%d",result.removed,result.failed,result.bytes,limit_bytes,result.over_limit);
        event_log_write(events,result.over_limit||result.failed?LOG_WARN:LOG_INFO,"logging",message);
    }
}
