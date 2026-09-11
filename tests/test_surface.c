#include "surface_monitor.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p,0700)
#endif
static int failures=0;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);failures++;}}while(0)
static const char *valid_json=
"{\"schema_version\":1,\"revision\":1,\"enabled\":true,\"width\":100,\"height\":100,\"camera_id\":\"test\",\"surfaces\":["
"{\"id\":\"table-1\",\"type\":\"table\",\"locked\":true,\"polygon\":[[0.1,0.1],[0.9,0.1],[0.9,0.9],[0.1,0.9]],"
"\"usage_zones\":[[[0,0],[0.1,0],[0.1,0.1],[0,0.1]]],\"enter_seconds\":1,\"departure_seconds\":2,\"confirm_seconds\":2,\"clear_seconds\":2,\"fixtures\":[]}]}";
static unsigned char rgb[100*100*3];
static char status[32768],err[256];
static void update(SurfaceMonitor *m,SurfaceFrame *f,EventLog *log,double t) {
    f->now=t;f->people_at=t;surface_monitor_update(m,f,log);surface_monitor_status(m,t,status,sizeof(status));
}
static void dirt(int value) {
    int x,y;for(y=35;y<55;y++)for(x=35;x<55;x++)memset(rgb+(y*100+x)*3,value,3);
}
static int scheduler_scenario(const char *dir,SurfaceConfig config,int reuse) {
    SurfaceMonitor *m=surface_monitor_create(dir);SurfaceFrame f;SurfaceInspection q;
    SurfaceObject person;EventLog log;int requests=0;double t;
    config.ai_reuse=reuse;
    CHECK(surface_monitor_apply(m,&config,err,sizeof(err))==0);
    memset(&f,0,sizeof(f));memset(rgb,80,sizeof(rgb));
    f.rgb=rgb;f.width=f.height=100;f.stride=300;f.camera_ok=f.people_valid=1;
    event_log_open(&log,":memory:",LOG_INFO,0);
    /* Identical 50-second sequence and deterministic successful AI stub. */
    for(t=1;t<=50;t+=0.5){
        if(t>=4)dirt(180);update(m,&f,&log,t);
        if(surface_monitor_poll_request(m,t,&q)){requests++;surface_monitor_submit_result(m,&q,NULL,0,1,t+.01,&log);}
    }
    CHECK(strstr(status,"\"alert\":true")!=NULL);
    if(reuse){
        /* Same bounding box, different appearance must invalidate the cache. */
        dirt(230);update(m,&f,&log,51);
        CHECK(surface_monitor_poll_request(m,51,&q)==1);CHECK(q.candidate_index>=0);
        surface_monitor_submit_result(m,&q,NULL,0,0,51.01,&log);
        update(m,&f,&log,52);CHECK(surface_monitor_poll_request(m,52,&q)==0);
        for(t=52.5;t<=56.5;t+=.5)update(m,&f,&log,t);
        CHECK(surface_monitor_poll_request(m,56.5,&q)==1);
        surface_monitor_submit_result(m,&q,NULL,0,1,56.51,&log);
        /* Partial/full occlusion never clears the alert or reuses hidden appearance. */
        memset(&person,0,sizeof(person));person.x1=person.y1=.1f;person.x2=person.y2=.9f;
        person.anchor_x=person.anchor_y=.05f;f.people=&person;f.people_count=1;
        for(t=57;t<=59;t+=.5)update(m,&f,&log,t);
        CHECK(strstr(status,"\"alert\":true")!=NULL);
        f.people_count=0;for(t=59.5;t<=62;t+=.5)update(m,&f,&log,t);
        CHECK(surface_monitor_poll_request(m,62,&q)==1);CHECK(q.candidate_index==-1); /* audit is not starved */
        surface_monitor_submit_result(m,&q,NULL,0,1,62.01,&log);
        update(m,&f,&log,63);CHECK(surface_monitor_poll_request(m,63,&q)==0);
        /* Expiry restores inspection even for unchanged, already-alerted residue. */
        for(t=63.5;t<=117;t+=.5)update(m,&f,&log,t);
        CHECK(surface_monitor_poll_request(m,117,&q)==1);CHECK(q.candidate_index>=0);
        surface_monitor_submit_result(m,&q,NULL,0,1,117.01,&log);
        /* An occupied inspection gets one new check after departure. */
        person.x1=person.y1=0;person.x2=person.y2=.08f;f.people_count=1;
        for(t=117.5;t<=120;t+=.5)update(m,&f,&log,t);
        dirt(180);update(m,&f,&log,123);
        CHECK(surface_monitor_poll_request(m,123,&q)==1); /* overdue safety audit */
        surface_monitor_submit_result(m,&q,NULL,0,1,123.01,&log);
        for(t=123.5;t<=125;t+=.5)update(m,&f,&log,t);
        CHECK(surface_monitor_poll_request(m,125,&q)==1);CHECK(q.candidate_index>=0);
        surface_monitor_submit_result(m,&q,NULL,0,1,125.01,&log);
        for(t=125.5;t<=131;t+=.5)update(m,&f,&log,t);
        CHECK(surface_monitor_poll_request(m,131,&q)==0);
        f.people_count=0;for(t=131.5;t<=134;t+=.5)update(m,&f,&log,t);
        CHECK(surface_monitor_poll_request(m,134,&q)==1);CHECK(q.candidate_index>=0);
        /* Late success must not turn into a reusable success result. */
        for(t=134.5;t<=136.5;t+=.5)update(m,&f,&log,t);
        surface_monitor_submit_result(m,&q,NULL,0,1,136.5,&log);
        surface_monitor_status(m,136.5,status,sizeof(status));CHECK(strstr(status,"\"ai\":-1")!=NULL);
        for(t=137;t<=142;t+=.5)update(m,&f,&log,t);
        CHECK(surface_monitor_poll_request(m,142,&q)==1);
    }
    surface_monitor_destroy(m);event_log_close(&log);return requests;
}
int main(int argc, char **argv) {
    SurfaceConfig config,next;SurfaceMonitor *m;SurfaceFrame f;SurfaceObject person;
    SurfaceInspection q;EventLog log;char dir[160];double t;
    if(argc==2){int rc=surface_config_read(argv[1],&config,err,sizeof(err));printf("config: %s\n",rc?err:"valid");return rc?1:0;}
    CHECK(surface_config_parse(valid_json,&config,err,sizeof(err))==0);
    CHECK(config.count==1&&config.surfaces[0].usage_count==1);
    CHECK(surface_config_parse("{}",&next,err,sizeof(err))<0);
    CHECK(surface_config_parse("{broken}",&next,err,sizeof(err))<0);
    CHECK(surface_polygon_contains(&config.surfaces[0].polygon,0.5f,0.5f));
    CHECK(!surface_polygon_contains(&config.surfaces[0].polygon,0.01f,0.01f));
    next=config;next.revision=2;next.automatic=1;
    CHECK(surface_config_transition(&config,&next,err,sizeof(err))==0);
    next.surfaces[0].threshold++;
    CHECK(surface_config_transition(&config,&next,err,sizeof(err))<0);
    next.automatic=0;CHECK(surface_config_transition(&config,&next,err,sizeof(err))==0);
    next.revision=1;CHECK(surface_config_transition(&config,&next,err,sizeof(err))<0);
    snprintf(dir,sizeof(dir),"surface-test-%.0f",platform_monotonic_seconds()*1000000.0);CHECK(MKDIR(dir)==0);
    m=surface_monitor_create(dir);CHECK(m!=NULL);CHECK(surface_monitor_apply(m,&config,err,sizeof(err))==0);
    memset(&f,0,sizeof(f));memset(rgb,80,sizeof(rgb));f.rgb=rgb;f.width=f.height=100;f.stride=300;f.camera_ok=f.people_valid=1;
    event_log_open(&log,":memory:",LOG_INFO,0);
    update(m,&f,&log,1);CHECK(strstr(status,"\"baseline\":false")!=NULL);
    CHECK(surface_monitor_capture(m,"unknown",0,err,sizeof(err))<0);
    CHECK(surface_monitor_capture(m,"table-1",0,err,sizeof(err))==0);
    for(t=2;t<=4;t+=0.5)update(m,&f,&log,t);
    CHECK(strstr(status,"\"baseline\":true")!=NULL);
    /* A seated person is associated through usage zone, not tabletop bbox. */
    memset(&person,0,sizeof(person));person.x1=person.y1=0;person.x2=person.y2=0.08f;
    person.anchor_x=person.anchor_y=0.05f;f.people=&person;f.people_count=1;
    for(t=4.5;t<=6;t+=0.5)update(m,&f,&log,t);
    dirt(180);
    for(t=6.5;t<=12;t+=0.5)update(m,&f,&log,t);
    CHECK(strstr(status,"\"occupancy\":\"occupied\"")!=NULL);
    CHECK(strstr(status,"\"alert\":true")==NULL);
    CHECK(surface_monitor_poll_request(m,12,&q)==1);
    CHECK(surface_monitor_poll_request(m,12.1,&q)==0);
    {SurfaceInspection stale=q;stale.config_revision++;
     surface_monitor_submit_result(m,&stale,NULL,0,1,12.1,&log);}
    surface_monitor_submit_result(m,&q,NULL,0,1,12.1,&log);
    /* Brief absence followed by return must not alert. */
    f.people_count=0;update(m,&f,&log,12.5);update(m,&f,&log,13);
    f.people_count=1;update(m,&f,&log,13.5);CHECK(strstr(status,"\"alert\":true")==NULL);
    f.people_count=0;
    for(t=14;t<=20;t+=0.5)update(m,&f,&log,t);
    CHECK(strstr(status,"\"alert\":true")!=NULL);
    CHECK(surface_monitor_acknowledge(m,"table-1",0,1,1,&log)==0);
    surface_monitor_status(m,20,status,sizeof(status));
    CHECK(strstr(status,"\"acknowledged\":true")!=NULL);
    CHECK(strstr(status,"\"alert\":true")!=NULL);
    CHECK(surface_monitor_acknowledge(m,"table-1",0,99,1,&log)<0);
    CHECK(surface_monitor_acknowledge(m,"table-1",0,1,99,&log)<0);
    /* Occlusion must never resolve an existing alert. */
    person.x1=person.y1=0.1f;person.x2=person.y2=0.9f;f.people_count=1;
    dirt(80);for(t=20.5;t<=24;t+=0.5)update(m,&f,&log,t);
    CHECK(strstr(status,"\"alert\":true")!=NULL);
    f.people_count=0;for(t=24.5;t<=28;t+=0.5)update(m,&f,&log,t);
    CHECK(strstr(status,"\"candidates\":[]")!=NULL);
    /* Capture persisted across monitor recreation; no implicit re-learning. */
    surface_monitor_destroy(m);m=surface_monitor_create(dir);CHECK(surface_monitor_apply(m,&config,err,sizeof(err))==0);
    surface_monitor_status(m,30,status,sizeof(status));CHECK(strstr(status,"\"baseline\":true")!=NULL);
    /* Entire-frame change cannot become a new clean baseline. */
    memset(rgb,200,sizeof(rgb));update(m,&f,&log,30);CHECK(strstr(status,"\"quality\":4")!=NULL);
    memset(rgb,80,sizeof(rgb));update(m,&f,&log,31);CHECK(strstr(status,"\"quality\":4")==NULL);
    f.width=99;update(m,&f,&log,32);CHECK(strstr(status,"\"quality\":1")!=NULL);
    f.width=100;
    /* Stale detection batches cannot supply absence evidence. */
    f.now=35;f.people_at=1;surface_monitor_update(m,&f,&log);surface_monitor_status(m,35,status,sizeof(status));
    CHECK(strstr(status,"\"occupancy\":\"unknown\"")!=NULL);
    CHECK(strstr(status,"\"quality\":1")!=NULL);
    /* A long observation gap restarts departure confirmation. */
    update(m,&f,&log,36);update(m,&f,&log,50);
    CHECK(strstr(status,"\"occupancy\":\"vacant\"")==NULL);
    /* Animals are inspected while a customer is present. */
    person.x1=person.y1=0;person.x2=person.y2=0.08f;f.people_count=1;
    for(t=50.5;t<=52;t+=0.5)update(m,&f,&log,t);
    dirt(180);update(m,&f,&log,52.5);
    CHECK(surface_monitor_poll_request(m,52.5,&q)==1);
    {SurfaceObject animal;memset(&animal,0,sizeof(animal));animal.kind=1;
     animal.x1=animal.y1=0.35f;animal.x2=animal.y2=0.55f;animal.anchor_x=0.45f;animal.anchor_y=0.55f;
     surface_monitor_submit_result(m,&q,&animal,1,1,52.6,&log);
     update(m,&f,&log,53);update(m,&f,&log,53.5);update(m,&f,&log,54);
     CHECK(surface_monitor_poll_request(m,54,&q)==1);
     surface_monitor_submit_result(m,&q,&animal,1,1,54.1,&log);
     surface_monitor_status(m,54.1,status,sizeof(status));
     CHECK(strstr(status,"\"animal\":true")!=NULL);
     CHECK(strstr(status,"\"alert\":true")==NULL);}
    /* An obsolete revision cannot set an animal warning in a new configuration. */
    next=config;next.revision=2;CHECK(surface_monitor_apply(m,&next,err,sizeof(err))==0);
    surface_monitor_submit_result(m,&q,NULL,0,1,54.2,&log);
    surface_monitor_status(m,54.2,status,sizeof(status));CHECK(strstr(status,"\"candidates\":[]")!=NULL);
    surface_monitor_destroy(m);event_log_close(&log);
    {int periodic=scheduler_scenario(dir,config,0),reused=scheduler_scenario(dir,config,1);
     printf("synthetic static scene (50s): periodic=%d reused=%d candidate requests\n",periodic,reused);
     CHECK(reused<periodic);CHECK(reused==1);}
    printf("surface tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
