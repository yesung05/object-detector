#include "surface_monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define G SURFACE_GRID
#define PIX (G*G)
#define CANDIDATES 16
typedef struct {
    int used,version,seen,count,alert,animal,animal_count,ai_state,pending,acknowledged;
    float x1,y1,x2,y2;
    double evidence,clear,first,last,last_ai,animal_at;
    char evidence_file[160];
    int cache_valid,cache_occupied;
    float cache_x1,cache_y1,cache_x2,cache_y2;
    double cache_at;
    double reuse_scan;
    unsigned long pending_serial;
    unsigned char cache_appearance[8*8*3];
} Candidate;
typedef struct {
    int ready,empty_ready,capturing,capture_empty,capture_count,occupancy,quality,layout;
    int overflow, geometry_invalid;
    float x1,y1,x2,y2;
    unsigned char mask[PIX],visible[PIX],changed[PIX],fixture_mask[PIX];
    unsigned char edge_distance[PIX],occluder_mask[PIX],occluder_hold[PIX];
    float occluder_fraction;
    int occluder_rejected;
    unsigned char baseline[PIX*3],empty[PIX*3],current[PIX*3];
    unsigned int capture_sum[PIX*3];
    int queue[PIX];
    double last_scan,enter,absence,last_valid,last_audit;
    int auto_capture_started; double auto_quiet; /* 자동 캡처: 이번 캡처가 자동 시작인지, 조건 유지 누적(초) */
    Candidate candidates[CANDIDATES];
} SurfaceRuntime;
struct SurfaceMonitor {
    SurfaceConfig config;
    SurfaceRuntime runtime[SURFACE_MAX];
    char directory[512];
    unsigned long serial;
    double last_dispatch;
    int candidate_sequence;
    unsigned long ai_requests,ai_reused,ai_reused_occupied,ai_reused_alert,ai_audits;
    int auto_capture; double auto_quiet_seconds; /* surface_monitor_set_auto_capture */
};
/* All runtime image arrays are monitor-owned, fixed-capacity, and released
 * together. Per-frame heap allocation is intentionally avoided. */
static int error_out(char *e,size_t n,const char *s){if(e&&n)snprintf(e,n,"%s",s);return -1;}
static float clamp(float v,float a,float b){return v<a?a:v>b?b:v;}
static uint32_t hash_bytes(uint32_t h,const void *data,size_t n) {
    const unsigned char *p=(const unsigned char*)data;while(n--){h^=*p++;h*=16777619u;}return h;
}
static uint32_t geometry_hash(const SurfaceConfig *c,int i) {
    const SurfaceDefinition *d=&c->surfaces[i];uint32_t h=2166136261u;
    h=hash_bytes(h,c->camera_id,strlen(c->camera_id));h=hash_bytes(h,&c->width,sizeof(int)*3);
    h=hash_bytes(h,&d->polygon,sizeof(d->polygon));h=hash_bytes(h,d->exclusions,sizeof(d->exclusions));
    h=hash_bytes(h,d->fixtures,sizeof(d->fixtures));
    if(d->occluder_count){h=hash_bytes(h,d->occluders,sizeof(d->occluders));h=hash_bytes(h,&d->occluder_dark_threshold,sizeof(float));h=hash_bytes(h,&d->occluder_max_fraction,sizeof(float));}
    return h;
}
static void baseline_path(const SurfaceMonitor *m,int i,int empty,char *out,size_t n) {
    snprintf(out,n,"%s/surface-%s-%08x-%s.bin",m->directory,m->config.surfaces[i].id,
             geometry_hash(&m->config,i),empty?"empty":"normal");
}
static void load_baseline(SurfaceMonitor *m,int i,int empty) {
    char path[1024];uint32_t head[4];FILE *f;SurfaceRuntime *r=&m->runtime[i];
    baseline_path(m,i,empty,path,sizeof(path));f=fopen(path,"rb");if(!f)return;
    if(fread(head,sizeof(head),1,f)==1&&head[0]==0x53524631&&head[1]==G&&head[2]==geometry_hash(&m->config,i)&&
       fread(empty?r->empty:r->baseline,1,PIX*3,f)==PIX*3) {
        if(empty)r->empty_ready=1;else r->ready=1;
    }
    fclose(f);
}
static int save_baseline(SurfaceMonitor *m,int i,int empty) {
    unsigned char *data=(unsigned char*)malloc(16+PIX*3);uint32_t head[4];char path[1024];int rc;
    if(!data)return -1;
    head[0]=0x53524631;head[1]=G;head[2]=geometry_hash(&m->config,i);head[3]=0;
    memcpy(data,head,16);memcpy(data+16,empty?m->runtime[i].empty:m->runtime[i].baseline,PIX*3);
    baseline_path(m,i,empty,path,sizeof(path));
    /* Preserve a timestamped prior capture before replacing the active reference. */
    {FILE *previous=fopen(path,"rb");if(previous){unsigned char old[16+PIX*3];size_t got=fread(old,1,sizeof(old),previous);char backup[1100];fclose(previous);
        snprintf(backup,sizeof(backup),"%s.history-%lld-%lu",path,(long long)time(NULL),++m->serial);
        if(surface_file_replace(backup,old,got)){free(data);return -1;}}}
    rc=surface_file_replace(path,data,16+PIX*3);free(data);return rc;
}
SurfaceMonitor *surface_monitor_create(const char *directory) {
    SurfaceMonitor *m=(SurfaceMonitor*)calloc(1,sizeof(*m));
    if(m){snprintf(m->directory,sizeof(m->directory),"%s",directory?directory:"config");m->auto_capture=1;m->auto_quiet_seconds=10;}return m;
}
void surface_monitor_destroy(SurfaceMonitor *m){free(m);}
int surface_monitor_enabled(const SurfaceMonitor *m){return m&&m->config.enabled;}
void surface_monitor_set_auto_capture(SurfaceMonitor *m,int enabled,double quiet_seconds){
    if(!m)return;m->auto_capture=enabled?1:0;m->auto_quiet_seconds=quiet_seconds>0?quiet_seconds:10;}
