/* Run with GEMMI_PET_SAVE pointing to a disposable path. */
#include "gemmi_pet.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    const char *path = getenv("GEMMI_PET_SAVE");
    assert(path && *path);
    remove(path);
    gemmi_pet_init();
    assert(gemmi_pet_level() == 1 && gemmi_pet_stats()->gems == 20);
    assert(!gemmi_pet_event("codex", "working", 1));
    assert(gemmi_pet_event("codex", "success", 2));
    assert(!gemmi_pet_event("codex", "success", 2));
    assert(gemmi_pet_stats()->xp == 25 && gemmi_pet_stats()->gems == 25);
    gemmi_pet_init();
    assert(!gemmi_pet_event("codex", "success", 2));
    assert(gemmi_pet_stats()->tasks == 1);
    assert(!strcmp(gemmi_pet_shop(GEMMI_PET_CROWN), "Save a few more gems first."));
    assert(gemmi_pet_stats()->gems == 25 && !gemmi_pet_stats()->owned);
    gemmi_pet_shop(GEMMI_PET_GLASSES);
    assert(gemmi_pet_stats()->gems == 10 && gemmi_pet_stats()->equipped == 1);
    gemmi_pet_shop(GEMMI_PET_GLASSES);
    assert(gemmi_pet_stats()->gems == 10 && gemmi_pet_stats()->equipped == 0);
    gemmi_pet_shop(GEMMI_PET_GLASSES);
    assert(gemmi_pet_stats()->gems == 10 && gemmi_pet_stats()->equipped == 1);
    for (int id = 3; id < 6; id++) assert(gemmi_pet_event("codex", "completed", id));
    assert(gemmi_pet_level() == 2 && gemmi_pet_stats()->xp == 100);
    assert(gemmi_pet_stats()->gems == 35); /* 10 + 3*5 + level bonus */
    assert(!gemmi_pet_event("demo", "success", 0));
    gemmi_pet_event("demo", "working", 0);
    assert(gemmi_pet_event("demo", "success", 0));
    assert(!gemmi_pet_event("demo", "success", 0));
    gemmi_pet_event("failed", "working", 0);
    gemmi_pet_event("failed", "failed", 0);
    assert(!gemmi_pet_event("failed", "success", 0));
    unsigned energy = gemmi_pet_stats()->energy, gems = gemmi_pet_stats()->gems;
    gemmi_pet_care(1);
    assert(gemmi_pet_stats()->energy == energy + 25 && gemmi_pet_stats()->gems == gems - 3);
    gemmi_pet_care(1);
    assert(gemmi_pet_stats()->gems == gems - 3); /* cooldown prevents repeated charge */
    gemmi_pet_care(0);
    unsigned mood = gemmi_pet_stats()->mood;
    gemmi_pet_care(0);
    assert(gemmi_pet_stats()->mood == mood && mood <= 100);
    gemmi_pet_init();
    assert(gemmi_pet_level() == 2 && gemmi_pet_stats()->equipped == 1);
    assert(gemmi_pet_save_ok());
    unsigned balance = gemmi_pet_stats()->gems;
    gemmi_pet_shop(GEMMI_PET_ROSE);
    assert(gemmi_pet_stats()->gems == balance - 20);
    gemmi_pet_init();
    assert(gemmi_pet_stats()->equipped == GEMMI_PET_ROSE + 1);
    assert(gemmi_pet_stats()->owned & (1u << GEMMI_PET_ROSE));
    gemmi_pet_shop(GEMMI_PET_GLASSES);
    assert(gemmi_pet_stats()->gems == balance - 20); /* owned looks remain free */
    assert(gemmi_pet_stats()->equipped == GEMMI_PET_GLASSES + 1);
    unsigned xp_before = gemmi_pet_stats()->xp;
    gemmi_pet_test_gems(10000);
    assert(gemmi_pet_stats()->gems == 10000 && gemmi_pet_stats()->xp == xp_before);
    unsigned previous_level = gemmi_pet_level();
    gemmi_pet_test_level_ready();
    gemmi_pet_event("demo", "working", 0);
    assert(gemmi_pet_event("demo", "success", 0));
    assert(gemmi_pet_level() == previous_level + 1);
    assert(gemmi_pet_stats()->gems == 10015);
    gemmi_pet_init();
    assert(gemmi_pet_stats()->gems == 10015);
    gemmi_pet_shop(GEMMI_PET_BOW);
    assert(gemmi_pet_stats()->equipped == GEMMI_PET_BOW + 1);
    assert(gemmi_pet_stats()->gems == 9990);
    gemmi_pet_init();
    assert(gemmi_pet_stats()->equipped == GEMMI_PET_BOW + 1);
    remove(path);
    puts("Pet rewards, deduplication, levels, purchases, care, and persistence passed.");
}
