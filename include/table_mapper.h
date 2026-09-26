#ifndef TABLE_MAPPER_H
#define TABLE_MAPPER_H
#include "yolo11.h"
#include "rules.h"
#include <math.h>
#include <string.h>
typedef struct {float x1,y1,x2,y2;int hits;double first,last;} TableCandidate;
typedef struct {TableCandidate items[16];int width,height;} TableMapper;
static float table_overlap(const TableCandidate *a,const TableCandidate *b) {
    float w=fminf(a->x2,b->x2)-fmaxf(a->x1,b->x1),h=fminf(a->y2,b->y2)-fmaxf(a->y1,b->y1);
    return w>0&&h>0?w*h:0;
}
static void table_mapper_update(TableMapper *m,const DetectionList *ds,int w,int h,double now,float min_score) {
    size_t k;int i;int used[16]={0};
    if(w<1||h<1)return;
    if(m->width!=w||m->height!=h){memset(m,0,sizeof(*m));m->width=w;m->height=h;}
    for(i=0;i<16;i++)if(now-m->items[i].last>8)memset(&m->items[i],0,sizeof(m->items[i]));
    for(k=0;k<ds->count;k++) {
        const Detection *d=&ds->items[k];TableCandidate b={0};int best=-1;float score=.7f;
        if(d->class_id!=OBJ_DININGTABLE||d->score<min_score)continue;
        b.x1=fmaxf(0,d->x1/w);b.y1=fmaxf(0,d->y1/h);b.x2=fminf(1,d->x2/w);b.y2=fminf(1,d->y2/h);
        if(b.x2-b.x1<.05f||b.y2-b.y1<.04f)continue;
        for(i=0;i<16;i++)if(m->items[i].hits&&!used[i]){
            TableCandidate *a=&m->items[i];float inter=table_overlap(a,&b);
            float ratio=inter/((a->x2-a->x1)*(a->y2-a->y1)+(b.x2-b.x1)*(b.y2-b.y1)-inter);
            if(ratio>score){score=ratio;best=i;}
        }
        if(best<0)for(i=0;i<16;i++)if(!m->items[i].hits&&!used[i]){best=i;break;}
        if(best>=0){TableCandidate *a=&m->items[best];int n=a->hits;
            a->x1=(a->x1*n+b.x1)/(n+1);a->y1=(a->y1*n+b.y1)/(n+1);
            a->x2=(a->x2*n+b.x2)/(n+1);a->y2=(a->y2*n+b.y2)/(n+1);
            if(!n)a->first=now;a->hits++;a->last=now;used[best]=1;
        }
    }
}
static int table_mapper_ready(const TableCandidate *a,double now) {
    return a->hits>=5&&a->last-a->first>=10&&now-a->last<3;
}
#endif
