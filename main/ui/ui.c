#include "ui.h"
#include "screens.h"
#include "images.h"
#include "actions.h"
#include "vars.h"

#include <string.h>

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    if (index == -1) {
        return 0;
    }
    return ((lv_obj_t **)&objects)[index];
}

void loadScreen(enum ScreensEnum screenId) {
    currentScreen = screenId - 1;
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    /* A 1080p software-rotated fade redraws the whole screen for every
     * animation step.  Use an immediate swap so navigation stays responsive. */
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

void ui_init() {
    create_screens();
    /* The boot screen is rendered while BL is still off.  Avoid a 200 ms
     * fade from LVGL's blank default screen. */
    currentScreen = SCREEN_ID_AMI - 1;
    lv_scr_load(getLvglObjectFromIndex(currentScreen));

}

void ui_tick() {
    tick_screen(currentScreen);
}
