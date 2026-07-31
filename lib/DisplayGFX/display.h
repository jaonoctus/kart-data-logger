/*
 * DisplayGFX — parallel Arduino_GFX display backend.
 *
 * The original lib/DisplayBSP/display.h exposed low-level esp_lcd panel
 * control. The shared app only includes this header transitively (via
 * uiHelper.h) and does not call any of its symbols, so this is intentionally
 * near-empty. The real API lives in esp_bsp.h.
 */
#pragma once
