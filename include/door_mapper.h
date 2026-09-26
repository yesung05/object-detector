#ifndef DOOR_MAPPER_H
#define DOOR_MAPPER_H
#include "tracks.h"
#include <string.h>
#include <math.h>

/* Passage estimation, not semantic door recognition. Count completed movement
 * away from an appearance and disappearance after movement, never dwell frames.
 * Startup occupants, short tracks and temporary occlusion are excluded. */
typedef struct {
    int id, entry, ended, eligible;
    double first, last;
    float sx, sy, x, y, moved;
    Detection start, end;
} DoorPath;
typedef struct {
    float x, y, x1, y1, x2, y2;
    int entries, exits;
    double last;
} DoorCluster;
typedef struct {
    DoorPath paths[128];
    DoorCluster clusters[16];
    int ready, samples, width, height;
    float x1,y1,x2,y2;
    double started;
} DoorMapper;

static void door_mapper_event(DoorMapper *m, const Detection *b, int entry, double now) {
    float x=(b->x1+b->x2)*.5f/m->width, y=b->y2/m->height;
    int i, best=-1; float dist=.12f*.12f;
    for(i=0;i<16;i++) {
        DoorCluster *c=&m->clusters[i];
        if(c->entries+c->exits && now-c->last>600) memset(c,0,sizeof(*c));
        if(c->entries+c->exits) {
            float dx=x-c->x,dy=y-c->y,d=dx*dx+dy*dy;
            if(d<dist){best=i;dist=d;}
        }
    }
    if(best<0)for(i=0;i<16;i++)if(!m->clusters[i].entries&&!m->clusters[i].exits){best=i;break;}
    if(best<0)return;
    {
        DoorCluster *c=&m->clusters[best]; int n=c->entries+c->exits;
        c->x=(c->x*n+x)/(n+1);c->y=(c->y*n+y)/(n+1);
        c->x1=(c->x1*n+b->x1/m->width)/(n+1);
        c->y1=(c->y1*n+b->y1/m->height)/(n+1);
        c->x2=(c->x2*n+b->x2/m->width)/(n+1);
        c->y2=(c->y2*n+b->y2/m->height)/(n+1);
        if(entry)c->entries++;else c->exits++;
        c->last=now;
        if(n+1>m->samples)m->samples=n+1;
        if(n+1>=6 && c->entries>=2 && c->exits>=2) {
            m->x1=fmaxf(0,c->x1-.03f);m->y1=fmaxf(0,c->y1-.03f-(c->y2-c->y1)*.25f);
            m->x2=fminf(1,c->x2+.03f);m->y2=fminf(1,c->y2+.03f);
            if(m->x2-m->x1>.03f&&m->y2-m->y1>.08f)m->ready=1;
        }
    }
}
static void door_mapper_update(DoorMapper *m,const TrackList *tracks,int w,int h,double now) {
    size_t j;int i;
    if(w<=0||h<=0)return;
    if(m->width!=w||m->height!=h){memset(m,0,sizeof(*m));m->width=w;m->height=h;m->started=now;}
    if(m->ready)return;
    for(j=0;j<tracks->count;j++) {
        const Track *t=&tracks->items[j];DoorPath *p=NULL;int returning=0;
        float x,y,dx,dy,d;
        if(!t->active||now-t->last_seen>1||t->box.score<.4f)continue;
        for(i=0;i<128;i++)if(m->paths[i].id==t->id){p=&m->paths[i];break;}
        if(!p)for(i=0;i<128;i++)if(!m->paths[i].id||now-m->paths[i].last>60){p=&m->paths[i];memset(p,0,sizeof(*p));break;}
        if(!p)continue;
        if(p->ended){memset(p,0,sizeof(*p));returning=1;}
        x=(t->box.x1+t->box.x2)*.5f/w;y=t->box.y2/h;
        if(!p->id){p->id=t->id;p->first=now;p->sx=x;p->sy=y;p->start=t->box;p->eligible=returning?now>m->started+5:t->first_seen>m->started+5;}
        p->last=now;p->x=x;p->y=y;p->end=t->box;
        dx=x-p->sx;dy=y-p->sy;d=sqrtf(dx*dx+dy*dy);
        if(d>p->moved)p->moved=d;
        if(p->eligible&&!p->entry&&p->moved>.15f&&now-p->first>=2){door_mapper_event(m,&p->start,1,now);p->entry=1;}
    }
    for(i=0;i<128;i++) {
        DoorPath *p=&m->paths[i];
        if(p->id&&!p->ended&&now-p->last>5){
            p->ended=1;
            if(p->eligible&&p->moved>.15f&&p->last-p->first>=2)door_mapper_event(m,&p->end,0,now);
        }
    }
}
#endif
