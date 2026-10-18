#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t xp, gems, tasks, owned, equipped;
    uint32_t energy, mood;
} gemmi_pet_stats_t;
typedef struct { const char *name; const char *description; uint32_t cost; } gemmi_pet_item_t;
enum { GEMMI_PET_GLASSES, GEMMI_PET_CROWN, GEMMI_PET_HALO, GEMMI_PET_SCARF, GEMMI_PET_HORNS, GEMMI_PET_BOW, GEMMI_PET_ITEM_COUNT };
extern const gemmi_pet_item_t gemmi_pet_items[GEMMI_PET_ITEM_COUNT];
void gemmi_pet_init(void);
const gemmi_pet_stats_t *gemmi_pet_stats(void);
uint32_t gemmi_pet_level(void);
/* Only a terminal task status earns rewards; repeated events are ignored. */
bool gemmi_pet_event(const char *agent, const char *state, int event_id);
const char *gemmi_pet_care(unsigned action); /* 0 pet, 1 snack, 2 rest */
const char *gemmi_pet_shop(unsigned item);
bool gemmi_pet_save_ok(void);

#ifndef ESP_PLATFORM
/* Explicit desktop testing controls; never built into device firmware. */
void gemmi_pet_test_gems(uint32_t amount);
void gemmi_pet_test_level_ready(void);
#endif
