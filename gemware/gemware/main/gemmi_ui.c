/*
 * Gemmi's tiny agent companion UI.
 *
 * The character is the only continuously animated region.  The three bottom
 * controls never move, so a human can build muscle memory even while an
 * agent changes the scene.  During an ask those same three slots become the
 * question's real choices: first tap selects, second tap sends.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "gemmi_anims.h"
#include "gemmi_bridge.h"
#include "gemmi_ui.h"
#include "gemmi_pet.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#ifdef GEMMI_SYSTEM_MENLO
#include "gemmi_ui_scale.h"
#endif

#define SCREEN_W 240
#define SCREEN_H 320
#define TOP_H 24
#define DOCK_H 38
#define BUBBLE_X 12
#define BUBBLE_Y 206
#define BUBBLE_W (SCREEN_W - 24)
#define BUBBLE_H 70
#define MAX_BUBBLE_TEXT 160
#define AGENT_FEED_LEN 8

/* Match the simulator's small console type without requiring a new font on
 * an already-configured firmware build. New builds enable these in defaults. */
#if defined(GEMMI_SYSTEM_MENLO)
extern const lv_font_t gemmi_menlo_8;
extern const lv_font_t gemmi_menlo_10;
extern const lv_font_t gemmi_menlo_12;
#define UI_MONO (&gemmi_menlo_10)
#define UI_BODY (&gemmi_menlo_12)
#else
#if LV_FONT_UNSCII_8
#define UI_MONO (&lv_font_unscii_8)
#else
#define UI_MONO LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_12
#define UI_BODY (&lv_font_montserrat_12)
#else
#define UI_BODY LV_FONT_DEFAULT
#endif
#endif

typedef enum {
    GEMMI_ACTION_HOME = 1,
    GEMMI_ACTION_INBOX,
    GEMMI_ACTION_AGENTS,
    GEMMI_ACTION_ASKS,
    GEMMI_ACTION_VIEW,
} gemmi_action_t;

#define AGENT_LOG_LEN 5
#define AGENT_LOG_TEXT 41

typedef struct {
    char agent[33];
    char state[33];
    char log[AGENT_LOG_LEN][AGENT_LOG_TEXT];
    int log_head;
    int log_count;
} gemmi_agent_state_t;

static lv_obj_t *gif;
static lv_obj_t *scene_obj;
static lv_obj_t *bubble_obj;
static lv_timer_t *idle_timer;
static lv_obj_t *headline_label;
static lv_obj_t *message_label;
static lv_obj_t *queue_label;
static lv_obj_t *status_label;
static lv_obj_t *console_title;
static lv_obj_t *log_title;
static lv_obj_t *list_title;
static lv_obj_t *agent_row_states[4];
static lv_obj_t *dock_buttons[3];
static lv_obj_t *dock_labels[3];

static lv_obj_t *pet_badge;
static lv_obj_t *ask_back;

static lv_obj_t *shop_panel, *shop_feedback, *shop_cards[4], *shop_prices[4];
static lv_obj_t *store_button;
static lv_obj_t *shop_previews[4], *shop_names[4], *shop_title;
static unsigned shop_page;
static const int shop_catalog[8] = {0, 1, 2, 7, 3, 4, 5, 6};
static void draw_shop_preview(lv_obj_t *preview, unsigned item);
static lv_obj_t *pet_glasses, *pet_crown, *pet_bow;
static lv_obj_t *pet_halo, *pet_antenna, *pet_scarf, *pet_horns;
static lv_obj_t *jpeg_view;
static lv_image_dsc_t jpeg_dsc;
static uint8_t *jpeg_buf;
static int pet_mode; /* compact shop overlay open */
static void on_pet(lv_event_t *event);
static void on_rest(lv_event_t *event);
static void refresh_pet_badge(void);
static void refresh_pet_look(void);
static void render_pet_room(const char *feedback);
static uint32_t current_animation = GEMMI_ANIM_IDLE;

static gemmi_event_t pending_ask;
static bool ask_pending;
static bool ask_foreground;
static int selected_choice = -1;
static gemmi_event_t last_artifact;
static bool have_artifact;
static gemmi_agent_state_t agent_feed[AGENT_FEED_LEN];
static int agent_feed_count;
static lv_obj_t *log_panel;
static lv_obj_t *log_label;
static lv_obj_t *musing_label;
#define AGENT_ROWS 4

static lv_obj_t *agent_rows[AGENT_ROWS];
static char last_musing[161];
/* 0 = nav, 1 = agent list, 2 = one agent's log */
static int agents_mode;
static int detail_agent = -1;
static int agents_page;
static void stop_monitor(void);

static uint32_t animation_for_state(const char *state)
{
    for (uint32_t index = 0; index < gemmi_anims_count; index++) {
        if (strcmp(gemmi_anims[index].name, state) == 0) return index;
    }
    return GEMMI_ANIM_IDLE;
}

static void trim_for_bubble(const char *source, char *destination, size_t destination_size)
{
    size_t length = strlen(source);
    if (length < MAX_BUBBLE_TEXT) {
        strlcpy(destination, source, destination_size);
        return;
    }

    size_t copy_length = MAX_BUBBLE_TEXT - 3;
    if (copy_length >= destination_size) copy_length = destination_size - 1;
    /* Do not cut inside a UTF-8 continuation byte or the panel shows glyphs. */
    while (copy_length > 0 && (source[copy_length] & 0xC0) == 0x80) {
        copy_length--;
    }
    memcpy(destination, source, copy_length);
    strlcpy(destination + copy_length, "...", destination_size - copy_length);
}

/* The display itself is rotated in gemmi_display.c. Keep every LVGL object
 * untransformed: LVGL's software ARGB transform path is unsafe on this ESP32
 * while GIFs or buttons are being redrawn. */
