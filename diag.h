#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stdint.h>

// Counters exposed to the host as vendor-defined feature reports, so recoveries
// can be observed without a debugger (tools/read_diag.py reads them).
struct __attribute__((__packed__)) diag_counters
{
    uint16_t data_gaps;      // packets resumed after a >= 50 ms gap
    uint16_t state_replays;  // effect memory rebuilds
    uint16_t full_reinits;   // handshakes re-run after a stall
    uint16_t sm_kicks;       // read state machine restarts
    uint16_t bad_packets;    // packets dropped for failing parity
    uint16_t max_gap_ms;     // longest gap between packets
    uint32_t last_gap_s;     // uptime (s) at the end of the last gap
    uint8_t  raw_packet[6];  // last raw 48-bit packet, LSB first
    uint8_t  raw_hi_bits;    // bits 46..47 of the last packet (parity)
    uint8_t  raw_hi_changes; // times bits 46..47 changed (wraps)
    uint16_t midi_queue_max; // MIDI queue high-water mark, bytes
    uint16_t midi_overflows; // times the queue was full and was flushed blocking
    uint16_t gov_triggers;   // oscillation governor interventions
};

// Times the base stopped answering, newest first. Times are seconds since boot.
#define HANG_LOG_LEN 4
struct __attribute__((__packed__)) hang_event
{
    uint32_t start_s;        // last packet before the silence
    uint32_t end_s;          // first packet after it (0 = still silent)
    uint16_t attempts;       // handshakes tried
    uint8_t  groups;         // pulse groups in the attempt that worked (7 or 4)
    uint8_t  reserved;
};
struct __attribute__((__packed__)) hang_log
{
    uint8_t count;           // total events since boot (saturates)
    struct hang_event ev[HANG_LOG_LEN];
};
extern struct hang_log hang_log;

_Static_assert(sizeof(struct diag_counters) == 30, "update the DIAG report count in usb_descriptors.c");
_Static_assert(sizeof(struct hang_log) == 49, "update the HANG_LOG report count in usb_descriptors.c");

extern struct diag_counters diag;

// Set by the diagnostics feature report, acted on by the main loop.
extern volatile bool diag_simulate_dropout;
extern volatile bool diag_reboot_to_bootsel;

#endif // DIAG_H
