#include <string.h>

#include "tusb.h"
#include "usb_report_ids.h"

#include "ffb_midi.h"
#include "diag.h"
#include "config.h"

#ifndef PITCH_CENTRE_SHIFT
#define PITCH_CENTRE_SHIFT 0
#endif


// Translate from index in USB descriptor to byte that Sidewinder MIDI expects
static const uint8_t effect_type_usb_to_midi[] =
{
    MIDI_ET_NONE,
    MIDI_ET_CONSTANT,
    MIDI_ET_RAMP,
    MIDI_ET_SQUARE,
    MIDI_ET_SINE,
    MIDI_ET_TRIANGLE,
    MIDI_ET_SAWTOOTHUP,
    MIDI_ET_SAWTOOTHDOWN,
    MIDI_ET_SPRING,
    MIDI_ET_DAMPER,
    MIDI_ET_INERTIA,
    MIDI_ET_FRICTION
};

// USB effect type index -> MIDI type; MIDI_ET_NONE for anything out of range.
static uint8_t usb_effect_type_to_midi(uint8_t usb_type)
{
    return usb_type < sizeof(effect_type_usb_to_midi) ? effect_type_usb_to_midi[usb_type] : MIDI_ET_NONE;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
        hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    switch (report_type)
    {
        case HID_REPORT_TYPE_FEATURE:
            switch (report_id)
            {
                // Block Load Request
                case REPORT_ID_FEATURE_BLOCK_LOAD:
                    // Block Index / Assigned Effect ID
                    buffer[0] = ffb_midi_last_assigned_effect_id();
                    // Block Load Status: per our descriptor, 1 is success, 2 is full, 3 is error.
                    buffer[1] = ffb_midi_last_add_succeeded() ? 1 : 2;
                    // 16-bit RAM pool available: we'll just give the number of effects remaining
                    buffer[2] = ffb_midi_get_num_available_effects();
                    buffer[3] = 0x00;
                    return 4;

                // Pool size, max simultaneous effects, etc.
                case REPORT_ID_FEATURE_POOL_REPORT:
                    // 16-bit RAM pool size: we'll just give the total number of effects
                    buffer[0] = EFFECT_MEMORY_SIZE;
                    buffer[1] = 0x00;
                    // Num simultaneous effects
                    buffer[2] = MAX_SIMULTANEOUS_EFFECTS;
                    // Bit 7: 1 for supporting shared managed pool, 0 for not
                    // Bit 6: 1 for supporting shared parameter blocks, 0 for not
                    buffer[3] = 0xff;
                    return 4;

                case REPORT_ID_FEATURE_DIAG:
                {
                    uint16_t n = reqlen < sizeof(diag) ? reqlen : sizeof(diag);
                    memcpy(buffer, &diag, n);
                    return n;
                }

                case REPORT_ID_FEATURE_HANG_LOG:
                {
                    uint16_t n = reqlen < sizeof(hang_log) ? reqlen : sizeof(hang_log);
                    memcpy(buffer, &hang_log, n);
                    return n;
                }
            }

            break;
    }

    return 0;
}

static inline uint16_t join16(uint8_t first, uint8_t second)
{
    return ((uint16_t) first) | (((uint16_t) second) << 8);
}

#define USB_DURATION_INFINITE 0xffff
#define MIDI_DURATION_INFINITE 0

struct __attribute__((__packed__ )) t_set_envelope_report
{
    uint8_t effect_id;
    uint8_t attack_level;
    uint8_t fade_level;
    uint16_t attack_time;
    uint16_t fade_time;
};

struct __attribute__((__packed__ )) t_set_condition_report
{
    uint8_t effect_id;
    uint8_t axis; // "block offset"
    int8_t center_point_offset;
    uint8_t pos_coefficient;
    uint8_t neg_coefficient;
    uint8_t pos_saturation;
    uint8_t neg_saturation;
    uint8_t dead_band;
};

