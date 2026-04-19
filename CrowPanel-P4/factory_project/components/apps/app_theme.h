#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_THEME_CALPOLY = 0,
    APP_THEME_TOPOGRAPHY,
    APP_THEME_VISTA,
    APP_THEME_MAX,
} app_theme_id_t;

int32_t app_get_ui_theme_id(void);
void app_set_ui_theme_id(int32_t theme_id);

#ifdef __cplusplus
}
#endif
