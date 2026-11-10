#pragma once

#include <stdint.h>
#include "driver/gpio.h"

// Pulse-output lines from the two YF-B5 hall-effect flow sensors. Both are
// plain GPIO (not strapping, not the antenna-switch pins 3/14 - see
// configure_internal_antenna() in main.c - not the onboard WS2812 LED (8) and
// not the native USB D-/D+ pins (12/13), which carry the console).
//
// If your SuperMini revision breaks these out differently, change the two
// lines below - nothing else in the firmware depends on the exact pin number.
#define FLOW_SUPPLY_GPIO GPIO_NUM_18  // "подача" - water going into the tub
#define FLOW_RETURN_GPIO GPIO_NUM_19  // "обратка" - water coming back out

// Zigbee endpoints for the two sensors. Fixed (not scanned/sorted like the
// DS18B20 1-Wire bus in the sibling zigbee-temp project) because each sensor
// is wired to its own dedicated GPIO, not shared bus - there's nothing to
// enumerate.
#define FLOW_SUPPLY_ENDPOINT 10
#define FLOW_RETURN_ENDPOINT 11

// YF-B5 pulses-per-liter calibration constant. The commonly quoted formula
// for this sensor family is F(Hz) = 7.5 x Q(L/min), i.e. 7.5 pulses/sec per
// L/min = 450 pulses per liter - but exact K-factor varies between YF-B5
// batches/pipe-diameter variants. VERIFY this against your specific sensor's
// datasheet, or calibrate empirically (count pulses while passing a known
// volume through the sensor) before trusting absolute L/min readings.
#define FLOW_SUPPLY_PULSES_PER_LITER 450.0f
#define FLOW_RETURN_PULSES_PER_LITER 450.0f

// How often the PCNT counters are read, converted to L/min and reported over
// Zigbee. Shorter than the temperature sensor's 20s - bath circulation flow
// starts/stops on a human timescale and we want that to show up promptly.
#define REPORT_INTERVAL_MS (5 * 1000)

// PCNT glitch filter: pulses narrower than this are treated as switch bounce
// / electrical noise on the hall sensor's open-collector output and ignored.
#define PCNT_GLITCH_FILTER_NS 1000

// Sanity bounds for reported flow (L/min) - values outside this range are
// treated as a counting glitch and not reported, rather than sent to Zigbee
// as a wildly wrong reading. Unlike temperature, 0 L/min is a normal,
// expected reading (pump off / valve closed) and is always reported.
#define FLOW_MIN_LPM 0.0f
#define FLOW_MAX_LPM 100.0f
