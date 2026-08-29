#include "tracker.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_TRACK_POINTS = 9 };

struct LightTracker {
    TrackerOptions options;
    uint8_t *previous;
    uint8_t *current;
    uint8_t *mask;
    int source_width;
    int source_height;
    int gray_width;
    int gray_height;
    int ready;
};

static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (!error || error_size == 0) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static int clampi(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int ensure_buffers(LightTracker *tracker, int width, int height,
                          char *error, size_t error_size) {
    int gray_width = (width + tracker->options.downsample - 1) /
                     tracker->options.downsample;
    int gray_height = (height + tracker->options.downsample - 1) /
                      tracker->options.downsample;
    size_t count = (size_t)gray_width * (size_t)gray_height;
    uint8_t *previous;
    uint8_t *current;
    uint8_t *mask;

    if (tracker->previous && width == tracker->source_width &&
        height == tracker->source_height) return 0;
    previous = (uint8_t *)malloc(count);
    current = (uint8_t *)malloc(count);
    mask = (uint8_t *)malloc(count);
    if (!previous || !current || !mask) {
        free(previous);
        free(current);
        free(mask);
        set_error(error, error_size, "out of memory creating tracker buffers");
        return -1;
    }
    free(tracker->previous);
    free(tracker->current);
    free(tracker->mask);
    tracker->previous = previous;
    tracker->current = current;
    tracker->mask = mask;
    tracker->source_width = width;
    tracker->source_height = height;
    tracker->gray_width = gray_width;
    tracker->gray_height = gray_height;
    tracker->ready = 0;
    return 0;
}

static void make_gray(const LightTracker *tracker, const uint8_t *rgb,
                      int stride, uint8_t *gray) {
    int step = tracker->options.downsample;
    for (int y = 0; y < tracker->gray_height; ++y) {
        int source_y = clampi(y * step + step / 2, 0,
                              tracker->source_height - 1);
        for (int x = 0; x < tracker->gray_width; ++x) {
            int source_x = clampi(x * step + step / 2, 0,
                                  tracker->source_width - 1);
            const uint8_t *pixel = rgb + source_y * stride + source_x * 3;
            gray[y * tracker->gray_width + x] =
                (uint8_t)((77u * pixel[0] + 150u * pixel[1] +
                           29u * pixel[2] + 128u) >> 8);
        }
    }
}

LightTracker *tracker_create(const TrackerOptions *options) {
    LightTracker *tracker;
    if (!options || options->downsample < 1 || options->search_radius < 1 ||
        options->patch_radius < 1 || options->motion_threshold < 1) return NULL;
    tracker = (LightTracker *)calloc(1, sizeof(*tracker));
    if (!tracker) return NULL;
    tracker->options = *options;
    return tracker;
}

void tracker_destroy(LightTracker *tracker) {
    if (!tracker) return;
    free(tracker->previous);
    free(tracker->current);
    free(tracker->mask);
    free(tracker);
}

int tracker_reset(LightTracker *tracker, const uint8_t *rgb, int width,
                  int height, int stride, char *error, size_t error_size) {
    if (!tracker || !rgb || width <= 0 || height <= 0 || stride < width * 3) {
        set_error(error, error_size, "invalid tracker_reset arguments");
        return -1;
    }
    if (ensure_buffers(tracker, width, height, error, error_size) != 0)
        return -1;
    make_gray(tracker, rgb, stride, tracker->previous);
    tracker->ready = 1;
    return 0;
}

static unsigned patch_sad(const LightTracker *tracker, int px, int py,
                          int cx, int cy) {
    unsigned sad = 0;
    int radius = tracker->options.patch_radius;
    for (int y = -radius; y <= radius; ++y) {
        const uint8_t *a = tracker->previous +
                           (py + y) * tracker->gray_width + px - radius;
        const uint8_t *b = tracker->current +
                           (cy + y) * tracker->gray_width + cx - radius;
        for (int x = 0; x <= radius * 2; ++x) {
            int difference = (int)a[x] - (int)b[x];
            sad += (unsigned)(difference < 0 ? -difference : difference);
        }
    }
    return sad;
}

static int point_gradient(const LightTracker *tracker, int x, int y) {
    const uint8_t *gray = tracker->previous;
    int width = tracker->gray_width;
    int gx = (int)gray[y * width + x + 1] -
             (int)gray[y * width + x - 1];
    int gy = (int)gray[(y + 1) * width + x] -
             (int)gray[(y - 1) * width + x];
    return (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
}

static int select_points(const LightTracker *tracker, const Detection *box,
                         int points_x[MAX_TRACK_POINTS],
                         int points_y[MAX_TRACK_POINTS]) {
    int step = tracker->options.downsample;
    int margin = tracker->options.patch_radius + tracker->options.search_radius + 1;
    int left = clampi((int)(box->x1 / (float)step), margin,
                      tracker->gray_width - margin - 1);
    int top = clampi((int)(box->y1 / (float)step), margin,
                     tracker->gray_height - margin - 1);
    int right = clampi((int)(box->x2 / (float)step), margin,
                       tracker->gray_width - margin - 1);
    int bottom = clampi((int)(box->y2 / (float)step), margin,
                        tracker->gray_height - margin - 1);
    int count = 0;

    if (tracker->gray_width <= margin * 2 + 1 ||
        tracker->gray_height <= margin * 2 + 1) return 0;
    if (right - left < 6 || bottom - top < 6) return 0;
    for (int grid_y = 0; grid_y < 3; ++grid_y) {
        int cell_top = top + (bottom - top) * grid_y / 3;
        int cell_bottom = top + (bottom - top) * (grid_y + 1) / 3;
        for (int grid_x = 0; grid_x < 3; ++grid_x) {
            int cell_left = left + (right - left) * grid_x / 3;
            int cell_right = left + (right - left) * (grid_x + 1) / 3;
            int best_gradient = 0;
            int best_x = -1;
            int best_y = -1;
            for (int y = cell_top; y <= cell_bottom; y += 2) {
                for (int x = cell_left; x <= cell_right; x += 2) {
                    int gradient = point_gradient(tracker, x, y);
                    if (gradient > best_gradient) {
                        best_gradient = gradient;
                        best_x = x;
                        best_y = y;
                    }
                }
            }
            if (best_gradient >= 12 && best_x >= 0) {
                points_x[count] = best_x;
                points_y[count] = best_y;
                count++;
            }
        }
    }
    return count;
}

static void sort_small(int *values, int count) {
    for (int i = 1; i < count; ++i) {
        int value = values[i];
        int j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static int track_box(const LightTracker *tracker, Detection *box) {
    int points_x[MAX_TRACK_POINTS];
    int points_y[MAX_TRACK_POINTS];
    int movements_x[MAX_TRACK_POINTS];
    int movements_y[MAX_TRACK_POINTS];
    int sorted_x[MAX_TRACK_POINTS];
    int sorted_y[MAX_TRACK_POINTS];
    int point_count = select_points(tracker, box, points_x, points_y);
    int valid = 0;
    int radius = tracker->options.patch_radius;
    int search = tracker->options.search_radius;
    unsigned patch_pixels = (unsigned)((radius * 2 + 1) * (radius * 2 + 1));

    for (int i = 0; i < point_count; ++i) {
        unsigned best_sad = ~(unsigned)0;
        int best_dx = 0;
        int best_dy = 0;
        for (int dy = -search; dy <= search; ++dy) {
            int cy = points_y[i] + dy;
            if (cy - radius < 0 || cy + radius >= tracker->gray_height)
                continue;
            for (int dx = -search; dx <= search; ++dx) {
                int cx = points_x[i] + dx;
                unsigned sad;
                if (cx - radius < 0 || cx + radius >= tracker->gray_width)
                    continue;
                sad = patch_sad(tracker, points_x[i], points_y[i], cx, cy);
                if (sad < best_sad ||
                    (sad == best_sad && abs(dx) + abs(dy) <
                                            abs(best_dx) + abs(best_dy))) {
                    best_sad = sad;
                    best_dx = dx;
                    best_dy = dy;
                }
            }
        }
        if (best_sad / patch_pixels <= 40u) {
            movements_x[valid] = best_dx;
            movements_y[valid] = best_dy;
            valid++;
        }
    }
    if (valid < 3) return 0;
    memcpy(sorted_x, movements_x, (size_t)valid * sizeof(sorted_x[0]));
    memcpy(sorted_y, movements_y, (size_t)valid * sizeof(sorted_y[0]));
    sort_small(sorted_x, valid);
    sort_small(sorted_y, valid);
    {
        int median_x = sorted_x[valid / 2];
        int median_y = sorted_y[valid / 2];
        int agreement = 0;
        int step = tracker->options.downsample;
        for (int i = 0; i < valid; ++i) {
            if (abs(movements_x[i] - median_x) <= 1 &&
                abs(movements_y[i] - median_y) <= 1) agreement++;
        }
        if (agreement * 2 < valid) return 0;
        float dx_pixels = (float)(median_x * step);
        float dy_pixels = (float)(median_y * step);
        box->x1 += dx_pixels;
        box->x2 += dx_pixels;
        box->y1 += dy_pixels;
        box->y2 += dy_pixels;
        if (box->x1 < 0.0f) {
            box->x2 -= box->x1;
            box->x1 = 0.0f;
        }
        if (box->y1 < 0.0f) {
            box->y2 -= box->y1;
            box->y1 = 0.0f;
        }
        if (box->x2 >= (float)tracker->source_width) {
            float excess = box->x2 - (float)(tracker->source_width - 1);
            box->x1 -= excess;
            box->x2 -= excess;
        }
        if (box->y2 >= (float)tracker->source_height) {
            float excess = box->y2 - (float)(tracker->source_height - 1);
            box->y1 -= excess;
            box->y2 -= excess;
        }
        /* keypoint 도 박스와 같은 델타로 평행이동합니다.
         * 박스와 달리 경계로 clamp 하지 않습니다. */
        if (box->keypoint_count > 0) {
            int k;
            for (k = 0; k < box->keypoint_count; ++k) {
                box->kp[k].x += dx_pixels;
                box->kp[k].y += dy_pixels;
            }
        }
    }
    return 1;
}

static void mark_detection_mask(LightTracker *tracker,
                                const DetectionList *detections) {
    size_t count = (size_t)tracker->gray_width * (size_t)tracker->gray_height;
    int step = tracker->options.downsample;
    memset(tracker->mask, 0, count);
    for (size_t i = 0; i < detections->count; ++i) {
        const Detection *box = &detections->items[i];
        int left = clampi((int)(box->x1 / (float)step) - 2,
                          0, tracker->gray_width - 1);
        int top = clampi((int)(box->y1 / (float)step) - 2,
                         0, tracker->gray_height - 1);
        int right = clampi((int)(box->x2 / (float)step) + 2,
                           0, tracker->gray_width - 1);
        int bottom = clampi((int)(box->y2 / (float)step) + 2,
                            0, tracker->gray_height - 1);
        for (int y = top; y <= bottom; ++y)
            memset(tracker->mask + y * tracker->gray_width + left, 1,
                   (size_t)(right - left + 1));
    }
}

static int unexplained_motion(LightTracker *tracker,
                              const DetectionList *detections) {
    int changed_total = 0;
    int pixel_total = tracker->gray_width * tracker->gray_height;
    const int tile = 8;
    mark_detection_mask(tracker, detections);
    for (int tile_y = 0; tile_y < tracker->gray_height; tile_y += tile) {
        for (int tile_x = 0; tile_x < tracker->gray_width; tile_x += tile) {
            int changed_outside = 0;
            int tile_pixels = 0;
            int y_end = clampi(tile_y + tile, 0, tracker->gray_height);
            int x_end = clampi(tile_x + tile, 0, tracker->gray_width);
            for (int y = tile_y; y < y_end; ++y) {
                for (int x = tile_x; x < x_end; ++x) {
                    int index = y * tracker->gray_width + x;
                    int difference = (int)tracker->previous[index] -
                                     (int)tracker->current[index];
                    if (difference < 0) difference = -difference;
                    if (difference >= tracker->options.motion_threshold) {
                        changed_total++;
                        if (!tracker->mask[index]) changed_outside++;
                    }
                    tile_pixels++;
                }
            }
            if (tile_pixels > 0 && changed_outside * 4 >= tile_pixels)
                return 1;
        }
    }
    return changed_total * 3 >= pixel_total;
}

int tracker_update(LightTracker *tracker, const uint8_t *rgb, int width,
                   int height, int stride, DetectionList *detections,
                   int *request_detection, char *error, size_t error_size) {
    uint8_t *swap;
    int uncertain = 0;
    if (!tracker || !rgb || !detections || !request_detection || width <= 0 ||
        height <= 0 || stride < width * 3) {
        set_error(error, error_size, "invalid tracker_update arguments");
        return -1;
    }
    if (ensure_buffers(tracker, width, height, error, error_size) != 0)
        return -1;
    if (!tracker->ready)
        return tracker_reset(tracker, rgb, width, height, stride,
                             error, error_size);

    make_gray(tracker, rgb, stride, tracker->current);
    if (unexplained_motion(tracker, detections)) uncertain = 1;
    for (size_t i = 0; i < detections->count; ++i) {
        if (!track_box(tracker, &detections->items[i])) uncertain = 1;
    }
    swap = tracker->previous;
    tracker->previous = tracker->current;
    tracker->current = swap;
    *request_detection = uncertain;
    return 0;
}
