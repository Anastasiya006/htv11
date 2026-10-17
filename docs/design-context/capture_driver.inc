/* Capture-only instrumentation, compiled into a temporary emulator launcher.
 * The firmware UI and animation objects remain the normal build artifacts. */
#include <stdlib.h>

typedef struct { unsigned at; char kind[32]; int x, y; char value[1024]; } context_action_t;
static context_action_t context_actions[256];
static unsigned context_count, context_index;
static FILE *context_stream;
static unsigned context_duration, context_next_frame;
static unsigned char *context_pixels;
static int context_width, context_height;
static SDL_Renderer *context_renderer;
static int context_failed;
static void context_init(lv_display_t *display)
{
    const char *pipe_path = getenv("GEMMI_CONTEXT_PIPE");
    const char *script = getenv("GEMMI_CONTEXT_SCRIPT");
    if (!pipe_path || !script) return;
    FILE *file = fopen(script, "r");
    if (!file) exit(2);
    char line[1200];
    while (fgets(line, sizeof(line), file)) {
        if (context_count >= 256) exit(2);
        context_action_t *a = &context_actions[context_count];
        if (sscanf(line, "%u %31s %d %d %1023s", &a->at, a->kind, &a->x, &a->y, a->value) == 5) context_count++;
    }
    fclose(file);
    context_duration = (unsigned)atoi(getenv("GEMMI_CONTEXT_DURATION"));
    context_renderer = lv_sdl_window_get_renderer(display);
    SDL_GetRendererOutputSize(context_renderer, &context_width, &context_height);
    if (context_width != 960 || context_height != 1280) {
        fprintf(stderr, "Capture expects Retina 960x1280; got %dx%d\n",context_width,context_height); exit(2);
    }
    context_pixels = malloc((size_t)context_width * context_height * 3);
    context_stream = fopen(pipe_path, "wb");
    if (!context_stream || !context_pixels) exit(2);
}
static void context_shot(const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0,context_width,context_height,32,SDL_PIXELFORMAT_ARGB8888);
    if (!s || SDL_RenderReadPixels(context_renderer,NULL,SDL_PIXELFORMAT_ARGB8888,s->pixels,s->pitch) || SDL_SaveBMP(s,path)) context_failed=1;
    if (s) SDL_FreeSurface(s);
}
static int context_tick(lv_display_t *display, unsigned elapsed)
{
    if (!context_stream) return 0;
    while (context_index < context_count && elapsed >= context_actions[context_index].at) {
        context_action_t *a = &context_actions[context_index++];
        if (!strcmp(a->kind,"shot")) { context_shot(a->value); continue; }
        SDL_Event event;
        memset(&event,0,sizeof(event));
        if (!strcmp(a->kind,"key")) {
            event.type = SDL_KEYDOWN;
            event.key.windowID = SDL_GetWindowID(lv_sdl_window_get_window(display));
            event.key.keysym.sym = SDL_GetKeyFromName(a->value);
        } else {
            event.type = !strcmp(a->kind,"down") ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            event.button.windowID = SDL_GetWindowID(lv_sdl_window_get_window(display));
            event.button.button = SDL_BUTTON_LEFT;
            event.button.x = a->x * 2; event.button.y = a->y * 2;
        }
        SDL_PushEvent(&event);
    }
    while (elapsed >= context_next_frame && context_next_frame < context_duration) {
        size_t bytes = (size_t)context_width * context_height * 3;
        if (SDL_RenderReadPixels(context_renderer,NULL,SDL_PIXELFORMAT_RGB24,context_pixels,context_width*3) ||
            fwrite(context_pixels,1,bytes,context_stream) != bytes) { context_failed=1; return 1; }
        context_next_frame += 50;
    }
    return elapsed >= context_duration;
}
static int context_finish(void)
{
    if (context_stream && fclose(context_stream)) context_failed=1;
    free(context_pixels);
    return context_failed;
}
