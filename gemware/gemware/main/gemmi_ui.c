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

#include "esp_random.h"
#include "esp_timer.h"
#include "gemmi_anims.h"
#include "gemmi_bridge.h"
#include "gemmi_ui.h"
#include "lvgl.h"

#define SCREEN_W 240
#define SCREEN_H 320
#define TOP_H 35
#define DOCK_H 42
#define BUBBLE_X 8
#define BUBBLE_Y 36
#define BUBBLE_W (SCREEN_W - 16)
#define BUBBLE_H 26
#define MAX_BUBBLE_TEXT 34
#define AGENT_FEED_LEN 8

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
static lv_obj_t *bubble_obj;
static lv_timer_t *idle_timer;
static lv_obj_t *headline_label;
static lv_obj_t *message_label;
static lv_obj_t *queue_label;
static lv_obj_t *status_label;
static lv_obj_t *dock_buttons[3];
static lv_obj_t *dock_labels[3];
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

#define SCENE_W 226
#define SCENE_H 238
#define GIF_SIZE 196

/* Sprite placement: 0 = big centred, 1 = corner badge, 2 = detail left. */
static int sprite_mode;

static void apply_sprite_mode(void)
{
    if (sprite_mode == 1) {
        lv_obj_set_size(gif, 44, 44);
        lv_image_set_scale(gif, 44 * 256 / 96);
        lv_obj_align(gif, LV_ALIGN_TOP_LEFT, 4, 4);
    } else if (sprite_mode == 2) {
        lv_obj_set_size(gif, 96, 96);
        lv_image_set_scale(gif, 256);
        lv_obj_align(gif, LV_ALIGN_TOP_LEFT, 4, 40);
    } else {
        lv_obj_set_size(gif, GIF_SIZE, GIF_SIZE);
        lv_image_set_scale(gif, GIF_SIZE * 256 / 96);
        lv_obj_center(gif);
    }
}

/* Never call lv_gif_set_src: if the new draw buffer fails to allocate the
 * widget is left with draw_buf NULL and a live frame timer, which crashes
 * the LVGL task on the next tick. A fresh object carries a fresh paused
 * timer, so a failed init just leaves the sprite absent instead. */
static void recreate_gif(const void *dsc)
{
    lv_obj_t *parent = lv_obj_get_parent(gif);
    lv_obj_delete(gif);
    gif = lv_gif_create(parent);
    lv_gif_set_color_format(gif, LV_COLOR_FORMAT_ARGB8888);
    lv_gif_set_src(gif, dsc);
    apply_sprite_mode();
}

static void back_to_idle(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    idle_timer = NULL;
    if (current_animation != GEMMI_ANIM_IDLE) {
        recreate_gif(gemmi_anims[GEMMI_ANIM_IDLE].dsc);
        current_animation = GEMMI_ANIM_IDLE;
    }
}

static int64_t last_swap_us;

/* Defer the swap to the LVGL timer loop instead of running it inside an
 * input-event handler. */
static void swap_animation(void *dsc)
{
    recreate_gif(dsc);
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

static void set_bubble(const char *headline, const char *message, const char *queue)
{
    char compact_message[MAX_BUBBLE_TEXT + 1];
    trim_for_bubble(message, compact_message, sizeof(compact_message));
    lv_label_set_text(headline_label, headline);
    lv_label_set_text(message_label, compact_message);
    lv_label_set_text(queue_label, queue);
}

static void set_status(const char *status)
{
    lv_label_set_text(status_label, status);
}

static void set_dock_caption(uint32_t slot, const char *caption)
{
    if (slot >= 3) return;
    lv_label_set_text(dock_labels[slot], caption);
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
            lv_obj_set_style_text_color(dock_labels[index], lv_color_hex(0x5AC8FF), 0);
        } else {
            lv_obj_remove_state(dock_buttons[index], LV_STATE_CHECKED);
            lv_obj_set_style_text_color(dock_labels[index], lv_color_hex(0x111111), 0);
        }
    }
}

static void show_nav_dock(void)
{
    select_choice(-1);
    set_dock_caption(0, "agents");
    set_dock_caption(1, "asks");
    set_dock_caption(2, "view");
}

static void set_sprite_small(void)
{
    /* Badge in the corner of the list view, out of the rows' way. */
    sprite_mode = 1;
    apply_sprite_mode();
}