int surface_monitor_apply(SurfaceMonitor *m,const SurfaceConfig *c,char *e,size_t n) {
    int i,x,y,k;
    if(!m||!c)return error_out(e,n,"Invalid monitor");
    if(m->config.revision&&surface_config_transition(&m->config,c,e,n))return -1;
    /* New revision starts fresh evidence. Existing event history remains in SQLite. */
    m->config=*c;memset(m->runtime,0,sizeof(m->runtime));
    for(i=0;i<c->count;i++) {
        const SurfaceDefinition *d=&c->surfaces[i];SurfaceRuntime *r=&m->runtime[i];
        r->x1=r->y1=1;r->x2=r->y2=0;
        for(k=0;k<d->polygon.count;k++) {
            SurfacePoint p=d->polygon.points[k];r->x1=fminf(r->x1,p.x);r->y1=fminf(r->y1,p.y);
            r->x2=fmaxf(r->x2,p.x);r->y2=fmaxf(r->y2,p.y);
        }
        for(y=0;y<G;y++)for(x=0;x<G;x++) {
            float px=r->x1+(x+0.5f)/G*(r->x2-r->x1),py=r->y1+(y+0.5f)/G*(r->y2-r->y1);
            int inside=surface_polygon_contains(&d->polygon,px,py);
            for(k=0;k<d->exclusion_count;k++)if(surface_polygon_contains(&d->exclusions[k],px,py))inside=0;
            r->mask[y*G+x]=(unsigned char)inside;
        }
        load_baseline(m,i,0);load_baseline(m,i,1);
        /* Distance inside ROI to its boundary, bounded to an eight-cell band. */
        memset(r->edge_distance,255,sizeof(r->edge_distance));
        {int head=0,tail=0;for(k=0;k<PIX;k++)if(!r->mask[k]){r->edge_distance[k]=0;r->queue[tail++]=k;}
         for(y=0;y<G;y++)for(x=0;x<G;x++)if((!x||!y||x==G-1||y==G-1)&&r->edge_distance[y*G+x]==255){r->edge_distance[y*G+x]=1;r->queue[tail++]=y*G+x;}
         while(head<tail){int p=r->queue[head++],ns[4]={p-1,p+1,p-G,p+G},z;if(r->edge_distance[p]>=8)continue;
          for(z=0;z<4;z++){int q=ns[z];if(q<0||q>=PIX||(z==0&&p%G==0)||(z==1&&p%G==G-1)||r->edge_distance[q]!=255)continue;r->edge_distance[q]=r->edge_distance[p]+1;r->queue[tail++]=q;}}}
    }
    return 0;
}
int surface_monitor_capture(SurfaceMonitor *m,const char *id,int empty,char *e,size_t n) {
    int i;if(!m||!m->config.enabled)return error_out(e,n,"Surface monitor disabled");
    for(i=0;i<m->config.count;i++)if(!strcmp(id,m->config.surfaces[i].id)) {
        SurfaceRuntime *r=&m->runtime[i];r->capturing=1;r->capture_empty=empty;
        r->capture_count=0;memset(r->capture_sum,0,sizeof(r->capture_sum));return 0;
    }
    return error_out(e,n,"Unknown surface ID");
}
static int object_covers(const SurfaceObject *o,float x,float y) {
    return x>=o->x1-0.01f&&x<=o->x2+0.01f&&y>=o->y1-0.01f&&y<=o->y2+0.01f;
}
static float box_iou(const Candidate *a,float x1,float y1,float x2,float y2) {
    float w=fmaxf(0,fminf(a->x2,x2)-fmaxf(a->x1,x1)),h=fmaxf(0,fminf(a->y2,y2)-fmaxf(a->y1,y1));
    float inter=w*h,area=(a->x2-a->x1)*(a->y2-a->y1)+(x2-x1)*(y2-y1)-inter;
    return area>0?inter/area:0;
}
static void emit(SurfaceMonitor *m,int i,int j,const char *kind,double now,EventLog *log) {
    char msg[320];Candidate *c=&m->runtime[i].candidates[j];
    snprintf(msg,sizeof(msg),"%s surface=%s candidate=%d revision=%d evidence=%.1f hygiene=unsupported bbox=%.3f,%.3f,%.3f,%.3f",
        kind,m->config.surfaces[i].id,j,m->config.revision,c->evidence,c->x1,c->y1,c->x2,c->y2);
    event_log_write(log,(!strcmp(kind,"surface_cleared")||!strcmp(kind,"surface_acknowledged"))?LOG_INFO:LOG_WARN,"surface",msg);
    if(strcmp(kind,"surface_cleared")&&strcmp(kind,"surface_acknowledged")) {
        unsigned char image[16+PIX*3];uint32_t header[4]={0x53524631,G,0,0};char path[1024];
        memcpy(image,header,16);memcpy(image+16,m->runtime[i].current,PIX*3);
        snprintf(c->evidence_file,sizeof(c->evidence_file),"surface-%s-evidence-%lld-%lu.bin",m->config.surfaces[i].id,(long long)(now*1000000),++m->serial);
        snprintf(path,sizeof(path),"%s/%s",m->directory,c->evidence_file);
        if(surface_file_replace(path,image,sizeof(image))){c->evidence_file[0]=0;event_log_write(log,LOG_WARN,"surface","evidence image save failed");}
        else {snprintf(msg,sizeof(msg),"surface_evidence surface=%s candidate=%d file=%s",m->config.surfaces[i].id,j,c->evidence_file);event_log_write(log,LOG_INFO,"surface",msg);}
    }
    (void)now;
}
/* Match only registered fixture templates within allowed translation bounds.
 * Removal locations with no empty reference remain unknown, never auto-clean. */
