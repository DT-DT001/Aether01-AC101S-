#include "app_ui_modal.h"
#include "app_usb_input.h"

lv_obj_t *app_ui_modal_msgbox(lv_obj_t *owner)
{
    if (!owner) return NULL;
    /* NULL makes LVGL create an auto-owned input-blocking backdrop. Move that
     * backdrop into the page so navigation also destroys it; auto_parent stays
     * intact and the public close API still deletes the entire backdrop. */
    lv_obj_t *box = lv_msgbox_create(NULL);
    if (box) lv_obj_set_parent(lv_obj_get_parent(box), owner);
    return box;
}
bool app_ui_modal_present(lv_obj_t *owner)
{
    if (!owner) return false;
    if (lv_obj_check_type(owner, &lv_msgbox_class)) return true;
    for (uint32_t i=0; i<lv_obj_get_child_count(owner); ++i)
        if (app_ui_modal_present(lv_obj_get_child(owner, (int32_t)i))) return true;
    return false;
}
void app_ui_modal_focus(lv_obj_t *modal, lv_obj_t *preferred)
{
    if (modal) (void)app_usb_input_modal_open(modal, preferred);
}
