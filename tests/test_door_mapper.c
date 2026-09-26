#include "door_mapper.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void observe(DoorMapper *m, Track *t, float x, double now) {
    TrackList list={0};list.items=t;list.count=1;
    t->active=1;t->last_seen=now;t->box.score=.9f;
    t->box.x1=x*1000;t->box.x2=x*1000+80;t->box.y1=200;t->box.y2=600;
    door_mapper_update(m,&list,1000,800,now);
}
static void passage(DoorMapper *m,int id,float start,float end,double now) {
    Track t={0};TrackList empty={0};t.id=id;t.first_seen=now;
    observe(m,&t,start,now);observe(m,&t,end,now+3);
    door_mapper_update(m,&empty,1000,800,now+9);
}
int main(void) {
    DoorMapper m={0};TrackList empty={0};Track t={0};int i;
    door_mapper_update(&m,&empty,1000,800,1);
    /* Dwell and detector startup occupants are not passages. */
    t.id=1;t.first_seen=1;
    for(i=0;i<20;i++)observe(&m,&t,.2f,2+i);
    CHECK(!m.ready&&m.samples==0);
    observe(&m,&t,.6f,23);CHECK(m.samples==0);
    memset(&m,0,sizeof(m));door_mapper_update(&m,&empty,1000,800,1);
    /* Repeated entry/exit endpoints, not repeated frames, establish the ROI. */
    for(i=0;i<3;i++)passage(&m,10+i,.15f,.60f,10+i*15);
    CHECK(!m.ready);
    for(i=0;i<3;i++)passage(&m,20+i,.60f,.15f,60+i*15);
    CHECK(m.ready);CHECK(m.samples>=6);CHECK(m.x1>=0&&m.x2<=1);
    /* Resolution change and explicit reset discard learned coordinates. */
    door_mapper_update(&m,&empty,640,480,120);CHECK(!m.ready&&m.samples==0);
    memset(&m,0,sizeof(m));CHECK(!m.ready&&m.samples==0);
    /* A brief disappearance does not vote as an exit. */
    door_mapper_update(&m,&empty,1000,800,1);
    memset(&t,0,sizeof(t));t.id=50;t.first_seen=10;
    observe(&m,&t,.15f,10);observe(&m,&t,.60f,13);
    door_mapper_update(&m,&empty,1000,800,15);CHECK(m.samples==1);
    puts("door mapper tests passed");return 0;
}
