// slot2.h — where slot 2 heads: the SD card's VMU or the one docked in a Pulsar.
// Both live in one buffer, never both at once; maple.c moves the slot, this decides. No hardware
// and no clock of its own, so a host test can drive it.
#ifndef P2M_SLOT2_H
#define P2M_SLOT2_H

#include <stdbool.h>
#include <stdint.h>

// SETTINGS_FIELD_SLOT2: what the owner last chose.
#define SLOT2_CHOICE_NONE   0u      // no Pulsar seen yet: SD
#define SLOT2_CHOICE_SD     1u
#define SLOT2_CHOICE_PULSAR 2u

typedef enum { SLOT2_SD = 1, SLOT2_PULSAR = 2 } slot2_backing_t;

// A Pulsar that held slot 2 keeps it this long after its link goes, so a quick reconnect finds
// its card where it left it.
#define SLOT2_GRACE_US 5000000u

// The chord: the next or the previous backing in {SD, Pulsar}, wrapping. The list is the one
// place a later backing is added. NONE steps as SD.
uint8_t slot2_cycle(uint8_t choice, bool forward);
// The first Pulsar to connect takes slot 2, once: NONE becomes PULSAR, a choice made holds.
uint8_t slot2_first_pulsar(uint8_t choice);
// Where slot 2 heads, from the choice, what it holds now, whether a Pulsar owns the link, how
// long since the one holding the slot went (0 while it has not), and whether its card is still
// owed blocks. A Pulsar chosen but not there leaves the slot to SD, except that the one holding it
// keeps it through the grace, and for as long as its card is owed blocks: SD would load over them.
slot2_backing_t slot2_target(uint8_t choice, slot2_backing_t at, bool linked, uint64_t lost_us,
                             bool owed);

#endif