static void enter_agents_list(void)
{
    agents_mode = 1;
    detail_agent = -1;
    set_status(agent_feed_count ? "gemmi: pick an agent" : "gemmi sees no agents");
    lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(log_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(musing_label, LV_OBJ_FLAG_HIDDEN);
    set_sprite_small();
    for (uint32_t row = 0; row < AGENT_ROWS; row++) {
        int agent = agents_page * AGENT_ROWS + (int)row;
        if (agent < agent_feed_count) {
            char caption[40];
            snprintf(caption, sizeof(caption), "%d %.24s", agent + 1,
                     agent_feed[agent].agent);
            lv_label_set_text(lv_obj_get_child(agent_rows[row], 0), caption);
            lv_obj_remove_flag(agent_rows[row], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(agent_rows[row], LV_OBJ_FLAG_HIDDEN);
        }
    }
    set_dock_caption(0, "back");
    set_dock_caption(1, "");
    set_dock_caption(2, agent_feed_count > AGENT_ROWS ? "more" : "");
}

static void show_agent_detail(void)
{
    agents_mode = 2;
    const gemmi_agent_state_t *entry = &agent_feed[detail_agent];
    char status[80];
    snprintf(status, sizeof(status), "watching %s · %s", entry->agent, entry->state);
    set_status(status);
    lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_remove_flag(log_panel, LV_OBJ_FLAG_HIDDEN);
    set_dock_caption(0, "back");
    set_dock_caption(1, "next");
    set_dock_caption(2, "");
}

static void set_sprite_home(void)
{
    sprite_mode = 0;
    apply_sprite_mode();
}

static void show_home(void)
{
    ask_foreground = false;
    agents_mode = 0;
    lv_obj_add_flag(log_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(musing_label, LV_OBJ_FLAG_HIDDEN);
    for (uint32_t slot = 0; slot < AGENT_ROWS; slot++) {
        lv_obj_add_flag(agent_rows[slot], LV_OBJ_FLAG_HIDDEN);
    }
    agents_page = 0;
    set_sprite_home();
    if (ask_pending) {
        set_status("gemmi needs an answer");
        set_bubble("ask waiting", pending_ask.message, "1 ask");
        lv_obj_remove_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_status("gemmi is idle");
        lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
    }
    show_nav_dock();
    play_state("idle", false);
}

static void enter_ask(void)
{
    ask_foreground = true;
    char status[80];
    snprintf(status, sizeof(status), "answer %s", pending_ask.agent);
    set_status(status);
    lv_obj_remove_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
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

/* Leaving an ask without answering consumes it on the bridge; it stays open
 * for the agent and can be reopened from the asks slot. */
static void dismiss_ask(void)
{
    gemmi_bridge_answer(pending_ask.id, NULL);
    show_home();
}

static void send_choice(int slot)
{
    const char *choice = pending_ask.choices[slot];
    gemmi_bridge_answer(pending_ask.id, choice);
    ask_pending = false;
    ask_foreground = false;
    char status[80];
    snprintf(status, sizeof(status), "gemmi sent: %s", choice);
    set_status(status);
    lv_obj_remove_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
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
            set_status("gemmi checked - no asks");
            lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
            play_state("thinking", true);
        }
        break;
    case GEMMI_ACTION_VIEW:
        if (have_artifact) {
            char status[80];
            snprintf(status, sizeof(status), "gemmi is viewing %.40s",
                     last_artifact.artifact[0] ? last_artifact.artifact : last_artifact.agent);
            set_status(status);
            lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            set_status("gemmi found no artifact");
            lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
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
    if (ask_foreground) {
        dismiss_ask();
    } else if (agents_mode == 2) {
        enter_agents_list();
    } else if (agents_mode == 1) {
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
    lv_obj_set_style_border_color(button, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    /* While held the button is grey so the caption stays readable; a selected
     * (checked) choice is solid black. Label colour for checked is driven in
     * select_choice because state styles never reach child labels. */
    lv_obj_set_style_bg_color(button, lv_color_hex(0xBBBBBB), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x111111), LV_STATE_CHECKED);
    /* Resistive touch often loses the release position, and CLICKED needs a
     * clean press+release inside the widget. PRESSED fires on touch-down. */
    lv_obj_add_event_cb(button, on_dock, LV_EVENT_PRESSED, (void *)(uintptr_t)slot);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_hex(0x111111), 0);
    make_label_upright(label, width - 6, height - 6);
    lv_obj_center(label);
    return label;
}

void gemmi_ui_init(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Flat black-and-white UI keeps the panel clean; Gemmi is the only colour. */
    lv_obj_t *scene = lv_obj_create(screen);
    lv_obj_set_pos(scene, 7, TOP_H + 3);
    lv_obj_set_size(scene, SCREEN_W - 14, SCREEN_H - TOP_H - DOCK_H - 5);
    lv_obj_set_style_bg_color(scene, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(scene, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(scene, 1, 0);
    lv_obj_set_style_radius(scene, 0, 0);
    lv_obj_set_style_pad_all(scene, 0, 0);
    lv_obj_set_scrollable(scene, false);

    /* Agent list rows, one tappable numbered row per active agent. They sit
     * below the sprite badge so nothing overlaps. */
    for (int slot = 0; slot < AGENT_ROWS; slot++) {
        agent_rows[slot] = lv_button_create(scene);
        lv_obj_set_pos(agent_rows[slot], 10, 52 + slot * 38);
        lv_obj_set_size(agent_rows[slot], SCENE_W - 20, 32);
        lv_obj_set_style_bg_color(agent_rows[slot], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(agent_rows[slot], lv_color_hex(0xBBBBBB), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(agent_rows[slot], lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_width(agent_rows[slot], 1, 0);
        lv_obj_set_style_radius(agent_rows[slot], 8, 0);
        lv_obj_t *label = lv_label_create(agent_rows[slot]);
        lv_obj_set_style_text_color(label, lv_color_hex(0x111111), 0);
        lv_obj_center(label);
        lv_obj_add_flag(agent_rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(agent_rows[slot], on_agent_row, LV_EVENT_PRESSED,
                            (void *)(uintptr_t)slot);
    }

    /* Per-agent log view on the right of the scene while drilled in; the
     * sprite shrinks to the left beside a thought caption. */
    log_panel = lv_obj_create(scene);
    lv_obj_set_pos(log_panel, 104, 8);
    lv_obj_set_size(log_panel, SCENE_W - 108, SCENE_H - 16);
    lv_obj_set_style_bg_color(log_panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(log_panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(log_panel, 1, 0);
    lv_obj_set_style_radius(log_panel, 0, 0);
    lv_obj_set_style_pad_all(log_panel, 6, 0);
    lv_obj_set_scrollable(log_panel, false);
    lv_obj_add_flag(log_panel, LV_OBJ_FLAG_HIDDEN);
    log_label = lv_label_create(log_panel);
    lv_obj_set_size(log_label, SCENE_W - 120, SCENE_H - 28);
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_CLIP);
    lv_label_set_recolor(log_label, true);
    lv_obj_set_style_text_color(log_label, lv_color_hex(0x222222), 0);

    musing_label = lv_label_create(scene);
    lv_obj_set_pos(musing_label, 4, 140);
    lv_obj_set_size(musing_label, 96, 96);
    lv_label_set_long_mode(musing_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(musing_label, lv_color_hex(0x222222), 0);
    lv_obj_add_flag(musing_label, LV_OBJ_FLAG_HIDDEN);

    gif = lv_gif_create(scene);
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
    lv_obj_set_style_border_color(bubble, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_radius(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 0, 0);
    lv_obj_set_scrollable(bubble, false);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bubble, on_bubble, LV_EVENT_PRESSED, NULL);

    headline_label = lv_label_create(bubble);
    lv_obj_add_flag(headline_label, LV_OBJ_FLAG_HIDDEN);

    message_label = lv_label_create(bubble);
    lv_obj_set_pos(message_label, 5, 3);
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(message_label, lv_color_hex(0x444444), 0);
    make_label_upright(message_label, BUBBLE_W - 10, 20);

    queue_label = lv_label_create(screen);
    /* Keep the character's full 2x sprite unobstructed. The queue state is
     * represented by the top status and three fixed bottom controls instead. */
    lv_obj_add_flag(queue_label, LV_OBJ_FLAG_HIDDEN);

    status_label = lv_label_create(screen);
    lv_obj_set_pos(status_label, 8, 3);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x333333), 0);
    make_label_upright(status_label, SCREEN_W - 16, 24);

    for (uint32_t index = 0; index < 3; index++) {
        dock_labels[index] = create_button(screen, 5 + (int16_t)index * 78, SCREEN_H - 35,
                                           74, 30, (int)index);
        dock_buttons[index] = lv_obj_get_parent(dock_labels[index]);
    }
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
            lv_obj_remove_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
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
        snprintf(status, sizeof(status), "watching %s · %s", event->agent, event->state);
    }
    set_status(status);
    if (!ask_foreground && !ask_pending) {
        char headline[80];
        snprintf(headline, sizeof(headline), "%s · %s", event->agent, event->state);
        lv_obj_remove_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
        set_bubble(headline, event->message, "attention needed");
    }
    play_state(event->state, false);
}