struct __attribute__((__packed__ )) t_set_ramp_report
{
    uint8_t effect_id;
    int8_t start;
    int8_t end;
};

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
        hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    switch (report_type)
    {
        case HID_REPORT_TYPE_OUTPUT:
        {
            switch(report_id)
            {
                case REPORT_ID_OUTPUT_SET_EFFECT:
                {
                    uint8_t effect_id       = buffer[0];
                    uint8_t effect_type_usb = buffer[1];
                    uint16_t duration       = join16(buffer[2], buffer[3]);
                    uint16_t trig_interval  = join16(buffer[4], buffer[5]);
                    uint16_t sample_period  = join16(buffer[6], buffer[7]);
                    uint8_t gain            = buffer[8];
                    uint8_t trig_button     = buffer[9];
                    bool ax0_enable         = (buffer[10] >> 0) & 0x01;
                    bool ax1_enable         = (buffer[10] >> 1) & 0x01;
                    bool dir_enable         = (buffer[10] >> 2) & 0x01;
                    uint8_t direction_x     = buffer[11];
                    uint8_t direction_y     = buffer[12];
                    uint16_t start_delay    = join16(buffer[13], buffer[14]);

                    // TODO use trigger interval
                    // TODO use sample period
                    // TODO use trigger button
                    // TODO use ax0 enable
                    // TODO use ax1 enable

                    uint8_t effect_type_midi = usb_effect_type_to_midi(effect_type_usb);

                    // MIDI duration is in "units of 2 ms".
                    // USB uses the max possible value for infinity. MIDI uses 0.
                    uint16_t duration_midi = (duration == USB_DURATION_INFINITE) ? MIDI_DURATION_INFINITE : (duration >> 1);
                    if (duration_midi > 0x3fff) { duration_midi = 0x3fff; } // cap long but finite effects
                    ffb_midi_modify(uart0, effect_id, MODIFY_DURATION, duration_midi);

                    switch (effect_type_midi)
                    {
                        case MIDI_ET_CONSTANT:
                        case MIDI_ET_SINE:
                        case MIDI_ET_SQUARE:
                        case MIDI_ET_RAMP:
                        case MIDI_ET_TRIANGLE:
                        case MIDI_ET_SAWTOOTHDOWN:
                        case MIDI_ET_SAWTOOTHUP:

                            ffb_midi_modify(uart0, effect_id, MODIFY_GAIN, gain);

                            // TODO axes enable
                            uint16_t direction_midi = 0;

                            if (dir_enable)
                            {
                                // map 0-180 to 0-360
                                direction_midi = ((uint16_t)direction_x) << 1;
                                ffb_midi_modify(uart0, effect_id, MODIFY_DIRECTION, direction_midi);
                            }
                            break;
                    }

                    break;
                }

                case REPORT_ID_OUTPUT_SET_ENVELOPE:
                {
                    struct t_set_envelope_report *report = (struct t_set_envelope_report*)(buffer);

                    // map 0->0xff down to 0->0x7f
                    ffb_midi_modify(uart0, report->effect_id, MODIFY_ATTACK_LEVEL, report->attack_level >> 1);
                    ffb_midi_modify(uart0, report->effect_id, MODIFY_FADE_LEVEL, report->fade_level >> 1);

                    // USB times are in ms; we convert to 2ms units
                    ffb_midi_modify(uart0, report->effect_id, MODIFY_ATTACK_TIME, report->attack_time >> 1);
                    ffb_midi_modify(uart0, report->effect_id, MODIFY_FADE_TIME, report->fade_time >> 1);

                    break;
                }

                case REPORT_ID_OUTPUT_SET_CONDITION:
                {
                    struct t_set_condition_report *report = (struct t_set_condition_report*)(buffer);
                    // SW FFB Pro cannot use Neg Coefficient, Pos/Neg Saturation, or Dead Band.

                    switch (report->axis)
                    {
                        case 0:
                        {
                            ffb_midi_modify(uart0, report->effect_id, MODIFY_OFFSET_X, report->center_point_offset);
                            ffb_midi_set_condition_strength(uart0, report->effect_id, 0, report->pos_coefficient >> 1);

                            break;
                        }
                        case 1:
                        {
                            // Apply PITCH_CENTRE_SHIFT (config.h), then negate: the FFB
                            // Pro's Y offset runs opposite to its reported Y axis
                            // (adapt-ffb-joy negates it too).
                            int offset = report->center_point_offset + PITCH_CENTRE_SHIFT;
                            if (offset > 127) { offset = 127; }
                            if (offset < -127) { offset = -127; }
                            int8_t offset_y = (int8_t)-offset;
                            ffb_midi_modify(uart0, report->effect_id, MODIFY_OFFSET_Y, offset_y);
                            ffb_midi_set_condition_strength(uart0, report->effect_id, 1, report->pos_coefficient >> 1);

                            break;
                        }
                    }

                    break;
                }

                case REPORT_ID_OUTPUT_SET_CONSTANT:
                {
                    uint8_t effect_id = buffer[0];
                    uint16_t magnitude  = join16(buffer[1], buffer[2]); // 255 to -255
                    magnitude = (magnitude & 0x1ff) >> 1;

                    ffb_midi_modify(uart0, effect_id, MODIFY_AMPLITUDE, magnitude);

                    break;
                }


                case REPORT_ID_OUTPUT_SET_RAMP:
                {
                    struct t_set_ramp_report *report = (struct t_set_ramp_report*)(buffer);

                    ffb_midi_modify(uart0, report->effect_id, MODIFY_AMPLITUDE, report->start);
                    ffb_midi_modify(uart0, report->effect_id, MODIFY_RAMP_END, report->end);

                    break;
                }

                case REPORT_ID_OUTPUT_SET_PERIODIC:
                {
                    uint8_t effect_id   = buffer[0];
                    uint8_t magnitude   = buffer[1];
                    int8_t offset       = (int8_t) buffer[2];
                    uint8_t phase       = buffer[3];
                    uint16_t period     = join16(buffer[4], buffer[5]);

                    // TODO: FFB Pro doesn't support offset or phase.
                    // adapt-ffb-joy works around this by switching between waveforms
                    // (apparently effect type 3 on the FFB Pro is a cosine?!)

                    uint16_t frequency = 1;
                    if (period <= 13) { frequency = 77; }
                    else if (period < 1000) { frequency = ((2000 / period) + 1) / 2; }

                    ffb_midi_modify(uart0, effect_id, MODIFY_FREQUENCY, frequency);
                    ffb_midi_modify(uart0, effect_id, MODIFY_SUSTAIN_LEVEL, magnitude);

                    break;
                }

                case REPORT_ID_OUTPUT_EFFECT_OPERATION:
                {
                    uint8_t effect_id = buffer[0];
                    uint8_t operation = buffer[1];
                    uint8_t loop_count = buffer[2]; // TODO use this

                    switch (operation)
                    {
                        case 1: // Start
                            ffb_midi_play(uart0, effect_id);
                            break;
                        case 2: // Start Solo
                            ffb_midi_play_solo(uart0, effect_id);
                            break;
                        case 3: // Stop
                            ffb_midi_pause(uart0, effect_id);
                            break;
                    }

                    break;
                }

                case REPORT_ID_OUTPUT_BLOCK_FREE:
                {
                    uint8_t effect_id = buffer[0];
                    ffb_midi_erase(uart0, effect_id);

                    break;
                }


                case REPORT_ID_OUTPUT_DEVICE_CONTROL:
                {
                    // 1 Enable Actuators, 2 Disable Actuators, 3 Stop All
                    // Effects, 4 Reset, 5 Pause, 6 Continue (per descriptor).
                    // DirectInput sends Reset when a game (re)acquires the
                    // device, then recreates its effects.
                    switch (buffer[0])
                    {
                        case 3:
                            ffb_midi_stop_all(uart0);
                            break;
                        case 4:
                            ffb_midi_reset_all(uart0);
                            break;
                    }

                    break;
                }

                case REPORT_ID_OUTPUT_DEVICE_GAIN:
                {
                    uint8_t device_gain = buffer[0];
                    ffb_midi_modify(uart0, 0x7f, MODIFY_DEVICE_GAIN, device_gain);
                }
            }

            break;
        }
        case HID_REPORT_TYPE_FEATURE:
        {
            switch(report_id)
            {
                case REPORT_ID_FEATURE_DIAG:
                {
                    // Commands: 1 = device reset (stock auto-center comes back
                    // on, as after a base reset), 2 = clear the counters,
                    // 3 = simulate a ~150 ms data dropout, 9 0xb7 = reboot into
                    // the USB bootloader.
                    if (bufsize >= 1 && buffer[0] == 1) { ffb_midi_device_control(uart0, MIDI_DC_RESET); }
                    if (bufsize >= 1 && buffer[0] == 2) { memset(&diag, 0, sizeof(diag)); }
                    if (bufsize >= 1 && buffer[0] == 3) { diag_simulate_dropout = true; }
                    if (bufsize >= 2 && buffer[0] == 9 && buffer[1] == 0xb7) { diag_reboot_to_bootsel = true; }
                    break;
                }

                case REPORT_ID_FEATURE_CREATE_NEW_EFFECT:
                {
                    // Only one byte: the effect type
                    struct Effect newEffect = {
                        .type = usb_effect_type_to_midi(buffer[0]),
                        .duration = 0,
                        .button_mask = 0,
                        .direction = 0,
                        .gain = 0x7f,
                        .strength_x = 0,
                        .strength_y = 0,
                        .sample_rate = 100,
                        .attack_level = 0x7f,
                        .sustain_level = 0x7f,
                        .fade_level = 0x7f,
                        .attack_time = 0,
                        .fade_time = 0,
                        .frequency = 1,
                        .amplitude = 0x7f,
                    };

                    ffb_midi_define_effect(uart0, &newEffect);

                    break;
                }
            }

            break;
        }
    }
}