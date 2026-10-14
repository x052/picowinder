#include "governor.h"

void governor_init(struct governor *g, uint32_t now_us)
{
    g->head = 0;
    g->count = 0;
    g->state = GOV_NORMAL;
    g->state_since_us = now_us;
    g->last_trigger_us = now_us;
    g->cap_permille = 1000;
    g->triggers = 0;
    g->recent_n = 0;
}

// Is the given axis swinging back and forth by itself within the window?
static bool axis_oscillating(const struct governor *g, int axis, uint32_t now_us)
{
    int lo = 0x7fff, hi = -0x7fff, n = 0;
    for (int k = 0; k < g->count; k++)
    {
        int i = (g->head - 1 - k + GOV_HIST) % GOV_HIST;
        if (now_us - g->t[i] > GOV_WINDOW_US) { break; }
        int v = g->pos[axis][i];
        if (v < lo) { lo = v; }
        if (v > hi) { hi = v; }
        n++;
    }
    if (n < 30 || hi - lo < GOV_MIN_SWING) { return false; }

    // Count direction reversals, oldest to newest, ignoring moves smaller than GOV_MIN_STEP.
    int reversals = 0, direction = 0;
    int extreme = g->pos[axis][(g->head - n + GOV_HIST) % GOV_HIST];
    for (int k = n - 2; k >= 0; k--)
    {
        int v = g->pos[axis][(g->head - 1 - k + GOV_HIST) % GOV_HIST];
        int d = v - extreme;
        if (direction >= 0 && d <= -GOV_MIN_STEP)
        {
            if (direction > 0) { reversals++; }
            direction = -1; extreme = v;
        }
        else if (direction <= 0 && d >= GOV_MIN_STEP)
        {
            if (direction < 0) { reversals++; }
            direction = 1; extreme = v;
        }
        else if ((direction > 0 && v > extreme) || (direction < 0 && v < extreme))
        {
            extreme = v;
        }
    }
    return reversals >= GOV_MIN_REVERSALS;
}

void governor_update(struct governor *g, uint32_t now_us, int x, int y)
{
    g->t[g->head] = now_us;
    g->pos[0][g->head] = (int16_t)x;
    g->pos[1][g->head] = (int16_t)y;
    g->head = (g->head + 1) % GOV_HIST;
    if (g->count < GOV_HIST) { g->count++; }

    // A quiet minute gives back one step of the cap.
    if (g->cap_permille < 1000 && now_us - g->last_trigger_us > GOV_CAP_RECOVER_US)
    {
        g->cap_permille += GOV_CAP_STEP;
        if (g->cap_permille > 1000) { g->cap_permille = 1000; }
        g->last_trigger_us = now_us;
    }

    switch (g->state)
    {
        case GOV_CALM:
            if (now_us - g->state_since_us > GOV_CALM_US)
            {
                g->state = GOV_RAMP;
                g->state_since_us = now_us;
                g->count = 0;   // judge the ramp on fresh data only
            }
            return;
        case GOV_RAMP:
            if (now_us - g->state_since_us > GOV_RAMP_US) { g->state = GOV_NORMAL; }
            break;
        case GOV_NORMAL:
            break;
    }

    if (axis_oscillating(g, 0, now_us) || axis_oscillating(g, 1, now_us))
    {
        g->state = GOV_CALM;
        g->state_since_us = now_us;
        g->last_trigger_us = now_us;
        g->triggers++;
        g->count = 0;

        // Lower the cap only if the oscillation keeps coming back: that means the
        // host's spring is too stiff for this stick right now. One snap-back after
        // the pilot lets go is handled by the calm/ramp alone.
        for (int i = GOV_PERSIST_COUNT - 1; i > 0; i--) { g->recent[i] = g->recent[i - 1]; }
        g->recent[0] = now_us;
        if (g->recent_n < GOV_PERSIST_COUNT) { g->recent_n++; }
        if (g->recent_n == GOV_PERSIST_COUNT
                && now_us - g->recent[GOV_PERSIST_COUNT - 1] <= GOV_PERSIST_US)
        {
            g->cap_permille = (g->cap_permille > GOV_CAP_MIN + GOV_CAP_STEP)
                    ? g->cap_permille - GOV_CAP_STEP : GOV_CAP_MIN;
        }
    }
}

uint16_t governor_spring_permille(const struct governor *g, uint32_t now_us)
{
    switch (g->state)
    {
        case GOV_CALM:
            return 0;
        case GOV_RAMP:
        {
            uint32_t elapsed = now_us - g->state_since_us;
            if (elapsed > GOV_RAMP_US) { elapsed = GOV_RAMP_US; }
            uint32_t frac = GOV_RAMP_START + (1000 - GOV_RAMP_START) * elapsed / GOV_RAMP_US;
            return (uint16_t)(frac * g->cap_permille / 1000);
        }
        case GOV_NORMAL:
        default:
            return g->cap_permille;
    }
}

bool governor_damper_on(const struct governor *g)
{
    return g->state == GOV_CALM;
}
