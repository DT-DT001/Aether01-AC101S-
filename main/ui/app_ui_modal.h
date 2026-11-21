#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* Page-owned LVGL modal backdrop; lv_msgbox_close releases both automatically. */
lv_obj_t *app_ui_modal_msgbox(lv_obj_t *owner);
bool app_ui_modal_present(lv_obj_t *owner);
/* Call after controls exist. Keypad focus cannot pass through the backdrop. */
void app_ui_modal_focus(lv_obj_t *modal, lv_obj_t *preferred);
