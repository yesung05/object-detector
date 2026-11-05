#include "table_mapper.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed at %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
    TableMapper m={0};Detection d={0};DetectionList ds={0};int i;
    ds.items=&d;ds.count=1;d.class_id=OBJ_DININGTABLE;d.score=.9f;
    d.x1=100;d.y1=200;d.x2=400;d.y2=400;
    for(i=0;i<6;i++)table_mapper_update(&m,&ds,1000,800,10+i*2,.55f);
    CHECK(table_mapper_ready(&m.items[0],20));CHECK(m.items[0].hits==6);
    CHECK(!table_mapper_ready(&m.items[0],24));
    memset(&m,0,sizeof(m));d.class_id=OBJ_CHAIR;
    for(i=0;i<6;i++)table_mapper_update(&m,&ds,1000,800,10+i*2,.55f);
    CHECK(m.items[0].hits==0);
    d.class_id=OBJ_DININGTABLE;d.score=.3f;
    table_mapper_update(&m,&ds,1000,800,25,.55f);CHECK(m.items[0].hits==0);
    d.score=.9f;table_mapper_update(&m,&ds,1000,800,26,.55f);
    table_mapper_update(&m,&ds,1000,800,40,.55f);CHECK(m.items[0].hits==1);
    table_mapper_update(&m,&ds,640,480,42,.55f);CHECK(m.items[0].hits==1);
    memset(&m,0,sizeof(m));d.score=.6f;
    table_mapper_update(&m,&ds,640,480,50,.8f);CHECK(m.items[0].hits==0);
    table_mapper_update(&m,&ds,640,480,51,.5f);CHECK(m.items[0].hits==1);
    /* 진행 통계: 임계값 미만 검출도 seen/below/max_score 에 잡혀야 "못 찾는 중"의 원인을 알 수 있다. */
    memset(&m,0,sizeof(m));d.class_id=OBJ_DININGTABLE;d.score=.3f;
    table_mapper_update(&m,&ds,640,480,60,.28f);   /* 기준 이상 → 후보 1 */
    table_mapper_update(&m,&ds,640,480,62,.5f);    /* 기준 미만 → 후보 불변, below 1 */
    CHECK(m.seen==2);CHECK(m.below==1);CHECK(m.max_score>.29f&&m.max_score<.31f);CHECK(m.started==60);
    {int cand,best;table_mapper_progress(&m,63,&cand,&best);CHECK(cand==1);CHECK(best==1);}
    d.class_id=OBJ_CHAIR;table_mapper_update(&m,&ds,640,480,64,.28f);CHECK(m.seen==2); /* 다른 클래스는 세지 않는다 */
    /* 현장 점수대(0.30)가 기본 임계값 0.28 에서 등록 가능해야 한다(이전 기본 0.55 로는 불가능했다). */
    memset(&m,0,sizeof(m));d.class_id=OBJ_DININGTABLE;d.score=.3f;
    for(i=0;i<6;i++)table_mapper_update(&m,&ds,1000,800,10+i*2,.28f);
    CHECK(table_mapper_ready(&m.items[0],20));
    memset(&m,0,sizeof(m));
    for(i=0;i<6;i++)table_mapper_update(&m,&ds,1000,800,10+i*2,.55f);
    CHECK(!table_mapper_ready(&m.items[0],20));CHECK(m.below==6);
    puts("table mapper tests passed");return 0;
}
