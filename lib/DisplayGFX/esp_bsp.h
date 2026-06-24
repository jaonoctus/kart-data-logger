/*
 * DisplayGFX — parallel Arduino_GFX display backend.
 *
 * BSP-compatible public API. Provides the SAME symbols the app uses against
 * lib/DisplayBSP so the shared sources (uiHelper.cpp, main_display.cpp) build
 * unchanged when DisplayBSP is lib_ignored.
 */
#pragma once

#include "lvgl.h"
#include "lv_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BSP display configuration structure
 *        EXACT field order must match lib/DisplayBSP/esp_bsp.h.
 */
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;  /*!< Configuration for the LVGL port */
    lv_display_rotation_t rotate;   /*!< Rotation configuration for the display */
} bsp_display_cfg_t;

/**
 * @brief Initialize display + touch + LVGL and start the LVGL task.
 * @return Pointer to LVGL display or NULL on error.
 */
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);

/** @brief Turn the backlight on/off (GPIO1, active HIGH). */
void bsp_display_backlight_on(void);
void bsp_display_backlight_off(void);

/**
 * @brief Take/give the recursive LVGL mutex.
 * @param timeout_ms Timeout in ms; 0 blocks indefinitely.
 */
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
