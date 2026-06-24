/*
 * DisplayGFX — parallel Arduino_GFX display backend.
 *
 * Minimal lv_port.h providing only the LVGL-port config type the app's
 * bsp_display_cfg_t embeds. Mirrors the EXACT field order of the original
 * lib/DisplayBSP lvgl_port_cfg_t so uiHelper.cpp compiles unchanged.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int task_priority;      /*!< LVGL task priority */
    int task_stack;         /*!< LVGL task stack size */
    int task_affinity;      /*!< LVGL task pinned to core (-1 is no affinity) */
    int task_max_sleep_ms;  /*!< Maximum sleep in LVGL task */
    int timer_period_ms;    /*!< LVGL timer tick period in ms */
} lvgl_port_cfg_t;

#ifdef __cplusplus
}
#endif
