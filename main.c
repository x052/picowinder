#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"

#include "tusb.h"

#include "ffb_handshake.pio.h"
#include "read_joystick.pio.h"
#include "ffb_midi.h"
#include "diag.h"
#include "watchdog.h"
#include "bus.h"
#include "governor.h"

#include "config.h"


struct JoystickState
{
    uint16_t buttons        ;
    uint16_t x          : 10;
    uint16_t y          : 10;
    uint8_t  throttle   :  7;
    uint8_t  twist      :  6;
    uint8_t  hat        :  4;
} joystickState;

struct report
{
    uint16_t buttons;
    uint16_t x;
    uint16_t y;
    uint8_t twist;
    uint8_t throttle;
    uint8_t hat;
} report;

uint16_t report_size_bytes = 9; // sizeof doesn't necessarily work well due to packing

uint rxFifoEntries;
uint32_t raw0;
uint32_t raw1;
uint64_t joystickStateRaw;

// Completed joystick scans and the gaps between them, maintained by the read
// IRQ and consumed by the base-reset watchdog in main(). See watchdog.h.
volatile struct scan_tracker scans = { 0 };

// Base-reset diagnostics, readable from the host via the vendor feature report
// (REPORT_ID_FEATURE_DIAG). See usb.c.
struct diag_counters diag = { 0 };
struct hang_log hang_log = { 0 };
volatile bool diag_simulate_dropout = false;
volatile bool diag_reboot_to_bootsel = false;


void joystickReadIRQ()
{
    const PIO pio = pio0;
    const uint sm = 0;

    rxFifoEntries = pio_sm_get_rx_fifo_level(pio, sm);

    uint64_t raw0 = pio_sm_get(pio, sm);
    uint64_t raw1 = pio_sm_get(pio, sm);
    uint64_t raw = (raw1 << 16) | (raw0 >> 8);

#ifdef FIRMWARE_SHIFT
    bool shift = ((~raw) & 0x100) != 0;
    uint16_t buttons = (~raw) & 0xff;
    joystickState.buttons = shift ? (buttons << 8) : buttons;
#else
    joystickState.buttons   = ~(raw & 0x1ff);
#endif

    for (int i = 0; i < 6; i++) { diag.raw_packet[i] = (raw >> (8 * i)) & 0xff; }
    uint8_t hi = (raw >> 46) & 0x03;   // the parity bits
    if (hi != diag.raw_hi_bits) { diag.raw_hi_changes++; diag.raw_hi_bits = hi; }

    // A packet that fails parity still shows the base is alive, but must not
    // move the stick on the host.
    scan_tracker_on_scan(&scans, time_us_32());
    if (!ffp_packet_valid(raw))
    {
        diag.bad_packets++;
        pio_interrupt_clear(pio, 0);
        return;
    }

    joystickState.x         = (raw >>  9) & 0x3ff;
    joystickState.y         = (raw >> 19) & 0x3ff;
    joystickState.throttle  = (raw >> 29) & 0x07f;
    joystickState.twist     = (raw >> 36) & 0x03f;
    joystickState.hat       = (raw >> 42) & 0x00f;

    report.buttons = joystickState.buttons;
    report.x = joystickState.x;
    report.y = joystickState.y;
    report.twist = joystickState.twist;
    report.throttle = joystickState.throttle;
    report.hat = joystickState.hat;

    pio_interrupt_clear(pio, 0);
}

void hid_task()
{
    if (tud_suspended())
    {
        tud_remote_wakeup();
    }
    else
    {
        if (!tud_hid_ready()) { return; }

        tud_hid_n_report(0x00, 0x01, &report, report_size_bytes);
    }
}


static uint offset_handshake;
static uint offset_readjoy;

// Wait, optionally keeping USB alive (used for re-handshakes mid-session, so the
// host doesn't see the device stall).
static void wait_ms(uint32_t ms, bool service_usb)
{
    if (!service_usb) { sleep_ms(ms); return; }
    absolute_time_t done = make_timeout_time_ms(ms);
    while (!time_reached(done))
    {
        tud_task();
        hid_task();
    }
}

