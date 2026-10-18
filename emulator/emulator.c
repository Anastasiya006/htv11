/* Desktop renderer for Gemmi's firmware UI.
 *
 * This binary links gemmi_ui.c and gemmi_anims.c directly.  It replaces only
 * the LCD/touch driver and HTTP bridge with SDL and small local fixtures.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include "gemmi_bridge.h"
#include "gemmi_ui.h"
#include "gemmi_pet.h"
#include "lvgl.h"

#ifdef GEMMI_SYSTEM_MENLO
#define GEMMI_RENDER_SCALE 4
#else
#define GEMMI_RENDER_SCALE 1
#endif

#define SCREEN_W 240
#define SCREEN_H 320

#ifdef __APPLE__
SDL_Window *gemmi_sdl_create_window(const char *title, int x, int y,
                                   int width, int height, Uint32 flags)
{
    return SDL_CreateWindow(title, x, y, width, height, flags | SDL_WINDOW_ALLOW_HIGHDPI);
}
#endif

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

static void post_ask(bool three_choices)
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
    event.choice_count = three_choices ? 3 : 2;
    if (three_choices) copy_text(event.choices[2], GEMMI_CHOICE_LEN, "deny");
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
    if (strcmp(scene, "level-up") == 0) {
        gemmi_pet_test_level_ready();
        post_event("status", "demo", "working", "Finishing a task", "");
        post_event("status", "demo", "success", "Task complete", "");
        return;
    }
    if (strcmp(scene, "reward") == 0) {
        post_event("status", "demo", "working", "Finishing a task", "");
        post_event("status", "demo", "success", "Task complete", "");
        return;
    }
    if (strcmp(scene, "pet") == 0) { gemmi_ui_open_pet(); return; }
    if (strcmp(scene, "thinking") == 0) {
        post_event("status", "codex", "thinking", "planning the layout", "");
    } else if (strcmp(scene, "working") == 0) {
        post_event("status", "codex", "working", "editing gemmi_ui.c", "");
    } else if (strcmp(scene, "error") == 0) {
        post_event("status", "demo", "error", "An agent needs your help", "");
    } else if (strcmp(scene, "ask") == 0) {
        post_ask(false);
    } else if (strcmp(scene, "ask-three") == 0) {
        post_ask(true);
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
    case SDLK_7:
        gemmi_pet_test_level_ready();
        post_event("status", "demo", "working", "Finishing a task", "");
        post_event("status", "demo", "success", "Task complete", "");
        break;
    case SDLK_6:
        post_event("status", "demo", "working", "Finishing a task", "");
        post_event("status", "demo", "success", "Task complete", "");
        break;
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

esp_err_t gemmi_bridge_post_raw(const char *path, const char *body)
{
    printf("post path=%s bytes=%zu\n", path ? path : "", body ? strlen(body) : 0U);
    return ESP_OK;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--scene home|thinking|working|error|ask|artifact|agents|offline|pet|reward|level-up]\n"
           "Hotkeys: 1 thinking, 2 working, 3 ask, 4 artifact, 5 agents, 6 demo task reward, 7 demo level-up, O offline, I online\nTesting: --test-gems amount\nCapture: --capture path.bmp [--tap x,y ...] (logical coordinates)\n",
           program);
}

int main(int argc, char **argv)
{
    const char *scene = "agents";
    const char *capture = NULL;
    unsigned test_gems = 0;
    bool set_test_gems = false;
    int tap_x[8], tap_y[8], tap_count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--test-gems") && i + 1 < argc) {
            if (sscanf(argv[++i], "%u", &test_gems) != 1 || test_gems > 1000000) return 2;
            set_test_gems = true;
        }
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--tap") && i + 1 < argc && tap_count < 8) {
            if (sscanf(argv[++i], "%d,%d", &tap_x[tap_count], &tap_y[tap_count]) != 2) return 2;
            tap_count++;
        } else { print_usage(argv[0]); return !strcmp(argv[i], "--help") ? 0 : 2; }
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    lv_init();
    lv_display_t *display = lv_sdl_window_create(SCREEN_W * GEMMI_RENDER_SCALE, SCREEN_H * GEMMI_RENDER_SCALE);
    if (display == NULL) {
        fprintf(stderr, "Unable to create the SDL display.\n");
        return 1;
    }
    lv_sdl_window_set_title(display, "Gemmi firmware UI emulator");
    lv_sdl_window_set_zoom(display, 2.0f / GEMMI_RENDER_SCALE);
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, display);
    SDL_AddEventWatch(hotkey, NULL);

    if (set_test_gems) {
        gemmi_pet_init();
        gemmi_pet_test_gems(test_gems);
        if (!gemmi_pet_save_ok()) { fprintf(stderr, "Could not save test gems.\n"); return 1; }
    }
    gemmi_ui_init(display);
    show_scene(scene);

    uint32_t started = SDL_GetTicks();
    int tap_index = 0;
    bool tap_down = false;
    while (true) {
        lv_timer_handler();
        uint32_t elapsed = SDL_GetTicks() - started;
        if (tap_index < tap_count && elapsed >= 500 + (uint32_t)tap_index * 350 + (tap_down ? 100 : 0)) {
            SDL_Event event;
            memset(&event, 0, sizeof(event));
            event.type = tap_down ? SDL_MOUSEBUTTONUP : SDL_MOUSEBUTTONDOWN;
            event.button.windowID = SDL_GetWindowID(lv_sdl_window_get_window(display));
            event.button.button = SDL_BUTTON_LEFT;
            event.button.x = tap_x[tap_index] * 2;
            event.button.y = tap_y[tap_index] * 2;
            SDL_PushEvent(&event);
            if (tap_down) tap_index++;
            tap_down = !tap_down;
        }
        if (capture && elapsed > 1000 + (uint32_t)tap_count * 350) {
            SDL_Renderer *renderer = lv_sdl_window_get_renderer(display);
            int width, height;
            SDL_GetRendererOutputSize(renderer, &width, &height);
            SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
            int result = surface ? SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) : -1;
            if (result == 0) result = SDL_SaveBMP(surface, capture);
            if (surface) SDL_FreeSurface(surface);
            if (result) fprintf(stderr, "Capture failed: %s\n", SDL_GetError());
            lv_sdl_quit();
            return result ? 1 : 0;
        }
        SDL_Delay(5);
    }
}
