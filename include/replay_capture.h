/* Internal main.c helper: accepts frame dimensions/index, never reads frame pixels. */
static unsigned long long replay_model_hash(const char *path) {
    unsigned long long hash=14695981039346656037ULL;unsigned char bytes[8192];size_t n;FILE *f;
    if(!path||!(f=fopen(path,"rb")))return 0;
    while((n=fread(bytes,1,sizeof(bytes),f))>0)for(size_t i=0;i<n;i++){hash^=bytes[i];hash*=1099511628211ULL;}
    fclose(f);return hash;
}
static double replay_norm(double x) {return isfinite(x)?fmin(1,fmax(0,x)):0;}
static void replay_capture_step(AppContext *app,const RgbFrame *frame,double now,const char *source) {
    char json[REPLAY_JSON_MAX];size_t pos=0;int count=0;double started;DetectorRunStats object_stats={0};
    ReplayLog *r=&app->replay;
    if(!replay_due(r,now)||frame->width<=0||frame->height<=0)return;
    started=platform_monotonic_seconds();
    if(app->obj_detector)detector_get_last_stats(app->obj_detector,&object_stats);
    if(!app->replay_cpu_wall||now-app->replay_cpu_wall>=10) {
        double cpu=platform_process_cpu_seconds();
        app->replay_cpu_pct=app->replay_cpu_wall?100*(cpu-app->replay_cpu_time)/(now-app->replay_cpu_wall):-1;
        app->replay_cpu_wall=now;app->replay_cpu_time=cpu;app->replay_mem_kb=platform_process_memory_kb();
    }
    if(app->surface_revision!=app->replay_geometry_revision&&app->surface_config_path[0]) {
        char geometry[65537];FILE *f=fopen(app->surface_config_path,"rb");
        if(f){size_t n=fread(geometry,1,sizeof(geometry)-1,f);fclose(f);geometry[n]=0;
            if(n)replay_geometry(r,app->surface_revision,geometry);app->replay_geometry_revision=app->surface_revision;}
    }
#define RJSON(...) do {int added=snprintf(json+pos,sizeof(json)-pos,__VA_ARGS__);if(added<0||(size_t)added>=sizeof(json)-pos)goto overflow;pos+=(size_t)added;} while(0)
    RJSON("{\"v\":1,\"t\":%.6f,\"frame\":%lld,\"w\":%d,\"h\":%d,\"source\":\"%s\",\"person_inference_age\":%.3f,\"geometry_revision\":%d,\"people\":[",now,(long long)frame->index,frame->width,frame->height,source,app->last_detection_time>0?now-app->last_detection_time:-1,app->surface_revision);
    for(size_t i=0;i<app->tracks.count && count<16;i++) {
        Track *t=&app->tracks.items[i];Detection *b=&t->box;double hold=0;int latched=0,fall_gate=0;
        if(!t->active)continue;
        for(size_t j=0;j<app->rules.capacity;j++)if(app->rules.states[j].track_id==t->id){
            TrackRuleState *s=&app->rules.states[j];hold=s->fall_start>0?now-s->fall_start:0;latched=s->fall_latched;fall_gate=s->fall_gate;break;}
        RJSON("%s{\"id\":%d,\"score\":%.3f,\"match_score\":%.3f,\"box\":[%.4f,%.4f,%.4f,%.4f],\"misses\":%d,\"seen_age\":%.3f,\"order\":%d,\"aspect\":%.3f,\"fall_hold\":%.2f,\"fall_latched\":%d,\"fall_gate\":%d,\"head\":[%d,%.4f,%.4f],\"kp\":[",
            count++?",":"",t->id,replay_norm(b->score),replay_norm(t->match_score),replay_norm(b->x1/frame->width),replay_norm(b->y1/frame->height),replay_norm(b->x2/frame->width),replay_norm(b->y2/frame->height),t->misses,now-t->last_seen,(int)t->order,
            b->y2>b->y1?(b->x2-b->x1)/(b->y2-b->y1):0,hold,latched,fall_gate,t->head_valid,replay_norm(t->head_cy_norm),replay_norm(t->head_y_fall_threshold_norm));
        for(int k=0;k<b->keypoint_count && k<YOLO11_NUM_KEYPOINTS;k++)RJSON("%s[%.4f,%.4f,%.3f]",k?",":"",replay_norm(b->kp[k].x/frame->width),replay_norm(b->kp[k].y/frame->height),replay_norm(b->kp[k].score));
        RJSON("]}");
    }
    RJSON("],\"people_truncated\":%s,\"low_people\":[",count>=16?"true":"false");
    for(size_t i=0;i<app->low_detections.count&&i<8;i++) {
        Detection *b=&app->low_detections.items[i];
        RJSON("%s{\"score\":%.3f,\"box\":[%.4f,%.4f,%.4f,%.4f]}",i?",":"",replay_norm(b->score),replay_norm(b->x1/frame->width),replay_norm(b->y1/frame->height),replay_norm(b->x2/frame->width),replay_norm(b->y2/frame->height));
    }
    RJSON("],\"objects_age\":%.3f,\"objects\":[",app->replay_tier2_at>0?now-app->replay_tier2_at:-1);
    for(size_t i=0;i<app->obj_detections.count&&i<24;i++) {
        Detection *b=&app->obj_detections.items[i];
        RJSON("%s{\"class\":%d,\"score\":%.3f,\"box\":[%.4f,%.4f,%.4f,%.4f]}",i?",":"",b->class_id,replay_norm(b->score),replay_norm(b->x1/frame->width),replay_norm(b->y1/frame->height),replay_norm(b->x2/frame->width),replay_norm(b->y2/frame->height));
    }
    RJSON("],\"door\":{\"state\":%d,\"phase\":%d,\"auto\":%d,\"box\":[%.4f,%.4f,%.4f,%.4f]},\"thresholds\":{\"person\":%.3f,\"object\":%.3f,\"new_person\":%.3f,\"table\":%.3f,\"fall_hold\":%.1f,\"fall_ratio\":[%.2f,%.2f]},",
        app->door.last_state,app->door.auto_phase,app->door_roi_auto,replay_norm((double)app->door.roi_x/frame->width),replay_norm((double)app->door.roi_y/frame->height),replay_norm((double)(app->door.roi_x+app->door.roi_w)/frame->width),replay_norm((double)(app->door.roi_y+app->door.roi_h)/frame->height),
        app->person_confidence,app->object_confidence,app->tracks.new_track_min_score,app->table_confidence,app->rules.config.fall_hold_seconds,app->rules.config.fall_aspect_ratio_kp,app->rules.config.fall_aspect_ratio_nokp);
    RJSON("\"perf\":{\"cpu_core_pct\":%.1f,\"memory_kb\":%ld,\"camera_fps\":%.2f,\"inference_fps\":%.2f,\"tier1_mean_ms\":%.2f,\"tier1_p95_ms\":%.2f,\"tier2_mean_ms\":%.2f,\"tier2_p95_ms\":%.2f,\"tier2_max_ms\":%.2f,\"recorder_previous_ms\":%.3f,\"camera_state\":%d,\"throttle\":%d},\"recording\":{\"errors\":%lld,\"full\":%d}}",
        app->replay_cpu_pct,app->replay_mem_kb,app->realtime_cam_fps,app->realtime_inf_fps,
        app->inference_runs?1000*(app->detector_stats.preprocess_seconds+app->detector_stats.inference_seconds+app->detector_stats.postprocess_seconds)/app->inference_runs:0,
        app->detector_stats.inference_p95_ms,
        app->obj_inference_runs?1000*(app->obj_detector_stats.preprocess_seconds+app->obj_detector_stats.inference_seconds+app->obj_detector_stats.postprocess_seconds)/app->obj_inference_runs:0,
        object_stats.inference_p95_ms,object_stats.inference_max_ms,r->write_ms,(int)app->cam_health.state,(int)app->throttle.level,r->errors,r->full);
    replay_sample(r,now,json);r->write_ms=(platform_monotonic_seconds()-started)*1000;
#undef RJSON
    return;
overflow:r->errors++;
}
