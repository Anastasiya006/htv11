#pragma once

#include "lvgl.h"

#define GEMMI_MAX_CHOICES 3
#define GEMMI_CHOICE_LEN 33

/* One bridge event as the firmware sees it. */
typedef struct {
    int id;
    char kind[12];
    char agent[33];
    char state[33];
    char artifact[201];
    char choices[GEMMI_MAX_CHOICES][GEMMI_CHOICE_LEN];
    int choice_count;
    char message[161];
} gemmi_event_t;

/* The small, device-facing Gemmi interface. */
void gemmi_ui_init(lv_display_t *display);
void gemmi_ui_show_event(const gemmi_event_t *event);
void gemmi_ui_show_image(uint8_t *data, size_t len);

/* Show or clear the "offline" status line when the bridge is unreachable. */
void gemmi_ui_offline(bool offline);

/* Open the pet room unless an agent question needs an answer. */
void gemmi_ui_open_pet(void);
