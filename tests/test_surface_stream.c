/* Deterministic stream/monitor fixture: does not open a camera or load a model. */
#include "surface_monitor.h"
#include "stream.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
int main(int argc,char **argv) {
    SurfaceMonitor *monitor;SurfaceConfig config;SurfaceFrame frame;EventLog log;
    unsigned char rgb[100*100*3];char directory[600],path[640],error[256],json[32768],id[40];
    int iteration;
    if(argc!=3)return 2;
    snprintf(directory,sizeof(directory),"%s/config",argv[1]);
    snprintf(path,sizeof(path),"%s/surfaces.json",directory);
    if(surface_config_read(path,&config,error,sizeof(error)))return 3;
    monitor=surface_monitor_create(directory);if(!monitor)return 4;
    if(surface_monitor_apply(monitor,&config,error,sizeof(error)))return 5;
    if(stream_start(atoi(argv[2]),argv[1]))return 6;
    event_log_open(&log,":memory:",LOG_INFO,0);
    memset(&frame,0,sizeof(frame));frame.rgb=rgb;frame.width=frame.height=100;frame.stride=300;
    frame.people_valid=frame.camera_ok=1;
    for(iteration=1;iteration<=600;iteration++) {
        int empty,candidate,revision,version;
        memset(rgb,80,sizeof(rgb));
        if(iteration>=100){int x,y;for(y=35;y<55;y++)for(x=35;x<55;x++)memset(rgb+(y*100+x)*3,180,3);}
        frame.now=iteration*0.5;frame.people_at=frame.now;
        if(stream_surface_capture_request(id,sizeof(id),&empty))surface_monitor_capture(monitor,id,empty,error,sizeof(error));
        if(stream_surface_ack_request(id,sizeof(id),&candidate,&revision,&version))surface_monitor_acknowledge(monitor,id,candidate,revision,version,&log);
        surface_monitor_update(monitor,&frame,&log);
        surface_monitor_status(monitor,frame.now,json,sizeof(json));stream_surface_status(json);
        stream_push(rgb,100,100,300);Sleep(50);
    }
    stream_stop();surface_monitor_destroy(monitor);event_log_close(&log);return 0;
}
