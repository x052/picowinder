#ifndef GOVERNOR_H
#define GOVERNOR_H

#include <stdbool.h>
#include <stdint.h>

/*
Oscillation governor, kept free of hardware access so it can be unit-tested on
the host (see tests/).

With its hand sensor bypassed, the FFB Pro can't cut force when the stick is let
go, and a stiff spring (Condor asks for up to 100%) can then drive the stick into
a self-sustaining oscillation of several hundred counts. The governor watches
the stick and, only when it is clearly oscillating on its own, briefly cuts the
spring and applies a damper, then ramps the spring back.

Safety rules (tested):
  * It only ever REDUCES spring strength (scale <= 1). It never adds force.
  * It never changes the stick position reported to the host.
  * Detection needs a large, fast, repeating swing that a pilot flying a glider
    doesn't produce.
*/

#define GOV_WINDOW_US        700000u   // detection window
#define GOV_MIN_SWING          150     // peak-to-peak counts within the window
#define GOV_MIN_STEP            40     // a reversal must follow a move of at least this
#define GOV_MIN_REVERSALS        3
#define GOV_CALM_US          600000u   // spring off + damper on
#define GOV_RAMP_US         2000000u   // spring ramps back from GOV_RAMP_START to the cap
#define GOV_RAMP_START         200     // per mille
#define GOV_CAP_STEP           100     // per mille lost from the cap...
#define GOV_PERSIST_COUNT        3     // ...but only when this many triggers fall
#define GOV_PERSIST_US     20000000u   // within this long (a single snap-back doesn't count)
#define GOV_CAP_MIN            500     // per mille
#define GOV_CAP_RECOVER_US 60000000u   // cap recovers one step per quiet minute

#define GOV_HIST 160                   // > window at 200 Hz

enum gov_state { GOV_NORMAL, GOV_CALM, GOV_RAMP };

struct governor
{
    uint32_t t[GOV_HIST];
    int16_t pos[2][GOV_HIST];
    int head, count;
    enum gov_state state;
    uint32_t state_since_us;
    uint32_t last_trigger_us;
    uint32_t recent[GOV_PERSIST_COUNT];   // times of the last few triggers
    int recent_n;
    uint16_t cap_permille;
    uint16_t triggers;
};

void governor_init(struct governor *g, uint32_t now_us);

// Feed one stick reading (call on every new scan).
void governor_update(struct governor *g, uint32_t now_us, int x, int y);

// Spring strength to apply, as a fraction of what the host asked for (per mille, <= 1000).
uint16_t governor_spring_permille(const struct governor *g, uint32_t now_us);

// Should the governor's own damper be playing right now?
bool governor_damper_on(const struct governor *g);

#endif // GOVERNOR_H
