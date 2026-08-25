#ifndef TRACKER_H
#define TRACKER_H

#include "yolo11.h"

#include <stddef.h>
#include <stdint.h>

typedef struct LightTracker LightTracker;

typedef struct {
    int downsample;
    int search_radius;
    int patch_radius;
    int motion_threshold;
} TrackerOptions;

LightTracker *tracker_create(const TrackerOptions *options);
void tracker_destroy(LightTracker *tracker);

/* A full detector result becomes the reference for the following frames. */
int tracker_reset(LightTracker *tracker, const uint8_t *rgb, int width,
                  int height, int stride, char *error, size_t error_size);

/* Updates boxes in place and asks for an early detector refresh when unsure. */
int tracker_update(LightTracker *tracker, const uint8_t *rgb, int width,
                   int height, int stride, DetectionList *detections,
                   int *request_detection, char *error, size_t error_size);

#endif
