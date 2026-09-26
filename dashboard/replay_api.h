/* Included by server.c after the shared auth, response and filename helpers. */
#include <stdarg.h>
typedef struct {char *data;size_t size,used;int failed;} ReplayResponse;
static void replay_response_add(ReplayResponse *b,const char *fmt,...) {
    va_list args;int n;if(b->failed)return;
    va_start(args,fmt);n=vsnprintf(b->data+b->used,b->size-b->used,fmt,args);va_end(args);
    if(n<0||(size_t)n>=b->size-b->used){b->failed=1;return;}b->used+=(size_t)n;
}
static void replay_response_string(ReplayResponse *b,const unsigned char *text) {
    replay_response_add(b,"\"");
    for(const unsigned char *p=text?text:(const unsigned char*)"";*p;p++) {
        if(*p=='"'||*p=='\\')replay_response_add(b,"\\%c",*p);
        else if(*p<32)replay_response_add(b,"\\u%04x",*p);
        else replay_response_add(b,"%c",*p);
    }
    replay_response_add(b,"\"");
}
static void replay_api_error(SOCKET s,int code,const char *message) {
    char body[256];int n=snprintf(body,sizeof(body),"{\"error\":\"%s\"}",message);
    send_header(s,code,"application/json",n);send_all(s,body,n);
}
static long long replay_query_id(const char *query,const char *key) {
    char text[40],*end;long long value;query_get(query,key,text,sizeof(text));
    if(!text[0])return 0;value=_strtoi64(text,&end,10);return *end||value<0?-1:value;
}
static void serve_replay(SOCKET socket,const char *path,const char *query,const char *body,int body_len,int post) {
    char file[256],db_path[MAX_PATH],label[32];sqlite3 *db=NULL;sqlite3_stmt *st=NULL;
    ReplayResponse out={0};long long event=replay_query_id(query,"event"),session=replay_query_id(query,"session"),before=replay_query_id(query,"before");
    double when=0;int rc,count=0;
    query_get(query,"file",file,sizeof(file));
    if(!safe_filename(file)||!is_db_file(file)||strstr(file,"_perf.db")||event<0||session<0||before<0){replay_api_error(socket,400,"invalid_request");return;}
    snprintf(db_path,sizeof(db_path),"%s\\%s",g_logs,file);
    if(sqlite3_open_v2(db_path,&db,post?SQLITE_OPEN_READWRITE:SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK){replay_api_error(socket,404,"recording_not_found");goto done;}
    sqlite3_busy_timeout(db,250);
    if(post) {
        query_get(query,"label",label,sizeof(label));
        if(strcmp(path,"/api/replay/review")||!event||body_len<0||body_len>1000||
           (strcmp(label,"expected")&&strcmp(label,"suspected_false")&&strcmp(label,"unknown")&&strcmp(label,"unreviewed"))) {
            replay_api_error(socket,400,"invalid_review");goto done;
        }
        if(sqlite3_prepare_v2(db,"UPDATE replay_events SET review=?,note=? WHERE event_id=?",-1,&st,NULL)!=SQLITE_OK){replay_api_error(socket,404,"no_coordinate_recording");goto done;}
        sqlite3_bind_text(st,1,label,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,body?body:"",body_len,SQLITE_TRANSIENT);sqlite3_bind_int64(st,3,event);
        if(sqlite3_step(st)!=SQLITE_DONE){replay_api_error(socket,500,"review_save_failed");goto done;}
        if(!sqlite3_changes(db)){replay_api_error(socket,404,"event_not_found");goto done;}
        send_header(socket,200,"application/json",11);send_all(socket,"{\"ok\":true}",11);goto done;
    }
    out.size=8*1024*1024;out.data=(char*)malloc(out.size);
    if(!out.data){replay_api_error(socket,500,"out_of_memory");goto done;}
    if(!strcmp(path,"/api/replay/events")) {
        if(sqlite3_prepare_v2(db,"SELECT r.event_id,r.session,r.mono,e.ts_iso,e.level,e.module,e.message,r.review,r.note FROM replay_events r JOIN events e ON e.id=r.event_id WHERE (?=0 OR r.event_id<?) ORDER BY r.event_id DESC LIMIT 100",-1,&st,NULL)!=SQLITE_OK){replay_api_error(socket,404,"no_coordinate_recording");goto done;}
        sqlite3_bind_int64(st,1,before);sqlite3_bind_int64(st,2,before);
        replay_response_add(&out,"{\"events\":[");
        while((rc=sqlite3_step(st))==SQLITE_ROW) {
            replay_response_add(&out,"%s{\"id\":%lld,\"session\":%lld,\"mono\":%.6f",count++?",":"",sqlite3_column_int64(st,0),sqlite3_column_int64(st,1),sqlite3_column_double(st,2));
            const char *keys[]={"ts","level","module","message","review","note"};
            for(int i=0;i<6;i++){replay_response_add(&out,",\"%s\":",keys[i]);replay_response_string(&out,sqlite3_column_text(st,i+3));}
            replay_response_add(&out,"}");
        }
        if(rc!=SQLITE_DONE){replay_api_error(socket,500,"read_failed");goto done;}
        sqlite3_finalize(st);st=NULL;replay_response_add(&out,"],\"sessions\":[");count=0;
        if(sqlite3_prepare_v2(db,"SELECT id,created,metadata FROM replay_sessions ORDER BY id DESC LIMIT 100",-1,&st,NULL)!=SQLITE_OK){replay_api_error(socket,500,"read_failed");goto done;}
        while(sqlite3_step(st)==SQLITE_ROW){replay_response_add(&out,"%s{\"id\":%lld,\"created\":",count++?",":"",sqlite3_column_int64(st,0));replay_response_string(&out,sqlite3_column_text(st,1));replay_response_add(&out,",\"metadata\":%s}",sqlite3_column_text(st,2));}
        replay_response_add(&out,"]}");
    } else if(!strcmp(path,"/api/replay/frames")) {
        if(event) {
            if(sqlite3_prepare_v2(db,"SELECT session,mono FROM replay_events WHERE event_id=?",-1,&st,NULL)!=SQLITE_OK){replay_api_error(socket,404,"no_coordinate_recording");goto done;}
            sqlite3_bind_int64(st,1,event);if(sqlite3_step(st)!=SQLITE_ROW){replay_api_error(socket,404,"event_not_found");goto done;}
            session=sqlite3_column_int64(st,0);when=sqlite3_column_double(st,1);sqlite3_finalize(st);st=NULL;
        }
        if(!session){replay_api_error(socket,400,"session_required");goto done;}
        replay_response_add(&out,"{\"event\":%lld,\"session\":%lld,\"event_mono\":%.6f,\"frames\":[",event,session,when);
        const char *sql=event?"SELECT payload FROM replay_frames WHERE session=? AND mono BETWEEN ? AND ? ORDER BY mono LIMIT 160":"SELECT payload FROM (SELECT mono,payload FROM replay_frames WHERE session=? ORDER BY mono DESC LIMIT 120) ORDER BY mono";
        if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)!=SQLITE_OK){replay_api_error(socket,404,"no_coordinate_recording");goto done;}
        sqlite3_bind_int64(st,1,session);if(event){sqlite3_bind_double(st,2,when-10);sqlite3_bind_double(st,3,when+10);}
        while((rc=sqlite3_step(st))==SQLITE_ROW)replay_response_add(&out,"%s%s",count++?",":"",sqlite3_column_text(st,0));
        if(rc!=SQLITE_DONE){replay_api_error(socket,500,"read_failed");goto done;}
        sqlite3_finalize(st);st=NULL;replay_response_add(&out,"],\"geometry\":[");count=0;
        if(sqlite3_prepare_v2(db,"SELECT revision,payload FROM replay_geometry WHERE session=? ORDER BY revision DESC LIMIT 64",-1,&st,NULL)!=SQLITE_OK){replay_api_error(socket,500,"read_failed");goto done;}
        sqlite3_bind_int64(st,1,session);
        while(sqlite3_step(st)==SQLITE_ROW)replay_response_add(&out,"%s{\"revision\":%d,\"config\":%s}",count++?",":"",sqlite3_column_int(st,0),sqlite3_column_text(st,1));
        replay_response_add(&out,"]}");
    } else {replay_api_error(socket,404,"not_found");goto done;}
    if(out.failed){replay_api_error(socket,500,"response_too_large");goto done;}
    send_header(socket,200,"application/json",(int64_t)out.used);send_all(socket,out.data,(int)out.used);
done:
    if(st)sqlite3_finalize(st);if(db)sqlite3_close(db);free(out.data);
}
