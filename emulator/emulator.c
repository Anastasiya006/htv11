/* Desktop renderer for Gemmi's firmware UI.
 *
 * This binary links gemmi_ui.c and gemmi_anims.c directly.  It replaces only
 * the LCD/touch driver and HTTP bridge with SDL and small local fixtures.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include "gemmi_bridge.h"
#include "gemmi_ui.h"
#include "lvgl.h"

#define SCREEN_W 240
#define SCREEN_H 320

static void copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0) return;
    snprintf(destination, destination_size, "%s", source ? source : "");
}

static void post_event(const char *kind, const char *agent, const char *state,
                       const char *message, const char *artifact)
{
    gemmi_event_t event;
    memset(&event, 0, sizeof(event));
    copy_text(event.kind, sizeof(event.kind), kind);
    copy_text(event.agent, sizeof(event.agent), agent);
    copy_text(event.state, sizeof(event.state), state);
    copy_text(event.message, sizeof(event.message), message);
    copy_text(event.artifact, sizeof(event.artifact), artifact);
    gemmi_ui_show_event(&event);
}

static void post_ask(void)
{
    gemmi_event_t event;
    memset(&event, 0, sizeof(event));
    event.id = 1;
    copy_text(event.kind, sizeof(event.kind), "ask");
    copy_text(event.agent, sizeof(event.agent), "codex");
    copy_text(event.state, sizeof(event.state), "needs-attention");
    copy_text(event.message, sizeof(event.message), "Run the visual checks?");
    copy_text(event.choices[0], GEMMI_CHOICE_LEN, "allow");
    copy_text(event.choices[1], GEMMI_CHOICE_LEN, "review");
    event.choice_count = 2;
    gemmi_ui_show_event(&event);
}

static void seed_agents(void)
{
    post_event("log", "codex", "thinking", "think: mapping the UI", "");
    post_event("log", "codex", "working", "tool: rg gemmi_ui", "");
    post_event("log", "reviewer", "reading", "ran: checking layout", "");
    post_event("log", "reviewer", "working", "think: compare frames", "");
}

static void show_scene(const char *scene)
{
    if (strcmp(scene, "home") == 0) return;
    if (strcmp(scene, "thinking") == 0) {
        post_event("status", "codex", "thinking", "planning the layout", "");
    } else if (strcmp(scene, "working") == 0) {
        post_event("status", "codex", "working", "editing gemmi_ui.c", "");
    } else if (strcmp(scene, "ask") == 0) {
        post_ask();
    } else if (strcmp(scene, "artifact") == 0) {
        post_event("artifact", "codex", "reading", "", "screenshots/layout.png");
    } else if (strcmp(scene, "agents") == 0) {
        seed_agents();
    } else if (strcmp(scene, "offline") == 0) {
        gemmi_ui_offline(true);
    } else {
        fprintf(stderr, "Unknown scene: %s\n", scene);
    }
}

static int hotkey(void *user_data, SDL_Event *event)
{
    (void)user_data;
    if (event->type != SDL_KEYDOWN || event->key.repeat) return 0;

    switch (event->key.keysym.sym) {
    case SDLK_1: show_scene("thinking"); break;
    case SDLK_2: show_scene("working"); break;
    case SDLK_3: show_scene("ask"); break;
    case SDLK_4: show_scene("artifact"); break;
    case SDLK_5: show_scene("agents"); break;
    case SDLK_o: gemmi_ui_offline(true); break;
    case SDLK_i: gemmi_ui_offline(false); break;
    default: break;
    }
    return 0;
}

/* The shared UI calls this after a device choice. The emulator has no bridge;
 * keeping the response visible in stdout makes button flows easy to verify. */
void gemmi_bridge_answer(int event_id, const char *choice)
{
    printf("answer event=%d choice=%s\n", event_id, choice ? choice : "(dismissed)");
    fflush(stdout);
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--scene home|thinking|working|ask|artifact|agents|offline]\n"
           "Hotkeys: 1 thinking, 2 working, 3 ask, 4 artifact, 5 agents, O offline, I online\n",
           program);
}

int main(int argc, char **argv)
{
    const char *scene = "agents";
    if (argc == 3 && strcmp(argv[1], "--scene") == 0) {
        scene = argv[2];
    } else if (argc != 1) {
        print_usage(argv[0]);
        return argc == 2 && strcmp(argv[1], "--help") == 0 ? 0 : 2;
    }

    lv_init();
    lv_display_t *display = lv_sdl_window_create(SCREEN_W, SCREEN_H);
    if (display == NULL) {
        fprintf(stderr, "Unable to create the SDL display.\n");
        return 1;
    }
    lv_sdl_window_set_title(display, "Gemmi firmware UI emulator");
    lv_sdl_window_set_zoom(display, 2.0f);
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, display);
    SDL_AddEventWatch(hotkey, NULL);

    gemmi_ui_init(display);
    show_scene(scene);

    while (true) {
        lv_timer_handler();
        SDL_Delay(5);
    }
}
