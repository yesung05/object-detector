/* Internal to door.c; uses its pixel comparison and reference helpers. */
static void passage_clear(DoorMonitor *d) {
    free(d->passage_base);d->passage_base=NULL;
    free(d->passage_candidate);d->passage_candidate=NULL;
    free(d->passage_previous);d->passage_previous=NULL;
    d->passage_cycles=d->passage_inside=d->passage_moved=0;
    d->passage_quiet=d->passage_hold=d->passage_return=0;
}
static double passage_background_diff(const uint8_t *base,const uint8_t *rgb,
    int w,int h,int stride,int x0,int y0,int x1,int y1,const GrayRect *people,int count) {
    double sum=0;int samples=0;
    for(int y=4;y<h;y+=8)for(int x=4;x<w;x+=8) {
        int covered=x>=x0&&x<x1&&y>=y0&&y<y1;
        for(int i=0;i<count&&!covered;i++)
            covered=x>=people[i].x1-8&&x<=people[i].x2+8&&y>=people[i].y1-8&&y<=people[i].y2+8;
        if(covered)continue;
        for(int c=0;c<3;c++)sum+=abs((int)base[((size_t)y*w+x)*3+c]-(int)rgb[(size_t)y*stride+x*3+c]);
        samples++;
    }
    return samples>=8?sum/(samples*3):255.0;
}
static int door_passage_update(DoorMonitor *d,const uint8_t *rgb,int w,int h,int stride,
    const GrayRect *people,int count,int camera_ok,double now,const char *closed_path,const char *open_path) {
    int x0=d->roi_x<0?0:d->roi_x,y0=d->roi_y<0?0:d->roi_y;
    int x1=d->roi_x+d->roi_w,y1=d->roi_y+d->roi_h,by,occupied=0,blocked=0;
    float cx=0,cy=0;
    double motion=255,change=0,background=255;
    if(x1>w)x1=w;if(y1>h)y1=h;
    by=band_bottom(d,y0,y1);
    d->auto_wait_seconds=0;d->auto_stalled=0;
    if(!camera_ok || w!=d->passage_w || h!=d->passage_h ||
       x0!=d->passage_x || y0!=d->passage_y || x1-x0!=d->passage_rw || by-y0!=d->passage_rh ||
       (d->passage_last>0 && now-d->passage_last>2.0)) {
        passage_clear(d);
        d->passage_w=w;d->passage_h=h;d->passage_x=x0;d->passage_y=y0;
        d->passage_rw=x1-x0;d->passage_rh=by-y0;
    }
    d->passage_last=now;
    if(!camera_ok || x1-x0<8 || by-y0<4 || d->band_ratio<=0){d->auto_phase=DOOR_AUTO_PASSAGE_BLOCKED;return 0;}
    for(int i=0;i<count;i++) {
        const GrayRect *p=&people[i];
        if(p->x2<=x0||p->x1>=x1||p->y2<=y0||p->y1>=y1)continue;
        if(!occupied){cx=(p->x1+p->x2)*.5f;cy=(p->y1+p->y2)*.5f;}
        occupied=1;
        if(p->y1<by+4)blocked=1;
    }
    if(d->passage_previous)motion=avg_l1(d->passage_previous,rgb,w,stride,x0,y0,x1,by);
    if(!d->passage_previous) {
        if(!install_reference(&d->passage_previous,&d->passage_w,&d->passage_h,rgb,w,h,stride))return -1;
    } else for(int y=0;y<h;y++)memcpy(d->passage_previous+(size_t)y*w*3,rgb+(size_t)y*stride,(size_t)w*3);
    if(!d->passage_base) {
        d->auto_phase=DOOR_AUTO_PASSAGE_BASE;
        if(occupied||motion>=3){d->passage_quiet=0;return 0;}
        if(d->passage_quiet<=0)d->passage_quiet=now;
        d->auto_wait_seconds=now-d->passage_quiet;
        if(d->auto_wait_seconds<d->auto_quiet_seconds)return 0;
        if(!install_reference(&d->passage_base,&d->passage_w,&d->passage_h,rgb,w,h,stride))return -1;
        d->passage_quiet=0;d->auto_phase=DOOR_AUTO_PASSAGE_WAIT;return 0;
    }
    change=avg_l1(d->passage_base,rgb,w,stride,x0,y0,x1,by);
    background=passage_background_diff(d->passage_base,rgb,w,h,stride,x0,y0,x1,y1,people,count);
    if(occupied) {
        if(!d->passage_inside) {
            d->passage_inside=1;d->passage_started=now;d->passage_observed=0;
            d->passage_start_x=cx;d->passage_start_y=cy;d->passage_moved=0;
        }
        if(fabs(cx-d->passage_start_x)>(x1-x0)*.15 || fabs(cy-d->passage_start_y)>(y1-y0)*.15)
            d->passage_moved=1;
        d->passage_return=0;
        d->auto_phase=blocked?DOOR_AUTO_PASSAGE_BLOCKED:DOOR_AUTO_PASSAGE_RETURN;
        if(blocked||motion>=3||change<d->auto_open_min_l1||background>=change*.5) {
            d->passage_hold=0;return 0;
        }
        if(d->passage_hold<=0)d->passage_hold=now;
        if(now-d->passage_hold<.25)return 0;
        if(d->passage_candidate) {
            if(avg_l1(d->passage_candidate,rgb,w,stride,x0,y0,x1,by)>10) {
                free(d->passage_candidate);d->passage_candidate=NULL;d->passage_cycles=0;
                d->passage_hold=now;return 0;
            }
        } else {
            if(!install_reference(&d->passage_candidate,&d->passage_w,&d->passage_h,d->passage_base,w,h,w*3))return -1;
            /* Keep the clear resting scene outside the panel; never save the passerby. */
            for(int y=y0;y<by;y++)memcpy(d->passage_candidate+((size_t)y*w+x0)*3,rgb+(size_t)y*stride+x0*3,(size_t)(x1-x0)*3);
        }
        d->passage_observed=1;
        return 0;
    }
    d->passage_hold=0;
    d->auto_phase=d->passage_cycles?DOOR_AUTO_PASSAGE_REPEAT:DOOR_AUTO_PASSAGE_WAIT;
    if(!d->passage_inside) {
        /* A changed resting scene is relearned, never immediately called open. */
        if(change>10 || background>10) {
            if(d->passage_quiet<=0)d->passage_quiet=now;
            if(now-d->passage_quiet>=d->auto_quiet_seconds){passage_clear(d);d->auto_phase=DOOR_AUTO_PASSAGE_BASE;}
        } else d->passage_quiet=0;
        return 0;
    }
    d->auto_phase=DOOR_AUTO_PASSAGE_RETURN;
    if(now-d->passage_started>30) {passage_clear(d);d->auto_phase=DOOR_AUTO_PASSAGE_BASE;return 0;}
    if(!d->passage_candidate || !d->passage_observed || !d->passage_moved || change>10 || background>10 || motion>=3) {
        d->passage_return=0;return 0;
    }
    if(d->passage_return<=0)d->passage_return=now;
    d->auto_wait_seconds=now-d->passage_return;
    if(d->auto_wait_seconds<1)return 0;
    d->passage_inside=0;d->passage_return=0;d->passage_cycles++;
    if(d->passage_cycles<2){d->auto_phase=DOOR_AUTO_PASSAGE_REPEAT;return 0;}
    /* Publish only after two complete movement/change/return cycles. */
    if((open_path && raw_rgb_save(open_path,d->passage_candidate,w,h,w*3)) ||
       (closed_path && raw_rgb_save(closed_path,d->passage_base,w,h,w*3))) {
        d->passage_cycles=1;return -1;
    }
    free(d->ref_closed_rgb);free(d->ref_open_rgb);
    d->ref_closed_rgb=d->passage_base;d->passage_base=NULL;
    d->ref_open_rgb=d->passage_candidate;d->passage_candidate=NULL;
    d->ref_closed_w=d->ref_open_w=w;d->ref_closed_h=d->ref_open_h=h;
    invalidate_band(d);d->last_state=-1;d->candidate_state=-1;d->candidate_frames=0;
    d->auto_phase=DOOR_AUTO_DONE;d->auto_wait_seconds=0;
    return 3;
}