static void fixtures(SurfaceRuntime *r,const SurfaceDefinition *d) {
    int fi,x,y;memset(r->fixture_mask,0,sizeof(r->fixture_mask));r->layout=0;
    for(fi=0;fi<d->fixture_count;fi++) {
        const SurfaceFixture *f=&d->fixtures[fi];int bestx=0,besty=0,best=1000,dx,dy;
        int range=f->movable?G/2:0;
        for(dy=-range;dy<=range;dy+=4)for(dx=-range;dx<=range;dx+=4) {
            int sum=0,count=0,valid=1;
            for(y=0;y<G&&valid;y+=3)for(x=0;x<G;x+=3) {
                float px=r->x1+(x+0.5f)/G*(r->x2-r->x1),py=r->y1+(y+0.5f)/G*(r->y2-r->y1);
                if(surface_polygon_contains(&f->polygon,px,py)) {
                    int xx=x+dx,yy=y+dy,k;
                    float nx=r->x1+(xx+0.5f)/G*(r->x2-r->x1),ny=r->y1+(yy+0.5f)/G*(r->y2-r->y1);
                    if(xx<0||yy<0||xx>=G||yy>=G||!r->visible[yy*G+xx]||
                       (f->movable&&!surface_polygon_contains(&f->allowed,nx,ny))){valid=0;break;}
                    for(k=0;k<3;k++)sum+=abs(r->current[(yy*G+xx)*3+k]-r->baseline[(y*G+x)*3+k]);
                    count+=3;
                }
            }
            if(valid&&count>=9&&sum/count<best){best=sum/count;bestx=dx;besty=dy;}
        }
        if(best>20)r->layout=1;
        else if(bestx||besty)r->layout=1;
        for(y=0;y<G;y++)for(x=0;x<G;x++) {
            float px=r->x1+(x+0.5f)/G*(r->x2-r->x1),py=r->y1+(y+0.5f)/G*(r->y2-r->y1);
            if(!surface_polygon_contains(&f->polygon,px,py))continue;
            /* Missing/unmatched fixture area is explicitly unknown, not evidence. */
            if(best>20||(!r->empty_ready&&(bestx||besty)))r->visible[y*G+x]=0;
            else if(bestx||besty)r->fixture_mask[y*G+x]=2; /* compare exposed empty surface */
            if(best<=20) {
                int xx=x+bestx,yy=y+besty;
                if(xx>=0&&xx<G&&yy>=0&&yy<G)r->fixture_mask[yy*G+xx]=1;
            }
        }
    }
}
/* Registered dark chair boundary, not a semantic object detector. Only dark
 * components connected outside the table can mask the current surface. Initial
 * dark boundary pixels have no empty reference and stay unknown, not auto-clean.
 * Interior isolated residue is never suppressed. No learned baseline writes. */
