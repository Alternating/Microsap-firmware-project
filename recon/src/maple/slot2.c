// slot2.c — where slot 2 heads (slot2.h).
#include "slot2.h"

static const uint8_t s_backings[] = { SLOT2_CHOICE_SD, SLOT2_CHOICE_PULSAR };
#define NBACKINGS (sizeof s_backings / sizeof s_backings[0])

uint8_t slot2_cycle(uint8_t choice, bool forward)
{
    unsigned i = 0;
    for (unsigned k = 0; k < NBACKINGS; k++)
        if (s_backings[k] == choice) i = k;
    i = forward ? (i + 1) % NBACKINGS : (i + NBACKINGS - 1) % NBACKINGS;
    return s_backings[i];
}

uint8_t slot2_first_pulsar(uint8_t choice)
{
    return choice == SLOT2_CHOICE_NONE ? SLOT2_CHOICE_PULSAR : choice;
}

slot2_backing_t slot2_target(uint8_t choice, slot2_backing_t at, bool linked, uint64_t lost_us,
                             bool owed)
{
    if (choice != SLOT2_CHOICE_PULSAR) return SLOT2_SD;
    if (linked) return SLOT2_PULSAR;
    if (at == SLOT2_PULSAR && (lost_us < SLOT2_GRACE_US || owed)) return SLOT2_PULSAR;
    return SLOT2_SD;
}
