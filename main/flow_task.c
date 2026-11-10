#include <math.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zigbee.h"
#include "tasks.h"
#include "config.h"

static const char *TAG = "flow_task";

typedef struct {
    const char *name;
    gpio_num_t gpio;
    uint8_t endpoint;
    float pulses_per_liter;
    pcnt_unit_handle_t pcnt_unit;
} flow_channel_t;

static flow_channel_t channels[] = {
    {"supply", FLOW_SUPPLY_GPIO, FLOW_SUPPLY_ENDPOINT, FLOW_SUPPLY_PULSES_PER_LITER, NULL},
    {"return", FLOW_RETURN_GPIO, FLOW_RETURN_ENDPOINT, FLOW_RETURN_PULSES_PER_LITER, NULL},
};
#define CHANNEL_COUNT (sizeof(channels) / sizeof(channels[0]))

// Arms one PCNT unit to count rising edges from a YF-B5's open-collector
// pulse output. Internal pull-up is enabled in addition to the external
// pull-up resistor called for in the README - belt and suspenders, since a
// missing/weak external pull-up would otherwise read as a permanently "high
// flow" floating input rather than a clean zero.
static void setup_channel(flow_channel_t *ch)
{
    gpio_set_direction(ch->gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(ch->gpio, GPIO_PULLUP_ONLY);

    pcnt_unit_config_t unit_config = {
        .high_limit = 30000,
        .low_limit = -30000,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &ch->pcnt_unit));

    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = PCNT_GLITCH_FILTER_NS,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(ch->pcnt_unit, &filter_config));

    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = ch->gpio,
        .level_gpio_num = -1, // no direction-control pin, we only ever count up
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(ch->pcnt_unit, &chan_config, &pcnt_chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP));

    ESP_ERROR_CHECK(pcnt_unit_enable(ch->pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(ch->pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(ch->pcnt_unit));

    ESP_LOGI(TAG, "PCNT channel '%s' armed on GPIO%d (endpoint %d)", ch->name, ch->gpio, ch->endpoint);
}

// ZCL Flow Measurement's MeasuredValue represents 10 x flow in m3/h.
// 1 L/min = 0.06 m3/h, so value = L/min x 0.6, rounded to the nearest
// integer - roughly 1.67 L/min per step. Documented in README.md.
static uint16_t lpm_to_zcl_flow(float lpm)
{
    return (uint16_t)lroundf(lpm * 0.6f);
}

void flow_task(void *pvParameters)
{
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
        setup_channel(&channels[i]);
    }

    const float minutes_per_interval = REPORT_INTERVAL_MS / 60000.0f;

    while (1) {
        vTaskDelay(REPORT_INTERVAL_MS / portTICK_PERIOD_MS);

        for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
            flow_channel_t *ch = &channels[i];
            int pulses = 0;
            pcnt_unit_get_count(ch->pcnt_unit, &pulses);
            pcnt_unit_clear_count(ch->pcnt_unit);

            // Zero flow is a normal, expected reading (pump off / valve
            // closed) - unlike the temperature sensor's out-of-range check,
            // this is always reported rather than skipped.
            float lpm = (pulses > 0) ? (pulses / ch->pulses_per_liter) / minutes_per_interval : 0.0f;

            if (lpm < FLOW_MIN_LPM || lpm > FLOW_MAX_LPM) {
                ESP_LOGW(TAG, "%s: implausible flow %.2f L/min (%d pulses), not reporting", ch->name, lpm, pulses);
                continue;
            }

            ESP_LOGI(TAG, "%s (endpoint %d): %.2f L/min (%d pulses)", ch->name, ch->endpoint, lpm, pulses);
            uint16_t zb_flow = lpm_to_zcl_flow(lpm);
            reportAttribute(ch->endpoint, ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT, ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_VALUE_ID, &zb_flow);
        }
    }
}
