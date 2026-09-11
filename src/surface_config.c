#include "surface_monitor.h"
#include "../third_party/sqlite/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#endif

/* SQLite is already shipped by the application. Its strict JSON parser avoids
 * extending the legacy flat configuration parser with an incompatible grammar. */
static int fail(char *e,size_t n,const char *s) { if(e&&n) snprintf(e,n,"%s",s); return -1; }
static int identifier(const char *s) {
    size_t i,n=s?strlen(s):0;
    if(!n||n>=40) return 0;
    for(i=0;i<n;i++) if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
        (s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-')) return 0;
    return 1;
}
/* Statements own extracted strings; all reads/copies occur before finalize. */
static int extract(sqlite3 *db,const char *json,const char *path,sqlite3_stmt **stmt) {
    if(sqlite3_prepare_v2(db,"SELECT json_extract(?1,?2),json_type(?1,?2)",-1,stmt,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(*stmt,1,json,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(*stmt,2,path,-1,SQLITE_TRANSIENT);
    return sqlite3_step(*stmt)==SQLITE_ROW?0:-1;
}
static int number(sqlite3 *db,const char *j,const char *p,double def,double lo,double hi,double *out) {
    sqlite3_stmt *s=NULL; int ok=0;
    if(extract(db,j,p,&s)==0) {
        const char *t=(const char*)sqlite3_column_text(s,1);
        double v=t?sqlite3_column_double(s,0):def;
        ok=(!t||!strcmp(t,"integer")||!strcmp(t,"real")||!strcmp(t,"true")||!strcmp(t,"false"))&&isfinite(v)&&v>=lo&&v<=hi;
        if(ok)*out=v;
    }
    sqlite3_finalize(s); return ok?0:-1;
}
static int string(sqlite3 *db,const char *j,const char *p,char *out,size_t cap,const char *def) {
    sqlite3_stmt *s=NULL; int ok=0;
    if(extract(db,j,p,&s)==0) {
        const char *t=(const char*)sqlite3_column_text(s,1);
        const char *v=t?(const char*)sqlite3_column_text(s,0):def;
        if((!t||!strcmp(t,"text"))&&v&&strlen(v)<cap){strcpy(out,v);ok=1;}
    }
    sqlite3_finalize(s);return ok?0:-1;
}
static double cross(SurfacePoint a,SurfacePoint b,SurfacePoint c) {
    return (double)(b.x-a.x)*(c.y-a.y)-(double)(b.y-a.y)*(c.x-a.x);
}
static int on_segment(SurfacePoint a,SurfacePoint b,SurfacePoint p) {
    return fabs(cross(a,b,p))<1e-8&&p.x>=fminf(a.x,b.x)-1e-7f&&p.x<=fmaxf(a.x,b.x)+1e-7f&&
           p.y>=fminf(a.y,b.y)-1e-7f&&p.y<=fmaxf(a.y,b.y)+1e-7f;
}
static int intersects(SurfacePoint a,SurfacePoint b,SurfacePoint c,SurfacePoint d) {
    double u=cross(a,b,c),v=cross(a,b,d),w=cross(c,d,a),z=cross(c,d,b);
    return ((u*v<0)&&(w*z<0))||on_segment(a,b,c)||on_segment(a,b,d)||on_segment(c,d,a)||on_segment(c,d,b);
}
int surface_polygon_contains(const SurfacePolygon *p,float x,float y) {
    int i,j,inside=0; SurfacePoint q={x,y};
    for(i=0,j=p->count-1;i<p->count;j=i++) {
        SurfacePoint a=p->points[i],b=p->points[j];
        if(on_segment(a,b,q)) return 1;
        if(((a.y>y)!=(b.y>y))&&(x<(b.x-a.x)*(y-a.y)/(b.y-a.y)+a.x)) inside=!inside;
    }
    return inside;
}
static int polygon(sqlite3 *db,const char *json,SurfacePolygon *out) {
    sqlite3_stmt *s=NULL; int i=0,j; double area=0;
    memset(out,0,sizeof(*out));
    if(sqlite3_prepare_v2(db,"SELECT json_type(value),json_array_length(value),json_extract(value,'$[0]'),json_extract(value,'$[1]'),json_type(value,'$[0]'),json_type(value,'$[1]') FROM json_each(?1)",-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,json,-1,SQLITE_TRANSIENT);
    while(sqlite3_step(s)==SQLITE_ROW) {
        const char *t=(const char*)sqlite3_column_text(s,0),*tx=(const char*)sqlite3_column_text(s,4),*ty=(const char*)sqlite3_column_text(s,5);
        double x=sqlite3_column_double(s,2),y=sqlite3_column_double(s,3);
        if(i>=SURFACE_POINTS||!t||strcmp(t,"array")||sqlite3_column_int(s,1)!=2||!tx||!ty||
           (strcmp(tx,"real")&&strcmp(tx,"integer"))||(strcmp(ty,"real")&&strcmp(ty,"integer"))||
           !isfinite(x)||!isfinite(y)||x<0||x>1||y<0||y>1){sqlite3_finalize(s);return -1;}
        out->points[i].x=(float)x;out->points[i++].y=(float)y;
    }
    sqlite3_finalize(s);out->count=i;if(i<3)return -1;
    for(i=0;i<out->count;i++) {
        SurfacePoint a=out->points[i],b=out->points[(i+1)%out->count];
        if(fabs(a.x-b.x)+fabs(a.y-b.y)<1e-7)return -1;
        area+=(double)a.x*b.y-(double)b.x*a.y;
        for(j=i+1;j<out->count;j++) {
            if(j==i+1||(i==0&&j==out->count-1))continue;
            if(intersects(a,b,out->points[j],out->points[(j+1)%out->count]))return -1;
        }
    }
    return fabs(area)>1e-5?0:-1;
}
static int read_polygon(sqlite3 *db,const char *j,const char *path,SurfacePolygon *out,int optional) {
    sqlite3_stmt *s=NULL;int rc=-1;
    if(extract(db,j,path,&s)==0) {
        const char *type=(const char*)sqlite3_column_text(s,1);
        if(!type&&optional){memset(out,0,sizeof(*out));rc=0;}
        else if(type&&!strcmp(type,"array"))rc=polygon(db,(const char*)sqlite3_column_text(s,0),out);
    }
    sqlite3_finalize(s);return rc;
}
static int polygons(sqlite3 *db,const char *j,const char *path,SurfacePolygon *out,int *count) {
    sqlite3_stmt *s=NULL,*a=NULL;int rc=0;*count=0;
    if(extract(db,j,path,&s)!=0){sqlite3_finalize(s);return -1;}
    const char *t=(const char*)sqlite3_column_text(s,1);
    if(!t){sqlite3_finalize(s);return 0;}
    if(strcmp(t,"array")){sqlite3_finalize(s);return -1;}
    sqlite3_prepare_v2(db,"SELECT value FROM json_each(?1)",-1,&a,NULL);
    sqlite3_bind_text(a,1,(const char*)sqlite3_column_text(s,0),-1,SQLITE_TRANSIENT);
    while(sqlite3_step(a)==SQLITE_ROW) {
        if(*count>=SURFACE_ZONES||polygon(db,(const char*)sqlite3_column_text(a,0),&out[*count])){rc=-1;break;}
        ++*count;
    }
    sqlite3_finalize(a);sqlite3_finalize(s);return rc;
}
#define NUM(path,field,def,lo,hi) do { stage=path; if(number(db,j,path,def,lo,hi,&v))goto invalid; field=v; }while(0)
int surface_config_parse(const char *json,SurfaceConfig *out,char *error,size_t size) {
    sqlite3 *db=NULL;sqlite3_stmt *s=NULL,*list=NULL;double v;int rc=-1,i,k;SurfaceConfig c;
    int surface_index=-1,fixture_index=-1;
    const char *j=json,*stage="JSON syntax";memset(&c,0,sizeof(c));
    if(!json||strlen(json)>SURFACE_JSON_MAX)return fail(error,size,"surface config too large");
    if(sqlite3_open(":memory:",&db)!=SQLITE_OK)goto invalid;
    sqlite3_prepare_v2(db,"SELECT json_valid(?1)",-1,&s,NULL);sqlite3_bind_text(s,1,json,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(s)!=SQLITE_ROW||!sqlite3_column_int(s,0))goto invalid;
    sqlite3_finalize(s);s=NULL;
    NUM("$.schema_version",v,-1,1,1);
    NUM("$.revision",v,-1,1,2147483646);if(floor(v)!=v)goto invalid;c.revision=(int)v;
    NUM("$.enabled",v,0,0,1);if(floor(v)!=v)goto invalid;c.enabled=(int)v;
    NUM("$.ai_reuse",v,1,0,1);if(floor(v)!=v)goto invalid;c.ai_reuse=(int)v;
    NUM("$.width",v,-1,32,8192);if(floor(v)!=v)goto invalid;c.width=(int)v;
    NUM("$.height",v,-1,32,8192);if(floor(v)!=v)goto invalid;c.height=(int)v;
    NUM("$.geometry_revision",v,1,1,2147483646);if(floor(v)!=v)goto invalid;c.geometry_revision=(int)v;
    if(string(db,j,"$.camera_id",c.camera_id,sizeof(c.camera_id),"camera-1")||!identifier(c.camera_id))goto invalid;
    {char source[16];if(string(db,j,"$.source",source,sizeof(source),"manual"))goto invalid;
     if(strcmp(source,"manual")&&strcmp(source,"auto"))goto invalid;c.automatic=!strcmp(source,"auto");}
    stage="surfaces array";if(extract(db,j,"$.surfaces",&s))goto invalid;
    if(!sqlite3_column_text(s,1)||strcmp((const char*)sqlite3_column_text(s,1),"array"))goto invalid;
    sqlite3_prepare_v2(db,"SELECT value FROM json_each(?1)",-1,&list,NULL);
    sqlite3_bind_text(list,1,(const char*)sqlite3_column_text(s,0),-1,SQLITE_TRANSIENT);
    sqlite3_finalize(s);s=NULL;
    while(sqlite3_step(list)==SQLITE_ROW) {
        char type[16];SurfaceDefinition *d;sqlite3_stmt *fl=NULL;stage="surface ID/type/polygon/usage";
        surface_index=c.count;fixture_index=-1;
        if(c.count>=SURFACE_MAX)goto invalid;d=&c.surfaces[c.count++];
        j=(const char*)sqlite3_column_text(list,0);
        if(string(db,j,"$.id",d->id,sizeof(d->id),"")||!identifier(d->id)||
           string(db,j,"$.type",type,sizeof(type),""))goto invalid;
        d->type=!strcmp(type,"floor")?0:!strcmp(type,"table")?1:!strcmp(type,"wall")?2:-1;
        if(d->type<0||read_polygon(db,j,"$.polygon",&d->polygon,0)||
           polygons(db,j,"$.usage_zones",d->usage,&d->usage_count)||
           polygons(db,j,"$.exclusions",d->exclusions,&d->exclusion_count))goto invalid;
        if(d->type==1&&!d->usage_count)goto invalid;
        NUM("$.locked",v,0,0,1);if(floor(v)!=v)goto invalid;d->locked=(int)v;
        NUM("$.enter_seconds",d->enter_seconds,3,0.5,60);
        NUM("$.departure_seconds",d->departure_seconds,45,1,600);
        NUM("$.confirm_seconds",d->confirm_seconds,d->type==2?60:d->type==0?20:15,1,600);
        NUM("$.clear_seconds",d->clear_seconds,10,1,120);
        NUM("$.threshold",v,24,5,100);d->threshold=(float)v;
        NUM("$.min_area",v,0.003,0.0002,0.5);d->min_area=(float)v;
        stage="fixtures";if(extract(db,j,"$.fixtures",&s))goto invalid;
        if(sqlite3_column_text(s,1)) {
            if(strcmp((const char*)sqlite3_column_text(s,1),"array"))goto invalid;
            sqlite3_prepare_v2(db,"SELECT value FROM json_each(?1)",-1,&fl,NULL);
            sqlite3_bind_text(fl,1,(const char*)sqlite3_column_text(s,0),-1,SQLITE_TRANSIENT);
            while(sqlite3_step(fl)==SQLITE_ROW) {
                SurfaceFixture *f;const char *fj=(const char*)sqlite3_column_text(fl,0);
                fixture_index=d->fixture_count;
                stage="fixtures limit (maximum 8)";
                if(d->fixture_count>=SURFACE_FIXTURES){sqlite3_finalize(fl);goto invalid;}
                f=&d->fixtures[d->fixture_count++];
                stage="fixtures.id (unique ASCII letters, digits, underscore or hyphen)";
                if(string(db,fj,"$.id",f->id,sizeof(f->id),"")||!identifier(f->id)){sqlite3_finalize(fl);goto invalid;}
                stage="fixtures.polygon (3-16 distinct non-crossing points)";
                if(read_polygon(db,fj,"$.polygon",&f->polygon,0)){sqlite3_finalize(fl);goto invalid;}
                stage="fixtures.allowed (omit if unused; otherwise a valid polygon)";
                if(read_polygon(db,fj,"$.allowed",&f->allowed,1)){sqlite3_finalize(fl);goto invalid;}
                stage="fixtures.movable (boolean)";
                if(number(db,fj,"$.movable",0,0,1,&v)){sqlite3_finalize(fl);goto invalid;}
                if(floor(v)!=v){sqlite3_finalize(fl);goto invalid;}
                stage="fixtures.allowed required for movable fixture";
                f->movable=(int)v;if(f->movable&&!f->allowed.count){sqlite3_finalize(fl);goto invalid;}
                stage="fixtures.polygon outside surface ROI";
                for(i=0;i<f->polygon.count;i++)if(!surface_polygon_contains(&d->polygon,f->polygon.points[i].x,f->polygon.points[i].y)){
                    sqlite3_finalize(fl);goto invalid;}
            }
            sqlite3_finalize(fl);
        }
        sqlite3_finalize(s);s=NULL;
        fixture_index=-1;stage="duplicate surface ID";
        for(i=0;i<c.count-1;i++)if(!strcmp(c.surfaces[i].id,d->id))goto invalid;
        stage="duplicate fixture ID";
        for(i=0;i<d->fixture_count;i++)for(k=i+1;k<d->fixture_count;k++)
            if(!strcmp(d->fixtures[i].id,d->fixtures[k].id)){fixture_index=k;goto invalid;}
    }
    surface_index=-1;fixture_index=-1;
    if(c.enabled&&!c.count){stage="enabled monitor requires a surface";goto invalid;}
    /* Raster ownership is explicit: reject any overlap at monitoring resolution.
     * Polygon exclusions may carve holes, so bounding-box overlap is not enough. */
    stage="overlapping surfaces";
    for(i=0;i<SURFACE_GRID;i++)for(k=0;k<SURFACE_GRID;k++) {
        int owner=-1,n,e;float x=(i+0.5f)/SURFACE_GRID,y=(k+0.5f)/SURFACE_GRID;
        for(n=0;n<c.count;n++) {
            SurfaceDefinition *d=&c.surfaces[n];int inside=surface_polygon_contains(&d->polygon,x,y);
            for(e=0;e<d->exclusion_count;e++)if(surface_polygon_contains(&d->exclusions[e],x,y))inside=0;
            if(inside){if(owner>=0)goto invalid;owner=n;}
        }
    }
    *out=c;rc=0;goto done;
invalid:
    if(error&&size) {
        if(fixture_index>=0)snprintf(error,size,"Invalid surface config at %s (surface %d, fixture %d)",stage,surface_index+1,fixture_index+1);
        else if(surface_index>=0)snprintf(error,size,"Invalid surface config at %s (surface %d)",stage,surface_index+1);
        else snprintf(error,size,"Invalid surface config at %s",stage);
    }
done:
    sqlite3_finalize(s);sqlite3_finalize(list);sqlite3_close(db);return rc;
}
int surface_config_transition(const SurfaceConfig *a,const SurfaceConfig *b,char *e,size_t n) {
    int i,j;if(b->revision<=a->revision)return fail(e,n,"Revision conflict: reload configuration");
    if(b->automatic)for(i=0;i<a->count;i++)if(a->surfaces[i].locked) {
        for(j=0;j<b->count;j++)if(!strcmp(a->surfaces[i].id,b->surfaces[j].id))break;
        if(j==b->count||memcmp(&a->surfaces[i],&b->surfaces[j],sizeof(SurfaceDefinition))||
           a->width!=b->width||a->height!=b->height||a->geometry_revision!=b->geometry_revision||strcmp(a->camera_id,b->camera_id))
            return fail(e,n,"Automatic proposal changes a manually locked surface");
    }
    return 0;
}
int surface_config_read(const char *path,SurfaceConfig *out,char *e,size_t n) {
    FILE *f=fopen(path,"rb");char *buf;size_t len;int rc;
    if(!f)return fail(e,n,"Surface config unavailable");
    buf=(char*)malloc(SURFACE_JSON_MAX+2);if(!buf){fclose(f);return fail(e,n,"Out of memory");}
    len=fread(buf,1,SURFACE_JSON_MAX+1,f);fclose(f);buf[len]=0;
    rc=surface_config_parse(buf,out,e,n);free(buf);return rc;
}
int surface_file_replace(const char *path,const void *data,size_t length) {
    char tmp[1024];FILE *f;int ok;
    if(snprintf(tmp,sizeof(tmp),"%s.pending",path)>=(int)sizeof(tmp))return -1;
    f=fopen(tmp,"wb");if(!f)return -1;
    ok=fwrite(data,1,length,f)==length;if(fclose(f))ok=0;
    if(!ok){remove(tmp);return -1;}
#if defined(_WIN32)
    if(!MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){remove(tmp);return -1;}
#else
    if(rename(tmp,path)){remove(tmp);return -1;}
#endif
    return 0;
}
