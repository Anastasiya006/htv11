#ifndef LV_CONF_H
#define LV_CONF_H

/* Match the firmware's software renderer where it matters, while SDL owns
 * presentation and pointer input on the desktop. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 1
#define LV_USE_GIF 1

#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH "SDL2/SDL.h"
#define LV_SDL_AUTO_BACKEND 1
#define LV_SDL_BACKEND LV_SDL_BACKEND_SW
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 2
#define LV_SDL_ACCELERATED 0
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