static int dark_pixel(const unsigned char *p,float threshold){return p[0]<=threshold&&p[1]<=threshold&&p[2]<=threshold;}
static void flood(unsigned char *mask,int *queue,int head,int tail) {
    while(head<tail){int p=queue[head++],ns[4]={p-1,p+1,p-G,p+G},z;
        for(z=0;z<4;z++){int q=ns[z];if(q<0||q>=PIX||(z==0&&p%G==0)||(z==1&&p%G==G-1)||mask[q]!=1)continue;mask[q]=2;queue[tail++]=q;}}
}
static void occluders(SurfaceRuntime *r,const SurfaceDefinition *d,const SurfaceFrame *f,int total) {
    unsigned char external[PIX],initial[PIX];int oi,k,x,y,count=0;
    memset(r->occluder_mask,0,sizeof(r->occluder_mask));r->occluder_fraction=0;r->occluder_rejected=0;
    if(!d->occluder_count)return;
    for(oi=0;oi<d->occluder_count;oi++) {
        const SurfacePolygon *polygon=&d->occluders[oi].polygon;float x1=1,y1=1,x2=0,y2=0;int tail=0,seeds=0;
        for(k=0;k<polygon->count;k++){x1=fminf(x1,polygon->points[k].x);x2=fmaxf(x2,polygon->points[k].x);y1=fminf(y1,polygon->points[k].y);y2=fmaxf(y2,polygon->points[k].y);}
        memset(external,0,sizeof(external));memset(initial,0,sizeof(initial));
        for(y=0;y<G;y++)for(x=0;x<G;x++) {
            int p=y*G+x,covered=0;float nx=x1+(x+.5f)/G*(x2-x1),ny=y1+(y+.5f)/G*(y2-y1);
            int xx=(int)clamp(nx*f->width,0,(float)f->width-1),yy=(int)clamp(ny*f->height,0,(float)f->height-1);
            if(!surface_polygon_contains(polygon,nx,ny)||!dark_pixel(f->rgb+(size_t)yy*f->stride+xx*3,d->occluder_dark_threshold))continue;
            for(k=0;k<(int)f->people_count;k++)if(object_covers(&f->people[k],nx,ny)){covered=1;break;}
            if(covered)continue;external[p]=1;
            if(!surface_polygon_contains(&d->polygon,nx,ny)){external[p]=2;r->queue[tail++]=p;seeds++;}
        }
        if(seeds>=8)flood(external,r->queue,0,tail);else memset(external,0,sizeof(external));
        tail=0;
        for(y=0;y<G;y++)for(x=0;x<G;x++) {
            int p=y*G+x,xx,yy;float nx=r->x1+(x+.5f)/G*(r->x2-r->x1),ny=r->y1+(y+.5f)/G*(r->y2-r->y1);
            if(!r->mask[p]||r->edge_distance[p]>8||!surface_polygon_contains(polygon,nx,ny))continue;
            xx=(int)((nx-x1)/(x2-x1)*G);yy=(int)((ny-y1)/(y2-y1)*G);
            if(xx>=0&&xx<G&&yy>=0&&yy<G&&external[yy*G+xx]==2)r->occluder_mask[p]=1;
            if(dark_pixel(r->baseline+p*3,d->occluder_dark_threshold)){
                initial[p]=1;if(r->edge_distance[p]<=1){initial[p]=2;r->queue[tail++]=p;}}
        }
        flood(initial,r->queue,0,tail);
        for(k=0;k<PIX;k++)if(initial[k]==2)r->occluder_mask[k]=1;
    }
    for(k=0;k<PIX;k++)if(r->mask[k]) {
        if(r->occluder_mask[k])r->occluder_hold[k]=5;
        else if(r->occluder_hold[k]){if(r->visible[k])r->occluder_hold[k]--;if(r->occluder_hold[k])r->occluder_mask[k]=1;}
        if(r->occluder_mask[k])count++;
    }
    if(count>total*d->occluder_max_fraction){r->occluder_rejected=1;memset(r->occluder_hold,0,sizeof(r->occluder_hold));memset(r->occluder_mask,0,sizeof(r->occluder_mask));return;}
    r->occluder_fraction=total?(float)count/total:0;
    for(k=0;k<PIX;k++)if(r->occluder_mask[k])r->visible[k]=0;
}
void surface_monitor_update(SurfaceMonitor *m,const SurfaceFrame *f,EventLog *log) {
    int i;if(!surface_monitor_enabled(m))return;
    for(i=0;i<m->config.count;i++) {
        SurfaceRuntime *r=&m->runtime[i];const SurfaceDefinition *d=&m->config.surfaces[i];
        double dt=f->now-r->last_scan;int x,y,k,total=0,visible=0,changed=0,present=0;
        int fresh=f->people_valid&&f->now-f->people_at<=2&&f->now>=f->people_at;
        int histogram[511]={0},samples=0,offset=0;
        if(r->last_scan&&dt<(d->type==2?2.0:0.5))continue;
        r->last_scan=f->now;r->quality=0;
        if(r->geometry_invalid){r->quality=6;continue;}
        if(!f->camera_ok||f->width!=m->config.width||f->height!=m->config.height||!fresh) {
            r->quality=1;r->occupancy=0;r->enter=r->absence=0;
            for(k=0;k<CANDIDATES;k++){r->candidates[k].evidence=0;r->candidates[k].clear=0;}
            continue;
        }
        if(dt>2.1||dt<0){dt=0;r->enter=r->absence=0;if(d->type==1)r->occupancy=0;}
        for(k=0;k<(int)f->people_count;k++) {
            int z;for(z=0;z<d->usage_count;z++)
                if(surface_polygon_contains(&d->usage[z],f->people[k].anchor_x,f->people[k].anchor_y))present=1;
        }
        if(d->type!=1)r->occupancy=1;
        else if(present) {
            r->absence=0;r->enter+=dt;
            if(r->enter>=d->enter_seconds||r->occupancy==2||r->occupancy==3)r->occupancy=2;
        } else {
            r->enter=0;r->absence+=dt;
            if(r->occupancy==2)r->occupancy=3;
            if(r->absence>=d->departure_seconds)r->occupancy=1;
        }
        for(y=0;y<G;y++)for(x=0;x<G;x++) {
            int p=y*G+x;float nx=r->x1+(x+0.5f)/G*(r->x2-r->x1),ny=r->y1+(y+0.5f)/G*(r->y2-r->y1);
            int xx=(int)clamp(nx*f->width,0,(float)f->width-1),yy=(int)clamp(ny*f->height,0,(float)f->height-1);
            const uint8_t *pixel=f->rgb+(size_t)yy*f->stride+xx*3;
            memcpy(r->current+p*3,pixel,3);r->visible[p]=r->mask[p];r->changed[p]=0;
            if(!r->mask[p])continue;total++;
            for(k=0;k<(int)f->people_count;k++)if(object_covers(&f->people[k],nx,ny))r->visible[p]=0;
            if(r->visible[p])visible++;
        }
        if(!total) {r->quality=2;continue;}
        if(visible<total*0.7)r->quality=2;
        if(r->capturing) {
            if(visible!=total){r->capture_count=0;memset(r->capture_sum,0,sizeof(r->capture_sum));r->quality=2;continue;}
            for(k=0;k<PIX*3;k++)r->capture_sum[k]+=r->current[k];
            if(++r->capture_count>=5) {
                unsigned char *target=r->capture_empty?r->empty:r->baseline;
                unsigned char backup[PIX*3];memcpy(backup,target,sizeof(backup));
                for(k=0;k<PIX*3;k++)target[k]=(unsigned char)(r->capture_sum[k]/5);
                if(save_baseline(m,i,r->capture_empty)==0){
                    char msg[200];if(r->capture_empty)r->empty_ready=1;else r->ready=1;
                    snprintf(msg,sizeof(msg),"surface_baseline_saved surface=%s kind=%s source=%s",d->id,r->capture_empty?"empty":"normal",r->auto_capture_started?"auto":"manual");
                    event_log_write(log,LOG_INFO,"surface",msg);
                } else {
                    char msg[200];memcpy(target,backup,sizeof(backup));r->quality=5;
                    snprintf(msg,sizeof(msg),"surface_baseline_save_failed surface=%s kind=%s — 디스크·config 폴더 확인",d->id,r->capture_empty?"empty":"normal");
                    event_log_write(log,LOG_WARN,"surface",msg);
                }
                r->capturing=0;r->auto_capture_started=0;
                /* Explicit baseline approval supersedes live evidence, not event history. */
                memset(r->candidates,0,sizeof(r->candidates));
            }
            continue;
        }
        if(!r->ready){
            /* 자동 캡처: 완전 가시(visible==total) + 테이블이면 비어 있음이 quiet 초 유지되면
               수동 캡처와 같은 5프레임 누적을 시작합니다. 위 capturing 분기가 다음 스캔부터 이어받습니다.
               quality=3(기준 없음)은 그대로 두어 대시보드가 "처음 설정 필요"를 계속 보이게 합니다. */
            if(m->auto_capture&&!r->capturing){
                if(visible==total&&(d->type!=1||r->occupancy==1)){
                    r->auto_quiet+=dt;
                    if(r->auto_quiet>=m->auto_quiet_seconds){
                        char msg[160];r->capturing=1;r->capture_empty=0;r->capture_count=0;
                        memset(r->capture_sum,0,sizeof(r->capture_sum));r->auto_capture_started=1;r->auto_quiet=0;
                        snprintf(msg,sizeof(msg),"surface_auto_capture_started surface=%s quiet=%.0fs",d->id,m->auto_quiet_seconds);
                        event_log_write(log,LOG_INFO,"surface",msg);
                    }
                }else r->auto_quiet=0;
            }
            r->quality=3;continue;
        }
        fixtures(r,d);
        occluders(r,d,f,total);
        if(d->occluder_count){visible=0;for(k=0;k<PIX;k++)visible+=r->visible[k]!=0;if(visible<total*.7)r->quality=2;}
        /* Robust brightness offset from visible, already-similar pixels. A large
         * unexplained change is held, never learned into the clean baseline. */
        for(k=0;k<PIX;k++)if(r->visible[k]&&!r->fixture_mask[k]) {
            int diff=(r->current[k*3]+r->current[k*3+1]+r->current[k*3+2]-
                      r->baseline[k*3]-r->baseline[k*3+1]-r->baseline[k*3+2])/3;
            if(abs(diff)<40){histogram[diff+255]++;samples++;}
        }
        if(samples>total/3){int sum=0;for(k=0;k<511;k++){sum+=histogram[k];if(sum>=samples/2){offset=k-255;break;}}}
        offset=(int)clamp((float)offset,-20,20);
        /* Compare stable baseline edges against small translated positions.
         * Strong common translation is a geometry fault, not a new stain. */
        {
            int dx,dy,center=0,best=2147483647,anchors=0;
            for(dy=-3;dy<=3;dy++)for(dx=-3;dx<=3;dx++) {
                int sum=0,count=0;
                for(y=4;y<G-4;y+=3)for(x=4;x<G-4;x+=3) {
                    int p=y*G+x,q=(y+dy)*G+x+dx;
                    if(!r->visible[p]||!r->visible[q]||r->fixture_mask[p])continue;
                    if(abs(r->baseline[p*3]-r->baseline[(p+1)*3])+abs(r->baseline[p*3]-r->baseline[(p+G)*3])<30)continue;
                    sum+=abs((int)r->current[q*3]-r->baseline[p*3]-offset);count++;
                }
                if(count>=20){int score=sum/count;if(!dx&&!dy){center=score;anchors=count;}else if(score<best)best=score;}
            }
            if(anchors>=20&&center>20&&best<center/2){r->geometry_invalid=1;r->quality=6;continue;}
        }
        for(k=0;k<PIX;k++)if(r->visible[k]&&r->fixture_mask[k]!=1) {
            const unsigned char *base=r->fixture_mask[k]==2?r->empty:r->baseline;int diff=0,ch;
            for(ch=0;ch<3;ch++){int v=abs((int)r->current[k*3+ch]-base[k*3+ch]-offset);if(v>diff)diff=v;}
            if(diff>d->threshold){r->changed[k]=1;changed++;}
        }
        if(changed>total*0.65){r->quality=4;r->last_valid=f->now;
            for(k=0;k<CANDIDATES;k++){r->candidates[k].evidence=0;r->candidates[k].clear=0;}continue;}
        for(k=0;k<CANDIDATES;k++)r->candidates[k].seen=0;
        r->overflow=0;
        for(k=0;k<PIX;k++)if(r->changed[k]) {
            int head=0,tail=0,minx=G,miny=G,maxx=0,maxy=0,count=0,best=-1,j;
            float x1,y1,x2,y2,bestiou=0;
            r->queue[tail++]=k;r->changed[k]=0;
            while(head<tail){int p=r->queue[head++],px=p%G,py=p/G,ns[4]={p-1,p+1,p-G,p+G},z;
                count++;if(px<minx)minx=px;if(px>maxx)maxx=px;if(py<miny)miny=py;if(py>maxy)maxy=py;
                for(z=0;z<4;z++){int q=ns[z];if(q<0||q>=PIX||(z==0&&px==0)||(z==1&&px==G-1))continue;
                    if(r->changed[q]){r->changed[q]=0;r->queue[tail++]=q;}}
            }
            if(count<3||count<total*d->min_area)continue;
            x1=r->x1+(float)minx/G*(r->x2-r->x1);x2=r->x1+(float)(maxx+1)/G*(r->x2-r->x1);
            y1=r->y1+(float)miny/G*(r->y2-r->y1);y2=r->y1+(float)(maxy+1)/G*(r->y2-r->y1);
            for(j=0;j<CANDIDATES;j++)if(r->candidates[j].used&&!r->candidates[j].seen){float ov=box_iou(&r->candidates[j],x1,y1,x2,y2);if(ov>bestiou){best=j;bestiou=ov;}}
            if(bestiou<0.2){best=-1;for(j=0;j<CANDIDATES;j++)if(!r->candidates[j].used){best=j;break;}}
            if(best<0){r->overflow=1;continue;}
            {Candidate *c=&r->candidates[best];
             if(!c->used){memset(c,0,sizeof(*c));c->used=1;c->version=++m->candidate_sequence;c->first=f->now;}
             else if(bestiou<0.5){c->version=++m->candidate_sequence;c->pending=0;c->last_ai=0;c->evidence=0;c->count=0;}
             if(bestiou<0.8)c->evidence=0;
             c->x1=x1;c->y1=y1;c->x2=x2;c->y2=y2;c->seen=1;c->clear=0;
             if(f->now-c->last>2.1){c->evidence=0;c->count=0;}
             if(r->occupancy==1&&!present)c->evidence+=dt;else c->evidence=0;
             c->last=f->now;c->count++;
             if(!c->alert&&c->count>=5&&c->evidence>=d->confirm_seconds){
                 c->alert=1;emit(m,i,best,d->type==1?"table_cleanup_suspected":d->type==0?"floor_contamination_suspected":"wall_change_suspected",f->now,log);}
            }
        }
        for(k=0;k<CANDIDATES;k++) {
            Candidate *c=&r->candidates[k];int valid=1,xx,yy;
            if(!c->used||c->seen)continue;
            for(yy=0;yy<G&&valid;yy++)for(xx=0;xx<G;xx++) {
                float nx=r->x1+(xx+0.5f)/G*(r->x2-r->x1),ny=r->y1+(yy+0.5f)/G*(r->y2-r->y1);
                if(nx>=c->x1&&nx<=c->x2&&ny>=c->y1&&ny<=c->y2&&r->mask[yy*G+xx]&&!r->visible[yy*G+xx]){valid=0;break;}
            }
            c->evidence=0;c->count=0;if(!valid){c->clear=0;continue;}
            if(c->animal_count&&f->now-c->animal_at<7){c->clear=0;continue;}
            c->clear+=dt;
            if(c->clear>=d->clear_seconds){if(c->alert||c->animal)emit(m,i,k,"surface_cleared",f->now,log);memset(c,0,sizeof(*c));}
        }
        r->last_valid=f->now;
    }
}
/* Descriptor is only reusable when the entire candidate rectangle is observed.
 * 8x8 RGB preserves coarse appearance, not semantic object identity. */
