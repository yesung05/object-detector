/* Offline evaluation adapter: RGB input resampled on a 2 Hz video-time grid.
 * Reuses production detector and surface code, but no tracker/camera-health layer.
 * Never loads/writes operational baselines. argv: config scratch person-model obj-model reuse */
#include "surface_monitor.h"
#include "yolo11.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
int main(int argc,char **argv){
 SurfaceConfig config;SurfaceMonitor *monitor;Detector *person,*object;DetectorOptions options={0};
 DetectionList persons={0},objects={0};EventLog log;unsigned char *rgb;char error[512]={0},json[SURFACE_JSON_MAX];
 int frame=0,capture_requested=0;unsigned long pruns=0,oruns=0;double person_s=0,object_s=0;
 const int width=1280,height=720,stride=1280*3;size_t bytes=(size_t)stride*height;
 if(argc!=6){fprintf(stderr,"config scratch person-model obj-model reuse\n");return 2;}
 if(surface_config_read(argv[1],&config,error,sizeof(error))){fprintf(stderr,"%s\n",error);return 2;}
 config.ai_reuse=atoi(argv[5]);monitor=surface_monitor_create(argv[2]);
 if(!monitor||surface_monitor_apply(monitor,&config,error,sizeof(error))){fprintf(stderr,"%s\n",error);return 2;}
 options.confidence=.25f;options.iou=.45f;options.max_candidates=1024;options.max_detections=64;options.threads=1;options.fast_preprocess=1;options.graph_optimization_all=1;options.provider=DETECTOR_PROVIDER_CPU;
 person=detector_create(argv[3],&options,error,sizeof(error));object=detector_create(argv[4],&options,error,sizeof(error));
 if(!person||!object){fprintf(stderr,"%s\n",error);return 2;}
 if(detection_list_init(&persons,1024)||detection_list_init(&objects,1024)||event_log_open(&log,":memory:",LOG_INFO,0))return 2;
 rgb=malloc(bytes);if(!rgb)return 2;
#ifdef _WIN32
 _setmode(_fileno(stdin),_O_BINARY);
#endif
 for(;;){size_t got=fread(rgb,1,bytes,stdin),i;SurfaceFrame sf={0};SurfaceObject people[64],found[128];SurfaceInspection request;double t=1.0+frame*.5,started;
  if(!got)break;if(got!=bytes){fprintf(stderr,"truncated raw frame\n");return 3;}
  started=platform_monotonic_seconds();if(detector_run(person,rgb,width,height,stride,&persons,error,sizeof(error))){fprintf(stderr,"%s\n",error);return 3;}person_s+=platform_monotonic_seconds()-started;pruns++;
  sf.people_count=persons.count<64?persons.count:64;
  for(i=0;i<sf.people_count;i++){Detection *d=&persons.items[i];SurfaceObject *p=&people[i];memset(p,0,sizeof(*p));p->id=(int)i;p->x1=d->x1/width;p->x2=d->x2/width;p->y1=d->y1/height;p->y2=d->y2/height;p->anchor_x=(p->x1+p->x2)*.5f;p->anchor_y=p->y1+(p->y2-p->y1)*.55f;
   if(d->keypoint_count>12&&d->kp[11].score>.4f&&d->kp[12].score>.4f){p->anchor_x=(d->kp[11].x+d->kp[12].x)*.5f/width;p->anchor_y=(d->kp[11].y+d->kp[12].y)*.5f/height;}}
  sf.rgb=rgb;sf.width=width;sf.height=height;sf.stride=stride;sf.now=t;sf.people_at=t;sf.camera_ok=sf.people_valid=1;sf.people=people;
  if(!capture_requested&&frame>=30){if(surface_monitor_capture(monitor,"table-1",0,error,sizeof(error))){fprintf(stderr,"%s\n",error);return 3;}capture_requested=1;}
  surface_monitor_update(monitor,&sf,&log);
  if(surface_monitor_poll_request(monitor,t,&request)){int x=(int)(request.x1*width),y=(int)(request.y1*height),w=(int)(request.x2*width)-x,h=(int)(request.y2*height)-y,success=0;size_t count=0;
   if(x>=0&&y>=0&&w>1&&h>1&&x+w<=width&&y+h<=height){started=platform_monotonic_seconds();success=detector_run(object,rgb+y*stride+x*3,w,h,stride,&objects,error,sizeof(error))==0;object_s+=platform_monotonic_seconds()-started;oruns++;
    if(success){count=objects.count<128?objects.count:128;for(i=0;i<count;i++){Detection *d=&objects.items[i];SurfaceObject *o=&found[i];memset(o,0,sizeof(*o));o->kind=(d->class_id==0||d->class_id==1)?1:2;o->x1=(d->x1+x)/width;o->x2=(d->x2+x)/width;o->y1=(d->y1+y)/height;o->y2=(d->y2+y)/height;}}}
   surface_monitor_submit_result(monitor,&request,found,count,success,t,&log);
  }
  surface_monitor_status(monitor,t,json,sizeof(json));printf("{\"video_s\":%.3f,\"person_count\":%zu,\"status\":%s}\n",frame*.5,sf.people_count,json);frame++;
 }
 fprintf(stderr,"EVAL frames=%d person_runs=%lu object_runs=%lu person_wall_s=%.6f object_wall_s=%.6f\n",frame,pruns,oruns,person_s,object_s);
 free(rgb);surface_monitor_destroy(monitor);detector_destroy(person);detector_destroy(object);detection_list_destroy(&persons);detection_list_destroy(&objects);event_log_close(&log);return 0;
}
