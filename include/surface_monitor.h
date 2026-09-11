#ifndef SURFACE_MONITOR_H
#define SURFACE_MONITOR_H
#include <stddef.h>
#include <stdint.h>
#include "log.h"
#define SURFACE_MAX 16
#define SURFACE_POINTS 16
#define SURFACE_ZONES 8
#define SURFACE_FIXTURES 8
#define SURFACE_JSON_MAX 65536
#define SURFACE_GRID 96
typedef struct { float x, y; } SurfacePoint;
typedef struct { int count; SurfacePoint points[SURFACE_POINTS]; } SurfacePolygon;
typedef struct {
    char id[40];
    int movable;
    SurfacePolygon polygon, allowed;
} SurfaceFixture;
typedef struct {
    char id[40];
    int type; /* 0=floor, 1=table, 2=wall */
    int locked;
    SurfacePolygon polygon;
    int usage_count, exclusion_count, fixture_count;
    SurfacePolygon usage[SURFACE_ZONES], exclusions[SURFACE_ZONES];
    SurfaceFixture fixtures[SURFACE_FIXTURES];
    double enter_seconds, departure_seconds, confirm_seconds, clear_seconds;
    float threshold, min_area;
} SurfaceDefinition;
typedef struct {
    int enabled, revision, width, height, geometry_revision, count, automatic;
    int ai_reuse; /* default enabled; false restores periodic candidate scheduling */
    char camera_id[40];
    SurfaceDefinition surfaces[SURFACE_MAX];
} SurfaceConfig;
/* Config is value-owned. Parser borrows JSON for this call only. No pointers survive. */
int surface_config_parse(const char *json, SurfaceConfig *out, char *error, size_t size);
int surface_config_read(const char *path, SurfaceConfig *out, char *error, size_t size);
int surface_config_transition(const SurfaceConfig *old, const SurfaceConfig *next,
                              char *error, size_t size);
int surface_polygon_contains(const SurfacePolygon *polygon, float x, float y);
/* Atomic replacement, including Windows; caller owns input until return. */
int surface_file_replace(const char *path, const void *data, size_t length);

typedef struct {
    float x1, y1, x2, y2, anchor_x, anchor_y;
    int id, kind; /* 0=person, 1=cat/dog, 2=other; normalized camera coordinates */
} SurfaceObject;
typedef struct {
    const uint8_t *rgb; /* borrowed until update/capture returns; never retained */
    int width, height, stride;
    double now, people_at;
    int camera_ok, people_valid;
    const SurfaceObject *people;
    size_t people_count;
} SurfaceFrame;
typedef struct {
    unsigned long serial;
    int config_revision, surface_index, candidate_index, candidate_version;
    float x1,y1,x2,y2;
    double observed_at;
    int appearance_valid, occupied;
    unsigned char appearance[8*8*3];
} SurfaceInspection;
typedef struct SurfaceMonitor SurfaceMonitor;
/* Monitor owns bounded buffers; release with surface_monitor_destroy. */
SurfaceMonitor *surface_monitor_create(const char *directory);
void surface_monitor_destroy(SurfaceMonitor *monitor);
int surface_monitor_apply(SurfaceMonitor *monitor, const SurfaceConfig *config,
                          char *error, size_t size);
int surface_monitor_enabled(const SurfaceMonitor *monitor);
void surface_monitor_update(SurfaceMonitor *monitor, const SurfaceFrame *frame, EventLog *log);
/* Capture is explicitly requested, accumulated on fresh unobscured frames. */
int surface_monitor_capture(SurfaceMonitor *monitor, const char *id, int empty,
                            char *error, size_t size);
int surface_monitor_poll_request(SurfaceMonitor *monitor, double now, SurfaceInspection *out);
void surface_monitor_submit_result(SurfaceMonitor *monitor, const SurfaceInspection *request,
                                   const SurfaceObject *objects, size_t count, int success,
                                   double now, EventLog *log);
void surface_monitor_status(const SurfaceMonitor *monitor, double now, char *json, size_t size);
int surface_monitor_acknowledge(SurfaceMonitor *monitor, const char *id, int candidate,
                                int revision, int candidate_version, EventLog *log);
#endif