static int appearance(const SurfaceRuntime *r,const Candidate *c,unsigned char *out) {
    int x,y,k;
    for(y=0;y<G;y++)for(x=0;x<G;x++) {
        float px=r->x1+(x+0.5f)/G*(r->x2-r->x1),py=r->y1+(y+0.5f)/G*(r->y2-r->y1);
        if(px>=c->x1&&px<=c->x2&&py>=c->y1&&py<=c->y2&&(!r->mask[y*G+x]||!r->visible[y*G+x]))return 0;
    }
    for(y=0;y<8;y++)for(x=0;x<8;x++) {
        float px=c->x1+(x+0.5f)/8*(c->x2-c->x1),py=c->y1+(y+0.5f)/8*(c->y2-c->y1);
        int xx=(int)((px-r->x1)/(r->x2-r->x1)*G),yy=(int)((py-r->y1)/(r->y2-r->y1)*G);
        if(xx<0||xx>=G||yy<0||yy>=G||!r->mask[yy*G+xx]||!r->visible[yy*G+xx])return 0;
        for(k=0;k<3;k++)out[(y*8+x)*3+k]=r->current[(yy*G+xx)*3+k];
    }
    return 1;
}
static int similar_appearance(const unsigned char *a,const unsigned char *b) {
    int i,sum=0,large=0;
    for(i=0;i<8*8*3;i++){int d=abs((int)a[i]-b[i]);sum+=d;if(d>24)large++;}
    return sum<=8*8*3*8&&large<=8*8*3/10;
}
static int reuse_result(SurfaceMonitor *m,int i,Candidate *c,double now) {
    SurfaceRuntime *r=&m->runtime[i];unsigned char pixels[8*8*3];
    if(!c->cache_valid||!c->seen||r->quality!=0||now<c->cache_at||now-c->cache_at>=60)return 0;
    /* Suspected animals must still receive the second confirmation immediately. */
    if(c->animal_count&&!c->animal&&now-c->animal_at<7)return 0;
    /* Re-evaluate once after confirmed departure, even if the object looks identical. */
    if(m->config.surfaces[i].type==1&&r->occupancy==1&&c->cache_occupied)return 0;
    if(c->reuse_scan==r->last_scan)return 1; /* no repeated descriptor work between scans */
    if(box_iou(c,c->cache_x1,c->cache_y1,c->cache_x2,c->cache_y2)<0.8f||
       !appearance(r,c,pixels)||!similar_appearance(pixels,c->cache_appearance)) {
        c->cache_valid=0;return 0;
    }
    if(c->reuse_scan!=r->last_scan){c->reuse_scan=r->last_scan;m->ai_reused++;
        if(r->occupancy!=1)m->ai_reused_occupied++;
        if(c->alert||c->animal)m->ai_reused_alert++;}
    return 1;
}
int surface_monitor_poll_request(SurfaceMonitor *m,double now,SurfaceInspection *out) {
    int i,j,bi=-1,bj=-1;double best=-1;
    if(!surface_monitor_enabled(m)||now-m->last_dispatch<1)return 0;
    for(i=0;i<m->config.count;i++) {
        SurfaceRuntime *r=&m->runtime[i];
        if((r->quality!=0&&r->quality!=2)||now-r->last_valid>2.1)continue;
        for(j=0;j<CANDIDATES;j++) {
            Candidate *c=&r->candidates[j];double age;
            if(!c->used||(!c->seen&&!(c->animal_count&&now-c->animal_at<7))||c->pending||
               (c->last_ai&&now-c->last_ai<(c->animal_count?1:5)))continue;
            if(m->config.ai_reuse&&reuse_result(m,i,c,now))continue;
            age=now-(c->last_ai?c->last_ai:c->first)+(!c->last_ai?5:0);
            if(age>best){best=age;bi=i;bj=j;}
        }
    }
    if(bi<0||m->config.ai_reuse) {
        /* Low-frequency whole-surface audit catches candidates missed by differencing.
         * It also checks animals independently of stationary-residue requirements. */
        for(i=0;i<m->config.count;i++) {
            SurfaceRuntime *r=&m->runtime[i];
            if((r->quality!=0&&r->quality!=2&&r->quality!=4)||now-r->last_valid>2.1||
               now-r->last_audit<(r->quality==4?5:60))continue;
            memset(out,0,sizeof(*out));out->serial=++m->serial;out->config_revision=m->config.revision;
            out->surface_index=i;out->candidate_index=-1;out->observed_at=now;
            out->x1=r->x1;out->y1=r->y1;out->x2=r->x2;out->y2=r->y2;
            r->last_audit=now;m->last_dispatch=now;m->ai_requests++;m->ai_audits++;return 1;
        }
        if(bi<0)return 0;
    }
    {Candidate *c=&m->runtime[bi].candidates[bj];float margin=0.04f;
     memset(out,0,sizeof(*out));out->serial=++m->serial;out->config_revision=m->config.revision;
     out->surface_index=bi;out->candidate_index=bj;out->candidate_version=c->version;out->observed_at=now;
     out->appearance_valid=appearance(&m->runtime[bi],c,out->appearance);
     out->occupied=m->runtime[bi].occupancy!=1;
     out->x1=clamp(c->x1-margin,0,1);out->y1=clamp(c->y1-margin,0,1);out->x2=clamp(c->x2+margin,0,1);out->y2=clamp(c->y2+margin,0,1);
     c->pending=1;c->pending_serial=out->serial;m->last_dispatch=now;m->ai_requests++;}
    return 1;
}
void surface_monitor_submit_result(SurfaceMonitor *m,const SurfaceInspection *q,const SurfaceObject *objects,size_t count,int success,double now,EventLog *log) {
    Candidate *c;SurfaceRuntime *r;size_t i;int animal=0;
    int ci=q->candidate_index;
    if(!m||q->config_revision!=m->config.revision||q->surface_index<0||q->surface_index>=m->config.count||ci>=CANDIDATES)return;
    r=&m->runtime[q->surface_index];
    if(ci<0) {
        if(!success||now-q->observed_at>2)return;
        for(i=0;i<count;i++)if(objects[i].kind==1&&surface_polygon_contains(&m->config.surfaces[q->surface_index].polygon,objects[i].anchor_x,objects[i].anchor_y)) {
            int k;for(k=0;k<CANDIDATES;k++)if(!r->candidates[k].used){ci=k;break;}
            if(ci<0)return;c=&r->candidates[ci];memset(c,0,sizeof(*c));c->used=c->seen=1;
            c->version=++m->candidate_sequence;c->x1=objects[i].x1;c->y1=objects[i].y1;c->x2=objects[i].x2;c->y2=objects[i].y2;
            c->first=c->last=now;break;
        }
        if(ci<0)return;
    }
    c=&r->candidates[ci];
    if(!c->used||(q->candidate_index>=0&&(q->candidate_version!=c->version||!c->pending||q->serial!=c->pending_serial)))return;
    c->pending=0;c->last_ai=now;c->ai_state=success?1:-1;
    c->cache_valid=0;
    if(!success||now<q->observed_at||now-q->observed_at>2||(!c->seen&&!c->animal_count)){c->ai_state=-1;return;}
    if(q->candidate_index>=0&&q->appearance_valid&&r->quality==0) {
        unsigned char pixels[8*8*3];
        if(appearance(r,c,pixels)&&similar_appearance(pixels,q->appearance)) {
            c->cache_valid=1;c->cache_at=now;c->cache_occupied=q->occupied;
            c->cache_x1=c->x1;c->cache_y1=c->y1;c->cache_x2=c->x2;c->cache_y2=c->y2;
            memcpy(c->cache_appearance,q->appearance,sizeof(c->cache_appearance));
        }
    }
    for(i=0;i<count;i++)if(objects[i].kind==1&&
        surface_polygon_contains(&m->config.surfaces[q->surface_index].polygon,objects[i].anchor_x,objects[i].anchor_y))animal=1;
    if(animal) {
        if(now-c->animal_at>7)c->animal_count=0;
        c->animal_at=now;c->animal_count++;
        if(c->animal_count>=2&&!c->animal&&m->config.surfaces[q->surface_index].type==1){
            c->animal=1;emit(m,q->surface_index,ci,"animal_on_table_suspected",now,log);}
    }else c->animal_count=0;
}
void surface_monitor_status(const SurfaceMonitor *m,double now,char *json,size_t size) {
    size_t pos=0;int i,j;
    if(!m||!size)return;
#define APPEND(...) do{int written=snprintf(json+pos,size-pos,__VA_ARGS__);if(written<0||(size_t)written>=size-pos){snprintf(json,size,"{\"error\":\"status too large\"}");return;}pos+=(size_t)written;}while(0)
    APPEND("{\"enabled\":%s,\"revision\":%d,\"frame_time\":%.3f,\"hygiene\":\"unsupported\",\"ai_scheduler\":{\"reuse_enabled\":%s,\"requests\":%lu,\"audits\":%lu,\"reuse_observations\":%lu,\"occupied_reuse\":%lu,\"alert_reuse\":%lu},\"surfaces\":[",m->config.enabled?"true":"false",m->config.revision,now,m->config.ai_reuse?"true":"false",m->ai_requests,m->ai_audits,m->ai_reused,m->ai_reused_occupied,m->ai_reused_alert);
    for(i=0;i<m->config.count;i++) {
        const SurfaceRuntime *r=&m->runtime[i];const char *states[]={"unknown","vacant","occupied","departure_pending"};
        APPEND("%s{\"id\":\"%s\",\"occupancy\":\"%s\",\"quality\":%d,\"baseline\":%s,\"empty_baseline\":%s,\"capturing\":%s,\"capture_count\":%d,\"layout_changed\":%s,\"overflow\":%s,\"absence_seconds\":%.1f,\"age\":%.1f,",
            i?",":"",m->config.surfaces[i].id,states[r->occupancy],r->quality,r->ready?"true":"false",r->empty_ready?"true":"false",r->capturing?"true":"false",r->capture_count,r->layout?"true":"false",r->overflow?"true":"false",r->absence,r->last_scan?now-r->last_scan:0);
        APPEND("\"occluder_fraction\":%.5f,\"occluder_rejected\":%s,\"auto_capture\":%s,\"auto_quiet\":%.1f,",r->occluder_fraction,r->occluder_rejected?"true":"false",(r->capturing&&r->auto_capture_started)?"true":"false",r->auto_quiet);
        APPEND("\"reference\":\"surface-%s-%08x-normal.bin\",\"candidates\":[",m->config.surfaces[i].id,geometry_hash(&m->config,i));
        {int first=1;for(j=0;j<CANDIDATES;j++)if(r->candidates[j].used){const Candidate *c=&r->candidates[j];
            APPEND("%s{\"id\":%d,\"version\":%d,\"alert\":%s,\"animal\":%s,\"acknowledged\":%s,\"visible\":%s,\"ai\":%d,\"evidence_file\":\"%s\",\"evidence\":%.1f,\"bbox\":[%.4f,%.4f,%.4f,%.4f]}",first?"":",",j,c->version,c->alert?"true":"false",c->animal?"true":"false",c->acknowledged?"true":"false",c->seen?"true":"false",c->ai_state,c->evidence_file,c->evidence,c->x1,c->y1,c->x2,c->y2);first=0;}}
        APPEND("]}");
    }
    APPEND("]}");
#undef APPEND
}
int surface_monitor_acknowledge(SurfaceMonitor *m,const char *id,int candidate,int revision,int version,EventLog *log) {
    int i;if(!m||revision!=m->config.revision||candidate<0||candidate>=CANDIDATES)return -1;
    for(i=0;i<m->config.count;i++)if(!strcmp(id,m->config.surfaces[i].id)) {
        Candidate *c=&m->runtime[i].candidates[candidate];
        if(!c->used||version!=c->version||(!c->alert&&!c->animal))return -1;
        c->acknowledged=1;emit(m,i,candidate,"surface_acknowledged",c->last,log);return 0;
    }
    return -1;
}
