#ifndef REPLAY_H
#define REPLAY_H
#include "log.h"
#define REPLAY_JSON_MAX 32768
#define REPLAY_RING 52
typedef struct {double mono;int length;char json[REPLAY_JSON_MAX];} ReplayFrame;
typedef struct {
    sqlite3 *db;sqlite3_stmt *insert;ReplayFrame *ring;
    long long session,bytes,written,errors;
    int next,count,pending,full;
    double now,last_sample,last_regular,until,write_ms;
} ReplayLog;
int replay_open(ReplayLog *r,EventLog *log,const char *metadata);
void replay_close(ReplayLog *r,EventLog *log);
void replay_observe(void *context,long long id,LogLevel level,const char *module,const char *message);
int replay_due(const ReplayLog *r,double now);
void replay_sample(ReplayLog *r,double now,const char *json);
void replay_geometry(ReplayLog *r,int revision,const char *json);
#endif
