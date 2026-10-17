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
#define AGENT_FEED_LEN 4

typedef enum {
    GEMMI_ACTION_HOME = 1,
    GEMMI_ACTION_INBOX,
    GEMMI_ACTION_AGENTS,
    GEMMI_ACTION_ASKS,
    GEMMI_ACTION_VIEW,
} gemmi_action_t;

typedef struct {
    char agent[33];
    char state[33];
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
static int agent_feed_cursor;

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

static void back_to_idle(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    idle_timer = NULL;
    if (current_animation != GEMMI_ANIM_IDLE) {
        lv_gif_set_src(gif, gemmi_anims[GEMMI_ANIM_IDLE].dsc);
        current_animation = GEMMI_ANIM_IDLE;
    }
}

#define SCENE_W 226
#define SCENE_H 238
#define GIF_SIZE 196

static int64_t last_swap_us;

/* lv_gif_set_src tears down and rebuilds the ARGB decoder; doing that inside
 * an input-event handler can hit the decoder while a draw references it.
 * Defer the swap to the LVGL timer loop instead. */
static void swap_animation(void *dsc)
{
    lv_gif_set_src(gif, dsc);
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

static void show_home(void)
{
    ask_foreground = false;
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
    switch ((gemmi_action_t)(GEMMI_ACTION_AGENTS + slot)) {
    case GEMMI_ACTION_AGENTS:
        if (agent_feed_count == 0) {
            set_status("gemmi sees no agents");
            lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            const gemmi_agent_state_t *entry =
                &agent_feed[(agent_feed_cursor + agent_feed_count - 1) % agent_feed_count];
            char status[80];
            snprintf(status, sizeof(status), "watching %s · %s", entry->agent, entry->state);
            set_status(status);
            lv_obj_add_flag(bubble_obj, LV_OBJ_FLAG_HIDDEN);
            agent_feed_cursor = (agent_feed_cursor + 1) % agent_feed_count;
        }
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

static void on_bubble(lv_event_t *event)
{
    LV_UNUSED(event);
    if (ask_foreground) dismiss_ask();
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

    gif = lv_gif_create(scene);
    lv_gif_set_color_format(gif, LV_COLOR_FORMAT_ARGB8888);
    lv_gif_set_src(gif, gemmi_anims[GEMMI_ANIM_IDLE].dsc);
    lv_obj_set_size(gif, GIF_SIZE, GIF_SIZE);
    lv_obj_center(gif);

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
    for (int index = 0; index < agent_feed_count; index++) {
        if (strcmp(agent_feed[index].agent, event->agent) == 0) {
            strlcpy(agent_feed[index].state, event->state, sizeof(agent_feed[index].state));
            return;
        }
    }
    if (agent_feed_count < AGENT_FEED_LEN) {
        gemmi_agent_state_t *entry = &agent_feed[agent_feed_count++];
        strlcpy(entry->agent, event->agent, sizeof(entry->agent));
        strlcpy(entry->state, event->state, sizeof(entry->state));
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