// MIDI initialisation sent after the trigger-pulse handshake, as Microsoft's
// driver does it (see adapt-ffb-joy's SidewinderFFBMIDI.md and
// FfbproEnableInterrupts()). The meaning of the device parameters written to
// effect 0x7f (params 0x40..0x74) is undocumented.
static const uint8_t init_reset[]  = { 0xc5, MIDI_DC_RESET };
static const uint8_t init_config[] = { 0xf0, 0x00, 0x01, 0x0a, 0x01, 0x10, 0x05, 0x6b, 0xf7 };
static const uint8_t init_device_params[] = {
    0xb5, 0x40, 0x7f, 0xa5, 0x72, 0x57,
    0xb5, 0x44, 0x7f, 0xa5, 0x3c, 0x43,
    0xb5, 0x48, 0x7f, 0xa5, 0x7e, 0x00,
    0xb5, 0x4c, 0x7f, 0xa5, 0x04, 0x00,
    0xb5, 0x50, 0x7f, 0xa5, 0x02, 0x00,
    0xb5, 0x54, 0x7f, 0xa5, 0x02, 0x00,
    0xb5, 0x58, 0x7f, 0xa5, 0x00, 0x7e,
    0xb5, 0x5c, 0x7f, 0xa5, 0x3c, 0x00,
    0xb5, 0x60, 0x7f, 0xa5, 0x14, 0x65,
    0xb5, 0x64, 0x7f, 0xa5, 0x7e, 0x6b,
    0xb5, 0x68, 0x7f, 0xa5, 0x36, 0x00,
    0xb5, 0x6c, 0x7f, 0xa5, 0x28, 0x00,
    0xb5, 0x70, 0x7f, 0xa5, 0x66, 0x4c,
    0xb5, 0x74, 0x7f, 0xa5, 0x7e, 0x01,
};
static const uint8_t init_gain_max[] = { 0xb5, 0x7c, 0x7f, 0xa5, 0x7f, 0x00 };
static const uint8_t init_stop_all[] = { 0xc5, MIDI_DC_STOP_ALL };

// Take a base that is already in FFB mode back out of it ("quit application"
// in the protocol notes). The base stays powered across a Pico reset, and the
// start-up sequence only works on a base that isn't in FFB mode.
static void ffb_quit_mode(bool service_usb)
{
    static const uint8_t quit[] = { 0xc5, 0x07 };
    uart_write_blocking(uart0, init_reset, sizeof(init_reset));
    wait_ms(20, service_usb);
    uart_write_blocking(uart0, quit, sizeof(quit));
    for (int pass = 0; pass < 2; pass++)
    {
        for (uint8_t ch = 0; ch < 16; ch++)
        {
            uint8_t msg[3] = { (uint8_t)(0xb0 | ch), 0x40, 0x00 };
            uart_write_blocking(uart0, msg, sizeof(msg));
        }
    }
    uart_tx_wait_blocking(uart0);
    wait_ms(100, service_usb);
}

static unsigned handshake_attempts = 0;

