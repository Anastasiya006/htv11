#pragma once

/* LVGL display lifecycle and the lock shared with background bridge tasks. */
void gemmi_display_init(void);
void gemmi_display_lock(void);
void gemmi_display_unlock(void);
