#include "gemmi_pet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

const gemmi_pet_item_t gemmi_pet_items[] = {
    {"Witch hat", "A little spellwork.", 12},
    {"Pumpkin beanie", "Warm autumn circuits.", 12},
    {"Bat bow tie", "A tiny night flight.", 9},
    {"Candy bandana", "Sweet and ready.", 9},
    {"Ghost cookie", "A spooky snack.", 4},
    {"Candy corn", "A bright little treat.", 3},
};
/* Versioned, fixed-size save; event high-water marks survive restarts. */
typedef struct { char name[33]; int32_t last_id; uint32_t active; } agent_t;
typedef struct {
    uint32_t version;
    gemmi_pet_stats_t stats;
    agent_t agents[8];
    int64_t care_at[3];
} save_t;
static save_t pet;
static bool save_ok = true;
static uint32_t clamp(uint32_t n) { return n > 100 ? 100 : n; }
#ifndef ESP_PLATFORM
static const char *save_path(void) {
    const char *path = getenv("GEMMI_PET_SAVE");
    return path && *path ? path : ".gemmi-pet-state";
}
#endif
static void persist(void) {
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    save_ok = nvs_open("gemmi_pet", NVS_READWRITE, &handle) == ESP_OK;
    if (!save_ok) return;
    save_ok = nvs_set_blob(handle, "state", &pet, sizeof(pet)) == ESP_OK;
    if (save_ok) save_ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
#else
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", save_path()) >= (int)sizeof(temporary)) { save_ok = false; return; }
    FILE *file = fopen(temporary, "wb");
    if (!file) { save_ok = false; return; }
    save_ok = fwrite(&pet, sizeof(pet), 1, file) == 1;
    if (fclose(file)) save_ok = false;
    if (save_ok) save_ok = rename(temporary, save_path()) == 0;
    if (!save_ok) remove(temporary);
#endif
}
void gemmi_pet_init(void) {
    memset(&pet, 0, sizeof(pet));
    save_ok = true;
    bool loaded = false;
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open("gemmi_pet", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(pet);
        loaded = nvs_get_blob(handle, "state", &pet, &size) == ESP_OK && size == sizeof(pet);
        nvs_close(handle);
    }
#else
    FILE *file = fopen(save_path(), "rb");
    if (file) { loaded = fread(&pet, sizeof(pet), 1, file) == 1; fclose(file); }
#endif
    if (!loaded || pet.version != 1 || pet.stats.energy > 100 || pet.stats.mood > 100 ||
        pet.stats.equipped > GEMMI_PET_ITEM_COUNT || pet.stats.owned >= (1u << GEMMI_PET_ITEM_COUNT) ||
        (pet.stats.equipped && !(pet.stats.owned & (1u << (pet.stats.equipped - 1))))) {
        memset(&pet, 0, sizeof(pet));
        pet.version = 1;
        pet.stats.gems = 20;
        pet.stats.energy = pet.stats.mood = 80;
    }
    for (unsigned i = 0; i < 8; i++) pet.agents[i].name[32] = 0;
}
const gemmi_pet_stats_t *gemmi_pet_stats(void) { return &pet.stats; }
uint32_t gemmi_pet_level(void) { return 1 + pet.stats.xp / 100; }
bool gemmi_pet_save_ok(void) { return save_ok; }
bool gemmi_pet_event(const char *agent, const char *state, int id) {
    if (!agent || !*agent || !state) return false;
    agent_t *entry = NULL;
    for (unsigned i = 0; i < 8; i++) if (!strcmp(pet.agents[i].name, agent)) { entry = &pet.agents[i]; break; }
    if (!entry) for (unsigned i = 0; i < 8; i++) if (!pet.agents[i].name[0]) {
        entry = &pet.agents[i]; snprintf(entry->name, sizeof(entry->name), "%s", agent); break;
    }
    if (!entry) return false;
    bool done = !strcmp(state, "success") || !strcmp(state, "done") || !strcmp(state, "completed");
    if (!done) {
        if (!strcmp(state, "thinking") || !strcmp(state, "working") || !strcmp(state, "reading")) entry->active = 1;
        else if (!strcmp(state, "error") || !strcmp(state, "failed") || !strcmp(state, "idle")) entry->active = 0;
        return false;
    }
    if (id > 0 ? id <= entry->last_id : !entry->active) return false;
    entry->active = 0;
    if (id > 0) entry->last_id = id;
    uint32_t previous_level = gemmi_pet_level();
    pet.stats.xp += 25;
    pet.stats.gems += 5;
    pet.stats.gems += (gemmi_pet_level() - previous_level) * 10;
    pet.stats.tasks++;
    pet.stats.energy = pet.stats.energy > 8 ? pet.stats.energy - 8 : 0;
    pet.stats.mood = clamp(pet.stats.mood + 2);
    persist();
    return true;
}
const char *gemmi_pet_care(unsigned action) {
    if (action > 2) return "Unknown action.";
    int64_t now = (int64_t)time(NULL);
    int64_t wait = action == 0 ? 10 : 60;
    if (pet.care_at[action] && now - pet.care_at[action] < wait) return "A little moment to enjoy that.";
    if (action == 0) {
        if (pet.stats.mood == 100) return "Already feeling very loved.";
        pet.stats.mood = clamp(pet.stats.mood + 12);
    } else {
        if (pet.stats.energy == 100) return "Already full of energy.";
        if (action == 1 && pet.stats.gems < 3) return "A snack costs 3 gems.";
        if (action == 1) pet.stats.gems -= 3;
        pet.stats.energy = clamp(pet.stats.energy + (action == 1 ? 25 : 15));
    }
    pet.care_at[action] = now;
    persist();
    return action == 0 ? "That made Gemmi happy!" : action == 1 ? "Crunch! Energy restored." : "A little breather. Energy +15.";
}
const char *gemmi_pet_shop(unsigned item) {
    if (item >= GEMMI_PET_ITEM_COUNT) return "Unknown item.";
    uint32_t bit = 1u << item;
    if (!(pet.stats.owned & bit)) {
        if (pet.stats.gems < gemmi_pet_items[item].cost) return "Save a few more gems first.";
        pet.stats.gems -= gemmi_pet_items[item].cost;
        pet.stats.owned |= bit;
    }
    pet.stats.equipped = pet.stats.equipped == item + 1 ? 0 : item + 1;
    persist();
    return pet.stats.equipped ? "Equipped! Looking good." : "Back to the original look.";
}

#ifndef ESP_PLATFORM
void gemmi_pet_test_gems(uint32_t amount) { pet.stats.gems = amount; persist(); }
void gemmi_pet_test_level_ready(void) {
    pet.stats.xp = (gemmi_pet_level() - 1) * 100 + 75;
    persist();
}
#endif
