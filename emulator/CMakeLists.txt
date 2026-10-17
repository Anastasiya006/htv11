cmake_minimum_required(VERSION 3.20)

project(gemmi_emulator LANGUAGES C)

# Build the exact LVGL revision the ESP-IDF project has already resolved. The
# emulator deliberately does not fetch its own copy, so rendering changes are
# exercised by the same UI source and LVGL version as the device.
set(GEMMI_FIRMWARE_DIR "${CMAKE_CURRENT_LIST_DIR}/../gemware/gemware")
set(GEMMI_MAIN_DIR "${GEMMI_FIRMWARE_DIR}/main")
set(GEMMI_LVGL_DIR "${GEMMI_FIRMWARE_DIR}/managed_components/lvgl__lvgl")

if(NOT EXISTS "${GEMMI_LVGL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "The pinned LVGL component is missing. Run idf.py reconfigure in gemware/gemware first.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE FILEPATH "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_SET_CONFIG_OPTS ON CACHE BOOL "" FORCE)
set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)

add_subdirectory("${GEMMI_LVGL_DIR}" "${CMAKE_BINARY_DIR}/lvgl")

add_executable(gemmi-emulator
    emulator.c
    "${GEMMI_MAIN_DIR}/gemmi_ui.c"
    "${GEMMI_MAIN_DIR}/gemmi_anims.c"
)

target_include_directories(gemmi-emulator PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/compat"
    "${GEMMI_MAIN_DIR}"
)
find_package(SDL2 REQUIRED)
target_link_libraries(gemmi-emulator PRIVATE lvgl::lvgl SDL2::SDL2)

# Match the HTML simulator's macOS monospace font without distributing Menlo.
if(APPLE AND EXISTS "/System/Library/Fonts/Menlo.ttc")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GEMMI_FONT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/gemmi_menlo.c")
    add_custom_command(OUTPUT "${GEMMI_FONT_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_font.py"
                "/System/Library/Fonts/Menlo.ttc" "${GEMMI_FONT_SOURCE}"
        DEPENDS generate_font.py VERBATIM)
    target_sources(gemmi-emulator PRIVATE "${GEMMI_FONT_SOURCE}")
    target_compile_definitions(gemmi-emulator PRIVATE GEMMI_SYSTEM_MENLO=1)
    # SDL's LVGL driver omits the Retina flag. Add it at window creation.
    target_compile_definitions(lvgl PRIVATE SDL_CreateWindow=gemmi_sdl_create_window)
endif()

