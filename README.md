# Picowinder: RP2040-based USB adapter for the Microsoft Sidewinder Force Feedback Pro joystick
Picowinder is an adapter that uses an RP2040 (i.e., a Raspberry Pi Pico) to turn the [Microsoft Sidewinder Force Feedback Pro joystick](https://www.youtube.com/watch?v=evwn435x0dM) into a USB joystick, including force-feedback communication.

# About this fork

This fork is set up for gliding in [Condor 3](https://www.condorsoaring.com/): long flights with the trim spring active, trimming for speed with "move stick with trim", and thermalling with light inputs. It is a fork of [NolanNicholson/picowinder](https://github.com/NolanNicholson/picowinder).

Changes from the original:

* **No more lockups mid-flight.** The original firmware could leave the joystick dead 10-25 minutes into a force-feedback session ([issue #9](https://github.com/NolanNicholson/picowinder/issues/9)). Joystick reads and MIDI commands no longer overlap, the base gets the full start-up sequence the Windows driver sends, and if it does stop answering it is restarted and its effects restored without the game noticing.
* **Pitch trim works.** The pitch spring offset was inverted, so trim pulled the wrong way. `PITCH_CENTRE_SHIFT` in `config.h` also corrects where the base centres its pitch spring (set it to 0 for plain passthrough, and re-measure for another stick).
* **No hands-off oscillation.** Condor asks for a stiff spring, which can make the stick oscillate on its own when the hand sensor is bypassed. The firmware detects that, briefly cuts the spring and damps the stick, then brings the spring back. It only ever reduces force and never changes what the game reads. Turn it off with `OSCILLATION_GOVERNOR` in `config.h`.
* **Force levels are passed through as the game sends them.**

Build without `EXAMPLE_EFFECTS` (comment it out in `config.h`) for Condor.

To flash this firmware, power the joystick base off first: the base can keep the Pico powered through the GamePort, which stops it entering BOOTSEL mode.

# Compatibility
Picowinder is compatible with both Linux and Windows. It has not been tested elsewhere.

The adapter is mostly compliant with the HID (Human Interface Device) PID (Physical Interface Device) spec.

# How to Build

## Build the Adapter
Strictly speaking, this adapter can be built using nothing except a Raspberry Pi Pico and a GamePort connector. A few optional resistors are also recommended.

Any pins not listed on both the GamePort side and the Pico side can be left disconnected.

| GamePort Pin                       | Recommended Resistor | Pico Pin        | Notes                                      | 
|------------------------------------|----------------------|-----------------|--------------------------------------------|
| 1 (VCC)                            |                      | Pin 40 (VBUS)   |                                            | 
| 2 (Button 1; Sidewinder CLK)       |                      | Pin 5 (GP3)     |                                            | 
| 3 (Axis X1; Sidewinder Trigger 0)  | 2.2k ohm             | Pin 4 (GP2)     | GamePort pins 3 and 11 both connect to GP2 | 
| 4 (GND)                            |                      | Any GND pin     |                                            |
| 7 (Button 2; Sidewinder Data 0)    |                      | Pin 6 (GP4)     |                                            |
| 10 (Button 3; Sidewinder Data 1)   |                      | Pin 7 (GP5)     |                                            |
| 11 (Axis X2; Sidewinder Trigger 1) | 2.2k ohm             | Pin 4 (GP2)     |                                            |
| 12 (MIDI TX)                       | 220 ohm              | Pin 1 (GP0)     |                                            |
| 14 (Button 4; Sidewinder Data 2)   |                      | Pin 9 (GP6)     |                                            |


Alternately you can connect the Raspberry Pi Pico directly to the PCB using the following pinout, this will allow you to mount the Pico inside the housing of the controller.



| Pin on PCB                         | Recommended Resistor | Pico Pin        | Notes                                      | 
|------------------------------------|----------------------|-----------------|--------------------------------------------|
| 1  (VCC)                           |                      | Pin 40 (VBUS)   |                                            | 
| 2  (GND)                           |                      | Any GND pin     |                                            | 
| 3  (MIDI TX)                       | 220 ohm              | Pin 1 (GP0)     |                                            | 
| 4  N/C                             |                      | N/C             |                                            |
| 5  (Button 1; Sidewinder CLK)      |                      | Pin 5 (GP3)     |                                            |
| 6  (Button 2; Sidewinder Data 0)   |                      | Pin 6 (GP4)     |                                            |
| 7  (Button 3; Sidewinder Data 1)   |                      | Pin 7 (GP5)     |                                            |
| 8  (Button 4; Sidewinder Data 2)   |                      | Pin 9 (GP6)     |                                            |
| 9  (Axis X2; Sidewinder Trigger 1) | 2.2k ohm             | Pin 4 (GP2)     | Pins 9 and 10 both connect to GP2          |
| 10 N/C                             |                      | N/C             |                                            |
| 11 (Axis X1; Sidewinder Trigger 0) | 2.2k ohm             | Pin 4 (GP2)     | Pins 9 and 10 both connect to GP2          |




## Flash the Firmware

To flash a release of the firmware to your Pico:
1. Hold the BOOTSEL button while plugging the Pico in to your computer.
2. Release the BOOTSEL button. The Pico should present itself as a storage drive.
3. Drag the picowinder.uf2 file into that storage drive. It should automatically disconnect, and the Pico should reboot.

# Known Issues

* Other than the Sidewinder Force Feedback Pro joystick, no other joysticks or peripherals are supported.
* The adapter does not support plugging in the GamePort connector after USB has already connected. You must connect the GamePort first, then plug the USB in.

# Credits

* This project uses a lot of information from the [adapt-ffb-joy](https://github.com/tloimu/adapt-ffb-joy) project, and from the [reverse-engineering](https://www.descentbb.net/viewtopic.php?t=19061) that went into it.
* In addition, several forks of adapt-ffb-joy, including those by [juchong](https://github.com/juchong/adapt-ffb-joy) and [jaybee](https://github.com/jaybee-git/adapt-ffb-joy), provided additional insight.
