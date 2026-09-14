#include "touch_cal_math.h"
#include <assert.h>
#include <stdio.h>

static const touch_cal_point_t target[4] = {{40,40}, {279,40}, {40,199}, {279,199}};

/* Independent simulated sensor: arbitrary axis order, direction, scale and offset. */
static touch_cal_point_t sensor(touch_cal_point_t p, int swap, int mx, int my) {
    float u = mx ? 319-p.x : p.x;
    float v = my ? 239-p.y : p.y;
    if (swap) return (touch_cal_point_t){400 + v*12, 300 + u*10};
    return (touch_cal_point_t){300 + u*10, 400 + v*12};
}

int main(void) {
    touch_cal_mapping_t m;
    touch_cal_point_t raw[4];
    for (int swap = 0; swap < 2; ++swap) {
        for (int mx = 0; mx < 2; ++mx) {
            for (int my = 0; my < 2; ++my) {
                for (int i = 0; i < 4; ++i) raw[i] = sensor(target[i], swap, mx, my);
                assert(touch_cal_fit(raw, target, &m));
                /* Include actual edges, outside the inset calibration targets. */
                const touch_cal_point_t probes[] = {{0,0}, {319,239}, {159,119}, {80,180}};
                for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i) {
                    touch_cal_point_t p = touch_cal_map(&m, sensor(probes[i], swap, mx, my));
                    assert(fabsf(p.x - probes[i].x) < 0.01f);
                    assert(fabsf(p.y - probes[i].y) < 0.01f);
                }
            }
        }
    }
    /* Cross-axis skew requires the full affine mapping, not independent min/max bounds. */
    for (int i = 0; i < 4; ++i) {
        raw[i] = (touch_cal_point_t){300 + 8*target[i].x + 2*target[i].y,
                                     400 + target[i].x + 12*target[i].y};
    }
    assert(touch_cal_fit(raw, target, &m));
    touch_cal_point_t skewed = touch_cal_map(&m, (touch_cal_point_t){300+8*100+2*150, 400+100+12*150});
    assert(fabsf(skewed.x - 100) < 0.01f && fabsf(skewed.y - 150) < 0.01f);
    /* A little sampling noise should remain within a pixel at the center. */
    for (int i = 0; i < 4; ++i) {
        raw[i] = sensor(target[i], 1, 1, 0);
        raw[i].x += (i & 1) ? 4 : -4;
        raw[i].y += (i & 2) ? -3 : 3;
    }
    assert(touch_cal_fit(raw, target, &m));
    touch_cal_point_t center = touch_cal_map(&m, sensor((touch_cal_point_t){159,119}, 1, 1, 0));
    assert(fabsf(center.x - 159) < 1 && fabsf(center.y - 119) < 1);
    /* Repeated corners, collinear points and an incorrect fourth tap must fail. */
    for (int i = 0; i < 4; ++i) raw[i] = (touch_cal_point_t){500, 500};
    assert(!touch_cal_fit(raw, target, &m));
    for (int i = 0; i < 4; ++i) raw[i] = (touch_cal_point_t){500+i*500, 600+i*500};
    assert(!touch_cal_fit(raw, target, &m));
    for (int i = 0; i < 4; ++i) raw[i] = sensor(target[i], 0, 0, 0);
    raw[3] = raw[0];
    assert(!touch_cal_fit(raw, target, &m));
    raw[0].x = NAN;
    assert(!touch_cal_fit(raw, target, &m));
    m = (touch_cal_mapping_t){0};
    assert(!touch_cal_mapping_valid(&m));
    puts("PASS: all 8 orientations, inset targets, edge extrapolation, noise and invalid samples");
    return 0;
}