static void make_label_upright(lv_obj_t *label, int16_t width, int16_t height)
{
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

#define SCENE_W 240
#define SCENE_H 178
#define GIF_SIZE 156

/* Sprite placement: 0 = big centred, 1 = corner badge, 2 = detail left. */
static int sprite_mode;
static const char *TAG = "gemmi_ui";

static void apply_sprite_mode(void)
{
    if (gif == NULL) return;
    /* Frames are native 64 px; scale factor is target * 256 / 64 = target*4. */
    if (sprite_mode == 1) {
        lv_obj_set_size(gif, 44, 44);
        lv_image_set_scale(gif, 44 * 4);
        lv_obj_align(gif, LV_ALIGN_TOP_RIGHT, -16, 4);
    } else if (sprite_mode == 2) {
        lv_obj_set_size(gif, 96, 96);
        lv_image_set_scale(gif, 96 * 4);
        lv_obj_align(gif, LV_ALIGN_TOP_LEFT, 12, 4);
    } else if (sprite_mode == 3) {
        lv_obj_set_size(gif, 96, 96);
        lv_image_set_scale(gif, 96 * 4);
        lv_obj_align(gif, LV_ALIGN_TOP_MID, 0, 10);
    } else {
        lv_obj_set_size(gif, GIF_SIZE, GIF_SIZE);
        lv_image_set_scale(gif, GIF_SIZE * 4);
        lv_obj_center(gif);
    }
    refresh_pet_look();
}

/* lv_gif_set_src frees the old frame buffer before allocating the new one,
 * so the freed slot usually satisfies the alloc even on a fragmented heap.
 * The failure mode to guard: alloc fails and the widget is left with a NULL
 * draw_buf plus a live frame timer, which crashes on the next tick. This
 * runs inside the LVGL task, so the timer cannot fire mid-check; rebuild
 * the widget (fresh paused timer) whenever init lost the decoder. */
static void swap_gif(const void *dsc)
{
    /* Never reuse the decoder: a failed re-init leaves the widget with a NULL
     * draw buffer and a LIVE frame timer — the next tick panics. Rebuild the
     * widget instead; a failed init keeps its timer paused (no crash, just a
     * missing sprite until the next swap). */
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 20 * 1024) {
        ESP_LOGW(TAG, "low contiguous heap; keeping animation");
        return;
    }
    lv_obj_t *parent = lv_obj_get_parent(gif);
    lv_obj_delete(gif);
    gif = lv_gif_create(parent);
    if (gif == NULL) {
        ESP_LOGW(TAG, "gif alloc failed, free heap %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_add_flag(gif, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gif, on_pet, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(gif, on_rest, LV_EVENT_LONG_PRESSED, NULL);
    lv_gif_set_color_format(gif, LV_COLOR_FORMAT_ARGB8888);
    lv_gif_set_src(gif, dsc);
    if (lv_image_get_src(gif) == NULL) {
        ESP_LOGW(TAG, "gif init failed, free heap %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }
    lv_image_set_antialias(gif, false);
    apply_sprite_mode();
}

static void back_to_idle(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    idle_timer = NULL;
    if (current_animation != GEMMI_ANIM_IDLE) {
        swap_gif(gemmi_anims[GEMMI_ANIM_IDLE].dsc);
        current_animation = GEMMI_ANIM_IDLE;
    }
}

static int64_t last_swap_us;

/* Defer the swap to the LVGL timer loop instead of running it inside an
 * input-event handler. */
static void swap_animation(void *dsc)
{
    swap_gif(dsc);
}

static void play_state(const char *state, bool return_to_idle)
{
    uint32_t index = animation_for_state(state);
    /* Keep GIFs native-size, never reload an unchanged source, and coalesce
     * bursts to one swap per 300 ms. */
    if (index != current_animation) {
        int64_t now = esp_timer_get_time();
        if (now - last_swap_us >= 300000) {
            current_animation = index;
            last_swap_us = now;
            lv_async_call(swap_animation, (void *)gemmi_anims[index].dsc);
        }
    }

    if (idle_timer) {
        lv_timer_delete(idle_timer);
        idle_timer = NULL;
    }
    if (return_to_idle && index != GEMMI_ANIM_IDLE) {
        idle_timer = lv_timer_create(back_to_idle, gemmi_anims[index].total_ms, NULL);
        lv_timer_set_repeat_count(idle_timer, 1);
    }
}

static void level_up_celebration(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (!ask_foreground && !ask_pending) play_state("jump", true);
}

static void set_bubble(const char *headline, const char *message, const char *queue)
{
    char compact_message[MAX_BUBBLE_TEXT + 1];
    trim_for_bubble(message, compact_message, sizeof(compact_message));
    lv_label_set_text(headline_label, headline);
    lv_label_set_text(console_title, "Gemmi");
    lv_label_set_text(message_label, compact_message);
    lv_label_set_text(queue_label, queue);
}

/* Titles are siblings of the frame so the renderer never clips their
 * upper pixels at the frame edge. Keep their visibility coupled. */
static void show_console(bool visible)
{
    if (visible) {
        lv_obj_remove_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(console_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(console_title, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_log(bool visible)
{
    if (visible) {
        lv_obj_remove_flag(log_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(log_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(log_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(log_title, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_status(const char *status)
{
    /* Always map verbose internal states to short top-bar captions — the raw
     * strings like "watching codex / working" overflow the 90 px label. */
    const char *action = status;
    if (!strncmp(status, "watching ", 9)) {
        const char *separator = strstr(status, " / ");
        if (separator) action = separator + 3;
    } else if (!strcmp(status, "gemmi is thinking")) action = "Thinking";
    else if (!strncmp(status, "answer ", 7) || !strcmp(status, "gemmi needs an answer")) action = "Needs input";
    else if (!strncmp(status, "again to send:", 14)) action = "Confirm choice";
    else if (!strncmp(status, "gemmi sent:", 11)) action = "Answered";
    else if (!strncmp(status, "gemmi is viewing", 16)) action = "Viewing image";
    else if (!strncmp(status, "Agents /", 8)) action = "Agents";
    else if (!strncmp(status, "offline", 7)) action = "Disconnected";
    else if (!strcmp(status, "gemmi is back online")) action = "Connected";
    else if (!strcmp(status, "gemmi checked - no asks")) action = "No pending asks";
    else if (!strcmp(status, "gemmi found no artifact")) action = "No image yet";
    if (!strcmp(action, "needs-attention")) action = "Needs input";
    char caption[64];
    snprintf(caption, sizeof(caption), "%s", action);
    if (caption[0] >= 'a' && caption[0] <= 'z') caption[0] -= 'a' - 'A';
    lv_label_set_text(status_label, caption);
}

static void set_dock_caption(uint32_t slot, const char *caption)
{
    if (slot >= 3) return;
    char display_caption[64];
    snprintf(display_caption, sizeof(display_caption), "%s", caption);
    if (display_caption[0] >= 'a' && display_caption[0] <= 'z') display_caption[0] -= 'a' - 'A';
    lv_label_set_text(dock_labels[slot], display_caption);
    lv_obj_set_size(dock_labels[slot], 54, 14);
    lv_obj_center(dock_labels[slot]);
    /* An empty caption would leave a dead button on screen. */
    if (caption[0] == '\0') {
        lv_obj_add_flag(dock_buttons[slot], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(dock_buttons[slot], LV_OBJ_FLAG_HIDDEN);
    }
}

static void select_choice(int slot)
{
    selected_choice = slot;
    for (uint32_t index = 0; index < 3; index++) {
        /* States on the button do not propagate to its label child, so the
         * caption colour is driven by hand to match the inverted fill. */
        if ((int)index == slot) {
            lv_obj_add_state(dock_buttons[index], LV_STATE_CHECKED);
            lv_obj_set_style_text_color(dock_labels[index], lv_color_hex(0x444444), 0);
        } else {
            lv_obj_remove_state(dock_buttons[index], LV_STATE_CHECKED);
            lv_obj_set_style_text_color(dock_labels[index], lv_color_hex(0x111111), 0);
        }
    }
}

/* Show a pre-rendered RGB565 artifact in the scene. Takes ownership of
 * `data` — freed when the next image arrives. Layout: w,h,pixels — no
 * decoder needed, the bridge pre-renders on the host. */
void gemmi_ui_show_image(uint8_t *data, size_t len)
{
    if (!jpeg_view) {
        jpeg_view = lv_image_create(scene_obj);
        lv_obj_align(jpeg_view, LV_ALIGN_CENTER, 0, 0);
    }
    /* `data` is the static bridge buffer — never freed; a new frame just
     * overwrites it. jpeg_buf marks "an image is loaded" for `view`. */
    if (len < 4) return;
    unsigned w = data[0] | (data[1] << 8);
    unsigned h = data[2] | (data[3] << 8);
    /* A legacy JPEG payload (FF D8 magic) or a bogus header must never reach
     * lv_image — a giant frame buffer overflows straight into heap lists. */
    if (w == 0 || w > 96 || h == 0 || h > 72 || (size_t)w * h * 2 + 4 > len) {
        ESP_LOGW(TAG, "bad image header w=%u h=%u len=%u", w, h, (unsigned)len);
        return;
    }
    jpeg_buf = data;
    jpeg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    jpeg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    jpeg_dsc.header.w = w;
    jpeg_dsc.header.h = h;
    jpeg_dsc.header.stride = w * 2;
    jpeg_dsc.header.flags = 0;
    jpeg_dsc.data = data + 4;
    jpeg_dsc.data_size = len - 4;
    lv_image_set_src(jpeg_view, &jpeg_dsc);
    lv_image_set_scale(jpeg_view, 440); /* ~1.7x zoom, no extra buffer */
    /* Buffer only — `view` reveals it. */
    lv_obj_add_flag(jpeg_view, LV_OBJ_FLAG_HIDDEN);
}

static void hide_jpeg(void)
{
    if (jpeg_view) lv_obj_add_flag(jpeg_view, LV_OBJ_FLAG_HIDDEN);
}

static void show_nav_dock(void)
{
    select_choice(-1);
    set_dock_caption(0, "agents");
    set_dock_caption(1, "asks");
    set_dock_caption(2, "view");
    if (store_button) lv_obj_remove_flag(store_button, LV_OBJ_FLAG_HIDDEN);
}

static void set_sprite_small(void)
{
    /* Badge in the corner of the list view, out of the rows' way. */
    sprite_mode = 1;
    apply_sprite_mode();
}

static void enter_agents_list(void)
{
    stop_monitor();
    agents_mode = 1;
    if (store_button) lv_obj_add_flag(store_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(scene_obj, 250);
    detail_agent = -1;
    set_status(agent_feed_count ? "Agents / pick an agent" : "Agents / none connected");
    lv_obj_add_flag(console_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(list_title, LV_OBJ_FLAG_HIDDEN);
    show_console(false);
    show_log(false);
    lv_obj_add_flag(musing_label, LV_OBJ_FLAG_HIDDEN);
    set_sprite_small();
    for (uint32_t row = 0; row < AGENT_ROWS; row++) {
        int agent = agents_page * AGENT_ROWS + (int)row;
        if (agent < agent_feed_count) {
            char caption[40];
            snprintf(caption, sizeof(caption), "%02d  %.24s", agent + 1,
                     agent_feed[agent].agent);
            lv_label_set_text(lv_obj_get_child(agent_rows[row], 0), caption);
            lv_label_set_text(agent_row_states[row], agent_feed[agent].state);
            lv_obj_remove_flag(agent_rows[row], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(agent_rows[row], LV_OBJ_FLAG_HIDDEN);
        }
    }
    set_dock_caption(0, "back");
    set_dock_caption(1, "");
    set_dock_caption(2, agent_feed_count > AGENT_ROWS ? "more" : "");
}

static lv_timer_t *monitor_timer;
static unsigned monitor_phase;
static const char *monitor_seq[] = {"loading", "look-left", "look-right", "reading"};
static void monitor_swap(lv_timer_t *t)
{
    play_state(monitor_seq[monitor_phase++ % 4], false);
}
static void stop_monitor(void)
{
    if (monitor_timer) { lv_timer_delete(monitor_timer); monitor_timer = NULL; }
}
static void show_agent_detail(void)
{
    agents_mode = 2;
    lv_obj_add_flag(list_title, LV_OBJ_FLAG_HIDDEN);

    const gemmi_agent_state_t *entry = &agent_feed[detail_agent];
    char status[80];
    snprintf(status, sizeof(status), "watching %s / %s", entry->agent, entry->state);
    set_status(status);
    show_console(false);
    for (uint32_t slot = 0; slot < AGENT_ROWS; slot++) {
        lv_obj_add_flag(agent_rows[slot], LV_OBJ_FLAG_HIDDEN);
    }
    /* Detail puts the sprite beside the feed, a little bigger than a badge. */
    sprite_mode = 2;
    apply_sprite_mode();
    if (last_musing[0]) {
        lv_label_set_text(musing_label, last_musing);
        lv_obj_remove_flag(musing_label, LV_OBJ_FLAG_HIDDEN);
    }
    /* Most recent line last, oldest first, one per row. */
    char text[AGENT_LOG_LEN * (AGENT_LOG_TEXT + 11)];
    size_t used = 0;
    int shown = entry->log_count < AGENT_LOG_LEN ? entry->log_count : AGENT_LOG_LEN;
    for (int row = 0; row < shown; row++) {
        int idx = (entry->log_head - shown + row + AGENT_LOG_LEN) % AGENT_LOG_LEN;
        const char *line = entry->log[idx];
        /* Line-type colours: thinking in blue, tool calls dim, rest dark. */
        const char *colour = "#222222";
        if (!strncmp(line, "think", 5) || !strncmp(line, "cot", 3)) {
            colour = "#1E6FA8";
        } else if (!strncmp(line, "tool", 4) || !strncmp(line, "ran", 3)) {
            colour = "#666666";
        } else if (!strncmp(line, "warn", 4) || !strncmp(line, "err", 3)) {
            colour = "#B04A1E";
        }
        used += snprintf(text + used, sizeof(text) - used,
                         "%s %s#\n", colour, line);
    }
    if (shown == 0) strlcpy(text, "no log yet\n", sizeof(text));
    lv_label_set_text(log_label, text);
    show_log(true);
    set_dock_caption(0, "back");
    set_dock_caption(1, "next");
    set_dock_caption(2, "");
    /* Watching an agent: alternate working and monitoring animations. */
    if (!monitor_timer) {
        monitor_phase = 0;
        monitor_timer = lv_timer_create(monitor_swap, 2600, NULL);
    }
}

static void set_sprite_home(void)
{
    sprite_mode = 0;
    apply_sprite_mode();
}

static void show_home(void)
{
    stop_monitor();
    hide_jpeg();
    ask_foreground = false;
    if (ask_back) lv_obj_add_flag(ask_back, LV_OBJ_FLAG_HIDDEN);
    pet_mode = 0;
    if (shop_panel) lv_obj_add_flag(shop_panel, LV_OBJ_FLAG_HIDDEN);
    agents_mode = 0;
    lv_obj_set_height(scene_obj, SCENE_H);
    lv_obj_add_flag(list_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(console_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(console_title, 20, BUBBLE_Y - 5);
    lv_obj_set_pos(bubble_obj, BUBBLE_X, BUBBLE_Y);
    lv_obj_set_size(bubble_obj, BUBBLE_W, BUBBLE_H);
    lv_obj_set_size(message_label, BUBBLE_W - 20, 34);
    show_log(false);
    lv_obj_add_flag(musing_label, LV_OBJ_FLAG_HIDDEN);
    for (uint32_t slot = 0; slot < AGENT_ROWS; slot++) {
        lv_obj_add_flag(agent_rows[slot], LV_OBJ_FLAG_HIDDEN);
    }
    agents_page = 0;
    set_sprite_home();
    if (ask_pending) {
        set_status("gemmi needs an answer");
        set_bubble("ask waiting", pending_ask.message, "1 ask");
        show_console(true);
    } else {
        set_status("Idle");
        set_bubble("gemmi", "Ready when you are.", "");
        show_console(true);
    }
    show_nav_dock();
    play_state("idle", false);
    lv_obj_invalidate(lv_obj_get_screen(bubble_obj));
}

static void refresh_pet_badge(void)
{
    if (!pet_badge) return;
    char text[64];
    const gemmi_pet_stats_t *stats = gemmi_pet_stats();
    snprintf(text, sizeof(text), "Lv.%lu / %lu gems", (unsigned long)gemmi_pet_level(), (unsigned long)stats->gems);
    lv_label_set_text(pet_badge, text);

}

static void render_pet_room(const char *feedback)
{
    if (!shop_panel) return;
    if (!pet_mode) {
        lv_obj_add_flag(shop_panel, LV_OBJ_FLAG_HIDDEN);
        if (!ask_foreground && !agents_mode) { sprite_mode = 0; apply_sprite_mode(); }
        lv_obj_invalidate(lv_obj_get_screen(bubble_obj));
        return;
    }
    sprite_mode = 3;
    apply_sprite_mode();
    lv_obj_remove_flag(shop_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(shop_panel);
    const gemmi_pet_stats_t *stats = gemmi_pet_stats();
    for (unsigned i = 0; i < 4; i++) {
        char price[24];
        int item = shop_catalog[shop_page * 4 + i];
        if (item < 0) { lv_obj_add_flag(shop_cards[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(shop_cards[i], LV_OBJ_FLAG_HIDDEN);
        const char *names[] = {"Specs", "Crown", "Halo", "Stalk", "Scarf", "Phones", "Bow", "Snack"};
        lv_label_set_text(shop_names[i], names[item]);
        lv_obj_clean(shop_previews[i]);
        draw_shop_preview(shop_previews[i], (unsigned)item);
        bool owned = item < GEMMI_PET_ITEM_COUNT && (stats->owned & (1u << item));
        bool equipped = item < GEMMI_PET_ITEM_COUNT && stats->equipped == (unsigned)item + 1;
        snprintf(price, sizeof(price), "%s", equipped ? "On" : owned ? "Owned" : "");
        if (!owned) snprintf(price, sizeof(price), "%lu gems", (unsigned long)(item < GEMMI_PET_ITEM_COUNT ? gemmi_pet_items[item].cost : 3));
        lv_label_set_text(shop_prices[i], price);
        lv_obj_set_style_border_color(shop_cards[i], lv_color_hex(equipped ? 0x9477AA : 0xD3CFD8), 0);
        lv_obj_set_style_bg_color(shop_cards[i], lv_color_hex(equipped ? 0xF5F1FA : 0xFFFFFF), 0);
    }
    lv_label_set_text(shop_title, shop_page ? "Little shop 2/2" : "Little shop 1/2");
    lv_label_set_text(shop_feedback, !gemmi_pet_save_ok() ? "Could not save progress" : feedback ? feedback : "Tap to buy / equip");
    refresh_pet_badge(); refresh_pet_look();
    lv_obj_invalidate(lv_obj_get_screen(bubble_obj));
}

static void open_pet_room(void)
{
    if (ask_foreground || ask_pending || agents_mode) return;
    pet_mode = !pet_mode;
    render_pet_room(NULL);
}
void gemmi_ui_open_pet(void) { open_pet_room(); }

static void on_shop(lv_event_t *event) { LV_UNUSED(event); open_pet_room(); }
static void on_shop_item(lv_event_t *event)
{
    unsigned slot = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    int item = shop_catalog[shop_page * 4 + slot];
    if (item < 0) return;
    const char *feedback = item < GEMMI_PET_ITEM_COUNT ? gemmi_pet_shop((unsigned)item) : gemmi_pet_care(1);
    render_pet_room(feedback);
    if (item == GEMMI_PET_ITEM_COUNT) play_state("success", true);
}
static void on_shop_next(lv_event_t *event) { LV_UNUSED(event); shop_page = 1 - shop_page; render_pet_room(NULL); }
static void on_rest(lv_event_t *event)
{
    LV_UNUSED(event);
    if (ask_foreground || ask_pending || agents_mode) return;
    gemmi_pet_care(2);
    play_state("idle", false);
    refresh_pet_badge();
}
static void on_pet(lv_event_t *event)
{
    LV_UNUSED(event);
    if (ask_foreground || ask_pending || agents_mode) return;
    gemmi_pet_care(0);
    refresh_pet_badge();
    play_state("success", true);
}

static void enter_ask(void)
{
    pet_mode = 0;
    if (store_button) lv_obj_add_flag(store_button, LV_OBJ_FLAG_HIDDEN);
    if (shop_panel) lv_obj_add_flag(shop_panel, LV_OBJ_FLAG_HIDDEN);
    agents_mode = 0;
    ask_foreground = true;
    if (pending_ask.choice_count == 3) lv_obj_remove_flag(ask_back, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ask_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(scene_obj, 124);
    lv_obj_add_flag(list_title, LV_OBJ_FLAG_HIDDEN);
    show_log(false);
    lv_obj_add_flag(musing_label, LV_OBJ_FLAG_HIDDEN);
    for (int row = 0; row < AGENT_ROWS; row++) lv_obj_add_flag(agent_rows[row], LV_OBJ_FLAG_HIDDEN);
    sprite_mode = 3;
    apply_sprite_mode();
    lv_point_t question_size;
    lv_text_get_size(&question_size, pending_ask.message, UI_BODY, 0, 4,
                     BUBBLE_W - 20, LV_TEXT_FLAG_NONE);
    int question_height = question_size.y + 34;
    if (question_height < BUBBLE_H) question_height = BUBBLE_H;
    if (question_height > 126) question_height = 126;
    int question_y = 276 - question_height;
    lv_obj_set_pos(bubble_obj, BUBBLE_X, question_y);
    lv_obj_set_size(bubble_obj, BUBBLE_W, question_height);
    lv_obj_set_size(message_label, BUBBLE_W - 20, question_height - 28);
    lv_obj_set_pos(console_title, 20, question_y - 5);
    lv_obj_remove_flag(console_title, LV_OBJ_FLAG_HIDDEN);
    char status[80];
    snprintf(status, sizeof(status), "answer %s", pending_ask.agent);
    set_status(status);
    show_console(true);
    set_bubble("ask", pending_ask.message, "pick one");
    select_choice(-1);
    for (uint32_t slot = 0; slot < 3; slot++) {
        const char *caption = "";
        if ((int)slot < pending_ask.choice_count) {
            caption = pending_ask.choices[slot];
        } else if (pending_ask.choice_count < 3 && (int)slot == pending_ask.choice_count) {
            caption = "back";
        }
        set_dock_caption(slot, caption);
    }
    play_state("needs-attention", false);
}

/* Back only navigates: keep the question pending without sending an answer. */
static void dismiss_ask(void)
{
    show_home();
}

static void on_ask_back(lv_event_t *event) { LV_UNUSED(event); dismiss_ask(); }

static void send_choice(int slot)
{
    const char *choice = pending_ask.choices[slot];
    gemmi_bridge_answer(pending_ask.id, choice);
    ask_pending = false;
    ask_foreground = false;
    lv_obj_add_flag(ask_back, LV_OBJ_FLAG_HIDDEN);
    set_sprite_home();
    lv_obj_set_height(scene_obj, SCENE_H);
    lv_obj_set_pos(bubble_obj, BUBBLE_X, BUBBLE_Y);
    lv_obj_set_size(bubble_obj, BUBBLE_W, BUBBLE_H);
    lv_obj_set_size(message_label, BUBBLE_W - 20, 34);
    lv_obj_set_pos(console_title, 20, BUBBLE_Y - 5);
    char status[80];
    snprintf(status, sizeof(status), "gemmi sent: %s", choice);
    set_status(status);
    show_console(true);
    set_bubble("answered", pending_ask.message, "done");
    show_nav_dock();
    play_state("success", true);
}

static void on_dock(lv_event_t *event)
{
    int slot = (int)(uintptr_t)lv_event_get_user_data(event);
    if (ask_foreground) {
        if (slot == selected_choice) {
            send_choice(slot);
        } else if (slot < pending_ask.choice_count) {
            select_choice(slot);
            char status[80];
            snprintf(status, sizeof(status), "again to send: %s", pending_ask.choices[slot]);
            set_status(status);
        } else if (slot == pending_ask.choice_count && pending_ask.choice_count < 3) {
            dismiss_ask();
        }
        return;
    }
    if (pet_mode) { pet_mode = 0; render_pet_room(NULL); }
    if (agents_mode >= 3) { if (slot == 0) show_home(); return; }
    if (agents_mode == 1) {
        if (slot == 0) {
            show_home();
        } else if (slot == 2 && agent_feed_count > AGENT_ROWS) {
            agents_page = (agents_page + 1) * AGENT_ROWS < agent_feed_count
                              ? agents_page + 1 : 0;
            enter_agents_list();
        }
        return;
    }
    if (agents_mode == 2) {
        if (slot == 0) {
            enter_agents_list();
        } else if (slot == 1) {
            detail_agent = (detail_agent + 1) % agent_feed_count;
            show_agent_detail();
        }
        return;
    }
    switch ((gemmi_action_t)(GEMMI_ACTION_AGENTS + slot)) {
    case GEMMI_ACTION_AGENTS:
        enter_agents_list();
        play_state("working", true);
        break;
    case GEMMI_ACTION_ASKS:
        if (ask_pending) {
            enter_ask();
        } else {
            agents_mode = 3;
            set_status("gemmi checked - no asks");
            set_bubble("Gemmi", "No questions waiting.", "");
            show_console(true);
            set_dock_caption(0, "back"); set_dock_caption(1, ""); set_dock_caption(2, "");
            play_state("thinking", true);
        }
        break;
    case GEMMI_ACTION_VIEW:
        agents_mode = 4;
        select_choice(-1);
        if (store_button) lv_obj_add_flag(store_button, LV_OBJ_FLAG_HIDDEN);
        set_dock_caption(0, "back"); set_dock_caption(1, ""); set_dock_caption(2, "");
        if (jpeg_buf) {
            char status[80];
            snprintf(status, sizeof(status), "gemmi is viewing %.40s",
                     last_artifact.artifact[0] ? last_artifact.artifact : last_artifact.agent);
            set_status(status);
            show_console(false);
            /* Small Gemmi in the corner "peeking" at the image. */
            sprite_mode = 1;
            apply_sprite_mode();
            lv_obj_remove_flag(jpeg_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(jpeg_view);
        } else if (have_artifact) {
            char status[80];
            snprintf(status, sizeof(status), "gemmi is viewing %.40s",
                     last_artifact.artifact[0] ? last_artifact.artifact : last_artifact.agent);
            set_status(status);
            set_bubble("Gemmi", last_artifact.artifact[0] ? last_artifact.artifact : last_artifact.message, "");
            show_console(true);
        } else {
            set_status("gemmi found no artifact");
            set_bubble("Gemmi", "No images shared yet.", "");
            show_console(true);
        }
        play_state("reading", true);
        break;
    default:
        show_home();
        break;
    }
}

static void on_agent_row(lv_event_t *event)
{
    int slot = agents_page * AGENT_ROWS + (int)(uintptr_t)lv_event_get_user_data(event);
    if (agents_mode == 1 && slot < agent_feed_count) {
        detail_agent = slot;
        show_agent_detail();
    }
}

static void on_bubble(lv_event_t *event)
{
    LV_UNUSED(event);
    if (pet_mode) { show_home(); return; }
    if (ask_foreground) {
        dismiss_ask();
    } else if (agents_mode == 2) {
        enter_agents_list();
    } else if (agents_mode == 1 || agents_mode >= 3) {
        show_home();
    }
}

static lv_obj_t *create_button(lv_obj_t *parent, int16_t x, int16_t y, int16_t width, int16_t height,
                               int slot)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_pad_all(button, 1, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    /* Match the console frame with square grey outlines. */
    lv_obj_set_style_border_color(button, lv_color_hex(0x444444), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0x444444), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xEEEEEE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xEEEEEE), LV_STATE_CHECKED);
    /* Resistive touch often loses the release position, and CLICKED needs a
     * clean press+release inside the widget. PRESSED fires on touch-down. */
    lv_obj_add_event_cb(button, on_dock, LV_EVENT_PRESSED, (void *)(uintptr_t)slot);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_hex(0x777777), 0);
    lv_obj_set_style_text_font(label, UI_MONO, 0);
    make_label_upright(label, width - 4, 14);
    lv_obj_center(label);
    return label;
}

static lv_obj_t *pet_outline(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, width, height);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x35304E), 0);
    lv_obj_set_style_radius(obj, 1, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void refresh_pet_look(void)
{
    if (!pet_glasses || !pet_crown) return;
    unsigned equipped = gemmi_pet_stats()->equipped;
    bool visible = sprite_mode == 0 || sprite_mode == 3;
    lv_obj_add_flag(pet_glasses, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pet_crown, LV_OBJ_FLAG_HIDDEN);
    if (pet_bow) lv_obj_add_flag(pet_bow, LV_OBJ_FLAG_HIDDEN);
    if (pet_halo) lv_obj_add_flag(pet_halo, LV_OBJ_FLAG_HIDDEN);
    if (pet_antenna) lv_obj_add_flag(pet_antenna, LV_OBJ_FLAG_HIDDEN);
    if (pet_scarf) lv_obj_add_flag(pet_scarf, LV_OBJ_FLAG_HIDDEN);
    if (pet_horns) lv_obj_add_flag(pet_horns, LV_OBJ_FLAG_HIDDEN);
    if (visible && equipped == GEMMI_PET_GLASSES + 1) {
        lv_obj_remove_flag(pet_glasses, LV_OBJ_FLAG_HIDDEN);
        /* The room uses the smaller native sprite; keep accessories proportional. */
        if (sprite_mode == 0) {
            lv_obj_set_pos(pet_glasses, 87, 76);
            lv_obj_set_size(pet_glasses, 66, 16);
            lv_obj_set_pos(lv_obj_get_child(pet_glasses, 0), 0, 0);
            lv_obj_set_size(lv_obj_get_child(pet_glasses, 0), 21, 16);
            lv_obj_set_pos(lv_obj_get_child(pet_glasses, 1), 45, 0);
            lv_obj_set_size(lv_obj_get_child(pet_glasses, 1), 21, 16);
            lv_obj_set_pos(lv_obj_get_child(pet_glasses, 2), 21, 7);
            lv_obj_set_size(lv_obj_get_child(pet_glasses, 2), 24, 1);
        } else {
            lv_obj_set_pos(pet_glasses, 101, 50);
            lv_obj_set_size(pet_glasses, 38, 10);
            lv_obj_set_pos(lv_obj_get_child(pet_glasses, 0), 0, 0);
            lv_obj_set_size(lv_obj_get_child(pet_glasses, 0), 12, 10);
            lv_obj_set_pos(lv_obj_get_child(pet_glasses, 1), 26, 0);
            lv_obj_set_size(lv_obj_get_child(pet_glasses, 1), 12, 10);
            lv_obj_set_pos(lv_obj_get_child(pet_glasses, 2), 12, 4);
            lv_obj_set_size(lv_obj_get_child(pet_glasses, 2), 14, 1);
        }
    }
    if (visible && equipped == GEMMI_PET_CROWN + 1) {
        lv_obj_remove_flag(pet_crown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pet_crown, 103, sprite_mode == 0 ? 8 : 12);
    }
    if (visible && equipped == GEMMI_PET_BOW + 1 && pet_bow) {
        lv_obj_remove_flag(pet_bow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pet_bow, 108, sprite_mode == 0 ? 112 : 73);
    }
    if (visible && equipped == GEMMI_PET_HALO + 1 && pet_halo) {
        lv_obj_remove_flag(pet_halo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pet_halo, sprite_mode == 0 ? 100 : 100, sprite_mode == 0 ? 2 : 0);
    }
    if (visible && equipped == GEMMI_PET_ANTENNA + 1 && pet_antenna) {
        lv_obj_remove_flag(pet_antenna, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pet_antenna, sprite_mode == 0 ? 114 : 114, sprite_mode == 0 ? 0 : 0);
    }
    if (visible && equipped == GEMMI_PET_SCARF + 1 && pet_scarf) {
        lv_obj_remove_flag(pet_scarf, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pet_scarf, 86, sprite_mode == 0 ? 100 : 64);
    }
    if (visible && equipped == GEMMI_PET_HORNS + 1 && pet_horns) {
        lv_obj_remove_flag(pet_horns, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pet_horns, sprite_mode == 0 ? 58 : 60, sprite_mode == 0 ? 14 : 2);
    }
    /* GIF replacement creates a new topmost sibling; keep accessories above it. */
    lv_obj_move_foreground(pet_glasses);
    lv_obj_move_foreground(pet_crown);
    if (pet_bow) lv_obj_move_foreground(pet_bow);
    if (pet_halo) lv_obj_move_foreground(pet_halo);
    if (pet_antenna) lv_obj_move_foreground(pet_antenna);
    if (pet_scarf) lv_obj_move_foreground(pet_scarf);
    if (pet_horns) lv_obj_move_foreground(pet_horns);
}

#ifdef GEMMI_SYSTEM_MENLO
#define PP(x,y) {(x)*4,(y)*4}
#define PET_STROKE 3
#define PET_SMALL_FONT (&gemmi_menlo_8)
#else
#define PP(x,y) {x,y}
#define PET_STROKE 1
#define PET_SMALL_FONT UI_MONO
#endif
static void pet_path(lv_obj_t *parent, const lv_point_precise_t *points, unsigned count, uint32_t colour)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, count);
    lv_obj_set_style_line_width(line, PET_STROKE, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(colour), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
}
static lv_obj_t *pet_caption(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, PET_SMALL_FONT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x77717E), 0);
    lv_obj_set_pos(label, x, y); lv_obj_set_size(label, width, 12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}
static void draw_shop_preview(lv_obj_t *preview, unsigned item)
{
        if (item == 0) {
            static const lv_point_precise_t specs[] = {PP(1,7),PP(8,7),PP(8,14),PP(1,14),PP(1,7),PP(8,7),PP(8,10),PP(14,10),PP(14,7),PP(21,7),PP(21,14),PP(14,14),PP(14,7)};
            for (int lens = 0; lens < 2; lens++) {
                lv_obj_t *glass = pet_outline(preview, lens ? 14 : 1, 7, 7, 7);
                lv_obj_set_style_border_width(glass, 0, 0);
                lv_obj_set_style_bg_color(glass, lv_color_hex(0xE4DCF2), 0);
                lv_obj_set_style_bg_opa(glass, LV_OPA_COVER, 0);
            }
            pet_path(preview, specs, 13, 0x6D6487);
        } else if (item == 1) {
            static const lv_point_precise_t crown[] = {PP(1,6),PP(6,10),PP(11,2),PP(16,10),PP(21,6),PP(19,17),PP(3,17),PP(1,6)};
            pet_path(preview, crown, 8, 0xB18B3B);
            lv_obj_t *jewel = pet_outline(preview, 9, 11, 4, 4);
            lv_obj_set_style_bg_color(jewel, lv_color_hex(0xD4ABBE), 0);
            lv_obj_set_style_bg_opa(jewel, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(jewel, lv_color_hex(0xB18B3B), 0);
        } else if (item == GEMMI_PET_HALO) {
            static const lv_point_precise_t halo[] = {PP(4,9),PP(7,6),PP(11,5),PP(15,6),PP(18,9),PP(15,12),PP(11,13),PP(7,12),PP(4,9)};
            pet_path(preview, halo, 9, 0xC9A227);
        } else if (item == GEMMI_PET_ANTENNA) {
            static const lv_point_precise_t stalk[] = {PP(11,20),PP(11,8)};
            pet_path(preview, stalk, 2, 0x6D6487);
            lv_obj_t *bob = pet_outline(preview, 9, 3, 5, 5);
            lv_obj_set_style_radius(bob, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(bob, lv_color_hex(0xD4556B), 0);
            lv_obj_set_style_bg_opa(bob, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bob, 0, 0);
            static const lv_point_precise_t wave1[] = {PP(5,7),PP(7,4),PP(9,7)};
            static const lv_point_precise_t wave2[] = {PP(13,7),PP(15,4),PP(17,7)};
            pet_path(preview, wave1, 3, 0x7399BD); pet_path(preview, wave2, 3, 0x7399BD);
        } else if (item == GEMMI_PET_SCARF) {
            static const lv_point_precise_t scarf[] = {PP(2,8),PP(8,5),PP(14,5),PP(20,8),PP(20,13),PP(14,16),PP(8,16),PP(2,13),PP(2,8)};
            pet_path(preview, scarf, 9, 0xB58297);
            lv_obj_t *tail = pet_outline(preview, 15, 12, 4, 7);
            lv_obj_set_style_bg_color(tail, lv_color_hex(0xF1DBE4), 0);
            lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(tail, lv_color_hex(0xB58297), 0);
        } else if (item == GEMMI_PET_HORNS) {
            static const lv_point_precise_t horn_l[] = {PP(2,14),PP(4,4),PP(8,11),PP(2,14)};
            static const lv_point_precise_t horn_r[] = {PP(14,11),PP(18,4),PP(20,14),PP(14,11)};
            pet_path(preview, horn_l, 4, 0x8A4A58); pet_path(preview, horn_r, 4, 0x8A4A58);
            lv_obj_t *fill_l = pet_outline(preview, 4, 9, 3, 5);
            lv_obj_set_style_bg_color(fill_l, lv_color_hex(0xE4BCC6), 0);
            lv_obj_set_style_bg_opa(fill_l, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(fill_l, 0, 0);
            lv_obj_t *fill_r = pet_outline(preview, 15, 9, 3, 5);
            lv_obj_set_style_bg_color(fill_r, lv_color_hex(0xE4BCC6), 0);
            lv_obj_set_style_bg_opa(fill_r, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(fill_r, 0, 0);
        } else if (item == GEMMI_PET_BOW) {
            static const lv_point_precise_t bow[] = {PP(1,5),PP(11,10),PP(1,16),PP(1,5),PP(11,10),PP(21,5),PP(21,16),PP(11,10),PP(7,20),PP(11,17),PP(15,20)};
            pet_path(preview, bow, 11, 0xB58297);
            lv_obj_t *knot = pet_outline(preview, 9, 8, 4, 4);
            lv_obj_set_style_bg_color(knot, lv_color_hex(0xD4ABBE), 0);
            lv_obj_set_style_bg_opa(knot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(knot, lv_color_hex(0xB58297), 0);
        } else {
            static const lv_point_precise_t cookie[] = {PP(5,3),PP(15,3),PP(20,8),PP(20,14),PP(15,19),PP(5,19),PP(1,14),PP(1,8),PP(5,3)};
            pet_path(preview, cookie, 9, 0xB18B3B);
            pet_outline(preview, 6, 7, 2, 2); pet_outline(preview, 13, 12, 2, 2);
        }
}

static void init_compact_pet_hud(lv_obj_t *screen)
{
    /* The store lives on the dock's bottom-right button instead of a
     * top-right icon. */
    shop_panel = pet_outline(screen, 12, 171, 216, 105);
    lv_obj_set_style_bg_color(shop_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(shop_panel, LV_OPA_COVER, 0);
    shop_title = pet_caption(shop_panel, "Little shop 1/2", 8, 5, 150);
    lv_obj_t *next = pet_outline(shop_panel, 166, 1, 22, 19);
    lv_obj_set_style_border_width(next, 0, 0);
    lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next, on_shop_next, LV_EVENT_PRESSED, NULL);
    pet_caption(next, ">", 7, 2, 10);
    lv_obj_t *close = pet_outline(shop_panel, 191, 1, 22, 19);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close, on_shop, LV_EVENT_PRESSED, NULL);
    pet_caption(close, "x", 7, 2, 10);
    for (unsigned i = 0; i < 4; i++) {
        lv_obj_t *card = shop_cards[i] = pet_outline(shop_panel, 6 + (int)i * 52, 23, 47, 59);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_shop_item, LV_EVENT_PRESSED, (void *)(uintptr_t)i);
        shop_previews[i] = pet_outline(card, 12, 4, 23, 21);
        lv_obj_set_style_border_width(shop_previews[i], 0, 0);
        shop_names[i] = pet_caption(card, "", 1, 28, 43);
        lv_obj_set_style_text_align(shop_names[i], LV_TEXT_ALIGN_CENTER, 0);
        shop_prices[i] = pet_caption(card, "", 1, 42, 43);
        lv_obj_set_style_text_align(shop_prices[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    shop_feedback = pet_caption(shop_panel, "Tap to buy / equip", 8, 88, 198);
    lv_obj_set_style_text_align(shop_feedback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(shop_panel, LV_OBJ_FLAG_HIDDEN);
}
#undef PP

static void init_pet_decorations(lv_obj_t *screen, lv_obj_t *scene)
{
    init_compact_pet_hud(screen);
    pet_badge = lv_label_create(screen);
    lv_obj_set_pos(pet_badge, 110, 8);
    lv_obj_set_size(pet_badge, 118, 12);
#ifdef GEMMI_SYSTEM_MENLO
    lv_obj_set_style_text_font(pet_badge, &gemmi_menlo_8, 0);
#else
    lv_obj_set_style_text_font(pet_badge, UI_MONO, 0);
#endif
    lv_obj_set_style_text_align(pet_badge, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(pet_badge, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(pet_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pet_badge, on_shop, LV_EVENT_PRESSED, NULL);
    pet_glasses = pet_outline(scene, 87, 76, 66, 16);
    lv_obj_set_style_border_width(pet_glasses, 0, 0);
    pet_outline(pet_glasses, 0, 0, 21, 16);
    pet_outline(pet_glasses, 45, 0, 21, 16);
    pet_outline(pet_glasses, 21, 7, 24, 1);
    pet_crown = lv_line_create(scene);
#ifdef GEMMI_SYSTEM_MENLO
#define CP(x,y) {(x)*4,(y)*4}
#else
#define CP(x,y) {x,y}
#endif
    static const lv_point_precise_t crown[] = {CP(0,4),CP(8,9),CP(17,0),CP(26,9),CP(34,4),CP(30,18),CP(4,18),CP(0,4)};
    lv_line_set_points(pet_crown, crown, 8);
    lv_obj_set_style_line_color(pet_crown, lv_color_hex(0xB18B3B), 0);
#ifdef GEMMI_SYSTEM_MENLO
    lv_obj_set_style_line_width(pet_crown, 6, 0);
#else
    lv_obj_set_style_line_width(pet_crown, 2, 0);
#endif
    lv_obj_remove_flag(pet_crown, LV_OBJ_FLAG_CLICKABLE);
    pet_bow = pet_outline(scene, 108, 112, 24, 16);
    lv_obj_set_style_border_width(pet_bow, 0, 0);
    static const lv_point_precise_t ribbon[] = {CP(1,3),CP(12,8),CP(1,13),CP(1,3),CP(12,8),CP(23,3),CP(23,13),CP(12,8),CP(8,16),CP(12,13),CP(16,16)};
#undef CP
    pet_path(pet_bow, ribbon, 11, 0xB58297);
    lv_obj_t *knot = pet_outline(pet_bow, 10, 6, 4, 4);
    lv_obj_set_style_bg_color(knot, lv_color_hex(0xD4ABBE), 0);
    lv_obj_set_style_bg_opa(knot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(knot, lv_color_hex(0xB58297), 0);
    /* Wearables: halo ring, antenna, scarf, horns. */
    pet_halo = pet_outline(scene, 100, 0, 40, 10);
    lv_obj_set_style_bg_opa(pet_halo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(pet_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(pet_halo, lv_color_hex(0xC9A227), 0);
    lv_obj_set_style_border_width(pet_halo, 2, 0);
    pet_antenna = pet_outline(scene, 115, 0, 12, 14);
    lv_obj_set_style_border_width(pet_antenna, 0, 0);
    static const lv_point_precise_t ant_stem[] = {{6,13},{6,5}};
    pet_path(pet_antenna, ant_stem, 2, 0x6D6487);
    lv_obj_t *ant_bob = pet_outline(pet_antenna, 3, 0, 6, 6);
    lv_obj_set_style_radius(ant_bob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ant_bob, lv_color_hex(0xD4556B), 0);
    lv_obj_set_style_bg_opa(ant_bob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ant_bob, 0, 0);
    pet_scarf = pet_outline(scene, 86, 98, 68, 16);
    lv_obj_set_style_border_width(pet_scarf, 0, 0);
    lv_obj_t *band = pet_outline(pet_scarf, 0, 0, 68, 8);
    lv_obj_set_style_bg_color(band, lv_color_hex(0xF1DBE4), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(band, lv_color_hex(0xB58297), 0);
    lv_obj_t *tail = pet_outline(pet_scarf, 46, 5, 10, 11);
    lv_obj_set_style_bg_color(tail, lv_color_hex(0xF1DBE4), 0);
    lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tail, lv_color_hex(0xB58297), 0);
    pet_horns = pet_outline(scene, 58, 14, 124, 24);
    lv_obj_set_style_border_width(pet_horns, 0, 0);
    /* Headphones: band arc + two earcup squares hugging the temples. */
    static const lv_point_precise_t hp_band[] = {{4,16},{10,2},{62,0},{114,2},{120,16}};
    static const lv_point_precise_t hp_cup_l[] = {{0,8},{0,22},{12,22},{12,8},{0,8}};
    static const lv_point_precise_t hp_cup_r[] = {{112,8},{112,22},{124,22},{124,8},{112,8}};
    pet_path(pet_horns, hp_band, 5, 0x8A4A58);
    pet_path(pet_horns, hp_cup_l, 5, 0x8A4A58);
    pet_path(pet_horns, hp_cup_r, 5, 0x8A4A58);
    refresh_pet_look();
}

void gemmi_ui_init(lv_display_t *display)
{
    gemmi_pet_init();
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Flat black-and-white UI keeps the panel clean; Gemmi is the only colour. */
    lv_obj_t *scene = scene_obj = lv_obj_create(screen);
    lv_obj_set_pos(scene, 0, TOP_H);
    lv_obj_set_size(scene, SCENE_W, SCENE_H);
    lv_obj_set_style_bg_color(scene, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(scene, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(scene, 0, 0);
    lv_obj_set_style_radius(scene, 0, 0);
    lv_obj_set_style_pad_all(scene, 0, 0);
    lv_obj_set_scrollable(scene, false);

    /* Agent list rows, one tappable numbered row per active agent. They sit
     * below the sprite badge so nothing overlaps. */
    list_title = lv_label_create(scene);
    lv_obj_set_pos(list_title, 12, 16);
    lv_obj_set_style_text_font(list_title, UI_MONO, 0);
    lv_obj_set_style_text_color(list_title, lv_color_hex(0x777777), 0);
    lv_label_set_text(list_title, "Your agents");
    lv_obj_add_flag(list_title, LV_OBJ_FLAG_HIDDEN);
    for (int slot = 0; slot < AGENT_ROWS; slot++) {
        agent_rows[slot] = lv_button_create(scene);
        lv_obj_set_pos(agent_rows[slot], 12, 58 + slot * 38);
        lv_obj_set_size(agent_rows[slot], SCENE_W - 24, 35);
        lv_obj_set_style_bg_color(agent_rows[slot], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(agent_rows[slot], lv_color_hex(0xF1EDF5), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(agent_rows[slot], lv_color_hex(0xEAEAEA), 0);
        lv_obj_set_style_border_side(agent_rows[slot], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(agent_rows[slot], 0, 0);
        lv_obj_set_style_shadow_width(agent_rows[slot], 0, 0);
        lv_obj_set_style_border_width(agent_rows[slot], 1, 0);
        lv_obj_set_style_radius(agent_rows[slot], 0, 0);
        lv_obj_t *label = lv_label_create(agent_rows[slot]);
        lv_obj_set_style_text_color(label, lv_color_hex(0x444444), 0);
        lv_obj_set_style_text_font(label, UI_BODY, 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 6, 1);
        agent_row_states[slot] = lv_label_create(agent_rows[slot]);
        lv_obj_set_style_text_font(agent_row_states[slot], UI_MONO, 0);
        lv_obj_set_style_text_color(agent_row_states[slot], lv_color_hex(0x999999), 0);
        lv_obj_align(agent_row_states[slot], LV_ALIGN_BOTTOM_LEFT, 6, -3);
        lv_obj_add_flag(agent_rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(agent_rows[slot], on_agent_row, LV_EVENT_PRESSED,
                            (void *)(uintptr_t)slot);
    }

    /* Per-agent log view on the right of the scene while drilled in; the
     * sprite shrinks to the left beside a thought caption. */
    log_panel = lv_obj_create(screen);
    lv_obj_set_pos(log_panel, 12, 138);
    lv_obj_set_size(log_panel, SCREEN_W - 24, 138);
    lv_obj_set_style_bg_color(log_panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(log_panel, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(log_panel, 1, 0);
    lv_obj_set_style_radius(log_panel, 0, 0);
    lv_obj_set_style_pad_all(log_panel, 10, 0);
    lv_obj_set_scrollable(log_panel, false);
    lv_obj_add_flag(log_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(log_panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    log_title = lv_label_create(screen);
    lv_label_set_text(log_title, "Agent log");
    lv_obj_set_pos(log_title, 20, 133);
    lv_obj_add_flag(log_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(log_title, UI_MONO, 0);
    lv_obj_set_style_text_color(log_title, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(log_title, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(log_title, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(log_title, 4, 0);
    log_label = lv_label_create(log_panel);
    lv_obj_set_size(log_label, SCREEN_W - 46, 116);
    lv_obj_set_style_text_font(log_label, UI_MONO, 0);
    lv_obj_set_style_text_line_space(log_label, 6, 0);
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_CLIP);
    lv_label_set_recolor(log_label, true);
    lv_obj_set_style_text_color(log_label, lv_color_hex(0x444444), 0);

    musing_label = lv_label_create(scene);
    lv_obj_set_pos(musing_label, 116, 12);
    lv_obj_set_size(musing_label, 108, 84);
    lv_obj_set_style_text_font(musing_label, UI_BODY, 0);
    lv_label_set_long_mode(musing_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(musing_label, lv_color_hex(0x444444), 0);
    lv_obj_add_flag(musing_label, LV_OBJ_FLAG_HIDDEN);

    gif = lv_gif_create(scene);
    lv_obj_add_flag(gif, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gif, on_pet, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(gif, on_rest, LV_EVENT_LONG_PRESSED, NULL);
    lv_image_set_antialias(gif, false);
    lv_gif_set_color_format(gif, LV_COLOR_FORMAT_ARGB8888);
    lv_gif_set_src(gif, gemmi_anims[GEMMI_ANIM_IDLE].dsc);
    /* Frames are native 96 px; scale the image up so the sprite fills the
     * object instead of drawing tiny in a big box. */
    sprite_mode = 0;
    apply_sprite_mode();

    lv_obj_t *bubble = bubble_obj = lv_obj_create(screen);
    lv_obj_set_pos(bubble, BUBBLE_X, BUBBLE_Y);
    lv_obj_set_size(bubble, BUBBLE_W, BUBBLE_H);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_radius(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 0, 0);
    lv_obj_set_scrollable(bubble, false);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bubble, on_bubble, LV_EVENT_PRESSED, NULL);

    headline_label = lv_label_create(bubble);
    lv_obj_add_flag(headline_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(bubble, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    console_title = lv_label_create(screen);
    lv_label_set_text(console_title, "Gemmi");
    lv_obj_set_pos(console_title, 20, BUBBLE_Y - 5);
    lv_obj_set_style_text_font(console_title, UI_MONO, 0);
    lv_obj_set_style_text_color(console_title, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(console_title, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(console_title, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(console_title, 4, 0);
    message_label = lv_label_create(bubble);
    lv_obj_set_pos(message_label, 9, 13);
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_label, UI_BODY, 0);
    lv_obj_set_style_text_line_space(message_label, 4, 0);
    lv_obj_set_style_text_color(message_label, lv_color_hex(0x444444), 0);
    make_label_upright(message_label, BUBBLE_W - 20, 34);
    lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_LEFT, 0);

    queue_label = lv_label_create(bubble);
    lv_obj_set_style_text_font(queue_label, UI_MONO, 0);
    lv_obj_set_style_text_color(queue_label, lv_color_hex(0x999999), 0);
    lv_obj_align(queue_label, LV_ALIGN_BOTTOM_RIGHT, -9, -7);
    /* Keep the character's full 2x sprite unobstructed. The queue state is
     * represented by the top status and three fixed bottom controls instead. */
    lv_obj_add_flag(queue_label, LV_OBJ_FLAG_HIDDEN);

    status_label = lv_label_create(screen);
    lv_obj_set_pos(status_label, 20, 8);
    lv_obj_set_style_text_font(status_label, UI_MONO, 0);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xA799B2), 0);
    /* Small diamond status marker to the left of the caption. */
    static const lv_point_precise_t status_diamond[] = {{4,0},{8,4},{4,8},{0,4},{4,0}};
    lv_obj_t *badge_icon = lv_line_create(screen);
    lv_line_set_points(badge_icon, status_diamond, 5);
    lv_obj_set_pos(badge_icon, 9, 9);
    lv_obj_set_style_line_width(badge_icon, 2, 0);
    lv_obj_set_style_line_color(badge_icon, lv_color_hex(0xA799B2), 0);
    make_label_upright(status_label, 90, 12);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_LEFT, 0);

    /* Dock row aligns with the console box above it: edges at x=12 and x=228. */
    for (uint32_t index = 0; index < 3; index++) {
        dock_labels[index] = create_button(screen, 12 + (int16_t)index * 58, SCREEN_H - 34,
                                           52, 24, (int)index);
        dock_buttons[index] = lv_obj_get_parent(dock_labels[index]);
    }
    /* Store sits at the bottom-right of the dock row. */
    store_button = pet_outline(screen, 186, SCREEN_H - 34, 42, 24);
    lv_obj_set_style_border_color(store_button, lv_color_hex(0x888888), 0);
    lv_obj_add_flag(store_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(store_button, on_shop, LV_EVENT_PRESSED, NULL);
    lv_obj_t *bag_icon = pet_outline(store_button, 16, 6, 9, 10);
    lv_obj_set_style_border_width(bag_icon, 0, 0);
    static const lv_point_precise_t bag[] = {{1,3},{7,3},{8,9},{0,9},{1,3}};
    static const lv_point_precise_t handle[] = {{2,4},{2,1},{3,0},{5,0},{6,1},{6,4}};
    pet_path(bag_icon, bag, 5, 0x77717E);
    pet_path(bag_icon, handle, 6, 0x77717E);
    ask_back = pet_outline(screen, 12, 28, 40, 19);
    lv_obj_set_style_border_color(ask_back, lv_color_hex(0x888888), 0);
    lv_obj_add_flag(ask_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ask_back, on_ask_back, LV_EVENT_PRESSED, NULL);
    lv_obj_t *back_caption = pet_caption(ask_back, "Back", 4, 3, 30);
    lv_obj_set_style_text_align(back_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(ask_back, LV_OBJ_FLAG_HIDDEN);
    init_pet_decorations(screen, scene);
    refresh_pet_badge();
    show_home();
}

static void remember_agent(const gemmi_event_t *event)
{
    gemmi_agent_state_t *entry = NULL;
    for (int index = 0; index < agent_feed_count; index++) {
        if (strcmp(agent_feed[index].agent, event->agent) == 0) {
            entry = &agent_feed[index];
            break;
        }
    }
    if (entry == NULL && agent_feed_count < AGENT_FEED_LEN) {
        entry = &agent_feed[agent_feed_count++];
        memset(entry, 0, sizeof(*entry));
        strlcpy(entry->agent, event->agent, sizeof(entry->agent));
    }
    if (entry == NULL) return;
    strlcpy(entry->state, event->state, sizeof(entry->state));
    if (strcmp(event->kind, "log") == 0 && event->message[0] != '\0') {
        strlcpy(entry->log[entry->log_head], event->message, AGENT_LOG_TEXT);
        entry->log_head = (entry->log_head + 1) % AGENT_LOG_LEN;
        if (entry->log_count < AGENT_LOG_LEN) entry->log_count++;
    }
}

void gemmi_ui_offline(bool offline)
{
    static bool was_offline;
    if (!status_label || offline == was_offline) return;
    was_offline = offline;
    if (offline) {
        set_status("offline - still holding your asks");
    } else {
        set_status("gemmi is back online");
    }
}

void gemmi_ui_show_event(const gemmi_event_t *event)
{
    if (!gif || !headline_label || !message_label || !queue_label) return;
    remember_agent(event);
    uint32_t previous_level = gemmi_pet_level();
    bool rewarded = !strcmp(event->kind, "status") &&
                    gemmi_pet_event(event->agent, event->state, event->id);
    refresh_pet_badge();

    if (strcmp(event->kind, "ask") == 0) {
        pending_ask = *event;
        ask_pending = true;
        if (pending_ask.choice_count == 0) {
            strlcpy(pending_ask.choices[0], "ok", GEMMI_CHOICE_LEN);
            pending_ask.choice_count = 1;
        }
        enter_ask();
        return;
    }

    if (pet_mode) render_pet_room(rewarded ? "+25 XP / +5 gems" : NULL);

    char status[80];
    if (strcmp(event->agent, "gemmi") == 0 && event->message[0]) {
        strlcpy(last_musing, event->message, sizeof(last_musing));
    }
    if (strcmp(event->kind, "artifact") == 0) {
        last_artifact = *event;
        have_artifact = true;
        snprintf(status, sizeof(status), "gemmi is viewing %.40s",
                 event->artifact[0] ? event->artifact : event->agent);
        set_status(status);
        if (!ask_foreground) {
            show_console(true);
            set_bubble("artifact",
                       event->artifact[0] ? event->artifact : event->message, "preview");
        }
        play_state("reading", true);
        return;
    }

    /* Status updates change the top line and the character. A foreground ask
     * keeps its question and real choices on screen. */
    if (strcmp(event->state, "thinking") == 0) {
        snprintf(status, sizeof(status), "gemmi is thinking");
    } else {
        snprintf(status, sizeof(status), "watching %s / %s", event->agent, event->state);
    }
    set_status(status);
    if (!ask_foreground && !ask_pending) {
        char headline[80];
        snprintf(headline, sizeof(headline), "%s / %s", event->agent, event->state);
        show_console(true);
        set_bubble(headline, event->message, "attention needed");
    }
    if (rewarded && !ask_foreground && !ask_pending) {
        set_status(gemmi_pet_level() > previous_level ? "Level up!" : "Task complete");
        set_bubble("Gemmi", gemmi_pet_level() > previous_level ?
                   "Level up! +25 XP, +5 gems and a 10 gem bonus." :
                   "Nice work! +25 XP / +5 gems. Gemmi is growing with you.", "");
    }
    play_state(event->state, rewarded);
    if (rewarded && gemmi_pet_level() > previous_level && !ask_foreground && !ask_pending) {
        /* Let any queued task animation finish swapping before the celebration. */
        lv_timer_t *celebration = lv_timer_create(level_up_celebration, 350, NULL);
        lv_timer_set_repeat_count(celebration, 1);
    }
}