// Send the trigger-pulse sequence that switches the Sidewinder into
// digital/MIDI mode, then initialise it the way Microsoft's driver does.
// If service_usb is set, USB is kept alive while waiting. Returns the number
// of pulse groups sent.
static int ffb_handshake(bool service_usb)
{
    const PIO pio = pio0;
    const uint sm = 0;

    ffb_quit_mode(service_usb);

    pio_sm_set_enabled(pio, sm, false);
    ffb_handshake_program_init(pio, sm, offset_handshake, 100000, PIN_TRIGGER);

    // Pulse groups and the delay before each (0.1 ms units): "1-4-3-2-2-3-2".
    // Delays (ms):            7     30    15    78     4    59
    uint delays[7] = { 1000,   70,   300,  150,  780,   40,  590   };
    // Pulses (count):       1     4     3     2     2     3     2
    uint pulses[7] = {       1,    4,    3,    2,    2,    3,    2 };

    // Every other attempt sends only the first four groups ("1-4-3-2"), the
    // sequence earlier versions of this firmware sent.
    int groups = (handshake_attempts++ % 2 == 0) ? 7 : 4;

    // The TX FIFO holds only 4 words, so feed the groups as it drains.
    pio_sm_set_enabled(pio, sm, true);
    for (int i = 0; i < groups; )
    {
        if (!pio_sm_is_tx_fifo_full(pio, sm))
        {
            pio_sm_put(pio, sm, (pulses[i] - 1) << 16 | (delays[i] - 1));
            i++;
        }
        else if (service_usb)
        {
            tud_task();
            hid_task();
        }
    }

    // The last group (59 ms delay + 2 pulses) is still being sent.
    wait_ms(80, service_usb);
    pio_sm_set_enabled(pio, sm, false);

    // MIDI start-up, with the driver's delays. Written directly rather than
    // queued: no joystick reads run during the handshake.
    uart_write_blocking(uart0, init_reset, sizeof(init_reset));
    wait_ms(20, service_usb);
    uart_write_blocking(uart0, init_config, sizeof(init_config));
    wait_ms(57, service_usb);
    uart_write_blocking(uart0, init_device_params, sizeof(init_device_params));
    uart_write_blocking(uart0, init_reset, sizeof(init_reset));
    wait_ms(70, service_usb);
    uart_write_blocking(uart0, init_gain_max, sizeof(init_gain_max));
#ifdef DISABLE_AUTO_CENTER
    uart_write_blocking(uart0, init_stop_all, sizeof(init_stop_all));
#endif
    uart_tx_wait_blocking(uart0);
    return groups;
}

// MIDI to the base is queued and released between joystick reads (see bus.h).
// If the queue is full, flush it blocking rather than drop part of a message.
void midi_write(uart_inst_t *uart, const uint8_t *data, size_t len)
{
    if (!midi_queue_push(data, len))
    {
        diag.midi_overflows++;
        uint8_t chunk[64];
        size_t n;
        while ((n = midi_queue_pop(chunk, sizeof(chunk))) > 0) { uart_write_blocking(uart, chunk, n); }
        if (!midi_queue_push(data, len)) { uart_write_blocking(uart, data, len); return; }
    }
    if (midi_queue_len() > diag.midi_queue_max) { diag.midi_queue_max = midi_queue_len(); }
}

static inline bool uart_tx_idle(uart_inst_t *uart)
{
    return (uart_get_hw(uart)->fr & UART_UARTFR_BUSY_BITS) == 0;
}

static void start_read_sm(void)
{
    const PIO pio = pio0;
    const uint sm = 0;

    pio_sm_set_enabled(pio, sm, false);
    read_joystick_program_init(pio, sm, offset_readjoy, 1000000,
            PIN_TRIGGER, PIN_CLK, PIN_D0, PIN_D1, PIN_D2);
    pio_sm_set_enabled(pio, sm, true);
}

int main()
{
    tud_init(0);

    // Hardware UART setup.
    // The default is 8 data bits, no parity bit, and 1 stop bit.
    uart_init(uart0, 31250);
    gpio_set_function(PIN_MIDI_TX, UART_FUNCSEL_NUM(uart0, PIN_MIDI_TX));

#ifdef PICO_DEFAULT_LED_PIN
    // Onboard LED: latched on after any recovery from a base reset or hang,
    // so you can tell after a session whether one happened.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    // PIO setup
    PIO pio = pio0;
    uint sm = 0;

    // Load both PIO programs up front: the handshake may be re-run later.
    offset_handshake = pio_add_program(pio, &ffb_handshake_program);
    offset_readjoy = pio_add_program(pio, &read_joystick_program);

    ffb_handshake(false);

    // Read-data PIO program setup.
    // We blow away the FFB-handshake state machine config,
    // because we don't need it anymore.
    read_joystick_program_init(pio, sm, offset_readjoy, 1000000,
            PIN_TRIGGER, PIN_CLK, PIN_D0, PIN_D1, PIN_D2);

    // Set up our IRQ to read the collected joystick data
    uint pio_irq = PIO0_IRQ_0;
    pio_set_irq0_source_enabled(pio0, pis_interrupt0, true);
    irq_set_exclusive_handler(pio_irq, joystickReadIRQ);
    irq_set_enabled(pio_irq, true);

    // Activate the state machine for reading the stick. It waits for the main
    // loop to request each read (bus.h) and fires an IRQ when a packet is in.
    pio_sm_set_enabled(pio, sm, true);

/*
Most games don't actually support force-feedback, but we can still use the joystick's
FFB to enhance them. We do so by adding two effects: a light spring effect that is
(at least in this author's opinion) gentler and more pleasant than the stock auto-center,
and a kickback effect that plays when the trigger is pulled, and sustains a little while
the trigger is held.
*/
#ifdef EXAMPLE_EFFECTS

    struct Effect lightSpringEffect = {
        .play_immediately = true,
        .type = MIDI_ET_SPRING,
        .duration = 0,
        .button_mask = 0,
        .strength_x = 0x30,
        .strength_y = 0x30,
        .offset_x = 0,
        .offset_y = 0,
    };

    struct Effect kickbackEffect = {
        .play_immediately = false,
        .type = MIDI_ET_CONSTANT,
        .duration = 0x3fff, // must be finite for the envelope to restart when stopping/playing
        .button_mask = 0x00,
        .direction = 0,
        .gain = 0x7f,
        .sample_rate = 100,
        .attack_level = 0x7f,
        .sustain_level = 0x28,
        .fade_level = 0x00,
        .attack_time = 80,
        .fade_time = 0,
        .frequency = 1,
        .amplitude = 0x7f,
    };

    int effect_id_spring = ffb_midi_define_effect(uart0, &lightSpringEffect);
    int effect_id_kickback = ffb_midi_define_effect(uart0, &kickbackEffect);

    bool fire_old;

#endif

    // Base-reset watchdog (see watchdog.h). Recovery never touches USB, so the
    // host keeps its device handle and effect IDs.
    struct watchdog wd;
    watchdog_init(&wd, time_us_32(), scans.count);

    struct bus bus;
    bus_init(&bus, time_us_32());

    // Hang log state (see diag.h)
    bool silent = false;
    int last_groups = 0;
    uint64_t last_packet_us = time_us_64();

#ifdef OSCILLATION_GOVERNOR
    struct governor gov;
    governor_init(&gov, time_us_32());
    bool gov_damper = false;
#endif

    // Main USB loop
    while (1)
    {
        tud_task(); // tinyusb device task
        hid_task();

        if (diag_reboot_to_bootsel)
        {
            ffb_quit_mode(true);   // the base keeps its state across our reset
            // Let the USB control transfer that asked for this complete.
            absolute_time_t t = make_timeout_time_ms(50);
            while (!time_reached(t)) { tud_task(); }
            reset_usb_boot(0, 0);
        }

        if (diag_simulate_dropout)
        {
            // Test hook: stop reading the base for ~150 ms, as a glitch would.
            diag_simulate_dropout = false;
            pio_sm_set_enabled(pio, sm, false);
            busy_wait_us_32(150000);
            pio_sm_set_enabled(pio, sm, true);
        }

        uint32_t sc = scans.count;
        uint32_t gap = 0;
        if (sc != wd.last_scan_count)
        {
            uint64_t now64 = time_us_64();
            if (silent)
            {
                hang_log.ev[0].end_s = (uint32_t)(now64 / 1000000u);
                hang_log.ev[0].groups = (uint8_t)last_groups;
                silent = false;
            }
            last_packet_us = now64;

            uint32_t irq_state = save_and_disable_interrupts();
            gap = scan_tracker_take_gap(&scans);
            restore_interrupts(irq_state);

            if (gap >= WATCHDOG_GAP_US)
            {
                uint32_t gap_ms = gap / 1000;
                diag.data_gaps++;
                if (gap_ms > diag.max_gap_ms) { diag.max_gap_ms = gap_ms > 0xffff ? 0xffff : gap_ms; }
                diag.last_gap_s = (uint32_t)(now64 / 1000000u);
            }

#ifdef OSCILLATION_GOVERNOR
            uint32_t t = time_us_32();
            governor_update(&gov, t, joystickState.x, joystickState.y);
            ffb_midi_set_spring_scale(uart0, governor_spring_permille(&gov, t));
            // The damper also plays whenever the game has nothing playing: the
            // motors otherwise free-wheel, and a mechanical pull on the pitch
            // axis can then run the stick into its stop. Damper plus a stiff
            // spring is unstable, so it's off while a game spring plays (except
            // in the governor's calm phase, when the spring is at zero).
            bool want = governor_damper_on(&gov) || !ffb_midi_host_effects_playing();
            if (want != gov_damper)
            {
                ffb_midi_governor_damper(uart0, want);
                gov_damper = want;
            }
            diag.gov_triggers = gov.triggers;
#endif
        }

        enum watchdog_action action = watchdog_poll(&wd, time_us_32(), sc, gap);
        switch (action)
        {
            case WD_REPLAY:
#ifdef DISABLE_AUTO_CENTER
                ffb_midi_device_control(uart0, MIDI_DC_STOP_ALL);
#endif
                ffb_midi_replay_state(uart0);
                diag.state_replays++;
                break;

            case WD_REINIT:
                // Anything still queued predates the hang; the replay below
                // restores the full state.
                midi_queue_clear();
                if (!silent)
                {
                    // A new silence: shift the log and open an entry.
                    for (int i = HANG_LOG_LEN - 1; i > 0; i--) { hang_log.ev[i] = hang_log.ev[i - 1]; }
                    hang_log.ev[0] = (struct hang_event){ .start_s = (uint32_t)(last_packet_us / 1000000u) };
                    if (hang_log.count < 255) { hang_log.count++; }
                    silent = true;
                }
                if (hang_log.ev[0].attempts < 0xffff) { hang_log.ev[0].attempts++; }
                last_groups = ffb_handshake(true);
                ffb_midi_replay_state(uart0);
                start_read_sm();
                diag.full_reinits++;
                diag.state_replays++;
                break;

            case WD_KICK:
                // Kick the read state machine: disable, flush any partial data,
                // reset internal state, jump back to the program start, re-enable.
                pio_sm_set_enabled(pio, sm, false);
                pio_sm_clear_fifos(pio, sm);
                pio_sm_restart(pio, sm);
                pio_sm_exec(pio, sm, pio_encode_jmp(offset_readjoy));
                pio_sm_set_enabled(pio, sm, true);
                diag.sm_kicks++;
                break;

            case WD_NONE:
                break;
        }
        if (action != WD_NONE)
        {
            watchdog_done(&wd, action, time_us_32());
#ifdef PICO_DEFAULT_LED_PIN
            if (action != WD_KICK) { gpio_put(PICO_DEFAULT_LED_PIN, 1); }
#endif
        }

        // Interleave stick reads and MIDI so they never overlap on the base.
        uint32_t now = time_us_32();
        if (bus_should_trigger(&bus, now, scans.count, uart_tx_idle(uart0)))
        {
            pio_sm_put(pio, sm, 0);
        }
        else
        {
            size_t n = bus_midi_budget(&bus, now, scans.count, midi_queue_len());
            if (n > 0)
            {
                uint8_t chunk[32];
                if (n > sizeof(chunk)) { n = sizeof(chunk); }
                n = midi_queue_pop(chunk, n);
                uart_write_blocking(uart0, chunk, n);   // fits in the 32-byte TX FIFO
                bus_midi_sent(&bus, now, n);
            }
        }

#ifdef EXAMPLE_EFFECTS
        bool fire = (joystickState.buttons & 0x0001) != 0;
        if (fire && !fire_old)
        {
            ffb_midi_play(uart0, effect_id_kickback);
        }
        else if (!fire && fire_old)
        {
            ffb_midi_pause(uart0, effect_id_kickback);
        }
        fire_old = fire;
#endif
    }
}
