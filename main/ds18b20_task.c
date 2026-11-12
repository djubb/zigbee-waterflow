#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zigbee.h"
#include "tasks.h"
#include "config.h"

static const char *TAG = "ds18b20_task";

static onewire_bus_handle_t bus = NULL;
static ds18b20_device_handle_t sensors[MAX_DS18B20];
static uint8_t sensor_count = 0;

// Scan the 1-Wire bus once at boot, keep only DS18B20 devices and sort them by
// their 64-bit ROM address so that endpoint[i] always maps to the same
// physical sensor across reboots (as long as the same sensors stay wired).
void ds18b20_scan_bus(void)
{
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = ONE_WIRE_GPIO,
        .flags = { .en_pull_up = false }, // add an external pull-up if your wiring doesn't already have one
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10, // 1 byte ROM command + 8 byte ROM number + 1 byte device command
    };
    if (onewire_new_bus_rmt(&bus_config, &rmt_config, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install 1-Wire bus on GPIO%d", ONE_WIRE_GPIO);
        return;
    }

    ds18b20_device_handle_t found[MAX_DS18B20];
    onewire_device_address_t found_addr[MAX_DS18B20];
    uint8_t found_count = 0;

    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_device;
    esp_err_t search_result = ESP_OK;

    ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
    ESP_LOGI(TAG, "Searching for DS18B20 sensors on GPIO%d...", ONE_WIRE_GPIO);
    do {
        search_result = onewire_device_iter_get_next(iter, &next_device);
        if (search_result == ESP_OK) {
            ds18b20_config_t ds_cfg = {};
            ds18b20_device_handle_t handle;
            if (ds18b20_new_device_from_enumeration(&next_device, &ds_cfg, &handle) == ESP_OK) {
                if (found_count < MAX_DS18B20) {
                    found[found_count] = handle;
                    found_addr[found_count] = next_device.address;
                    ESP_LOGI(TAG, "Found DS18B20[%d], address: %016llX", found_count, next_device.address);
                    found_count++;
                } else {
                    ESP_LOGW(TAG, "More than %d DS18B20 found, ignoring extra sensor %016llX", MAX_DS18B20, next_device.address);
                    ds18b20_del_device(handle);
                }
            } else {
                ESP_LOGW(TAG, "Found an unknown 1-Wire device, address: %016llX", next_device.address);
            }
        }
    } while (search_result != ESP_ERR_NOT_FOUND);
    ESP_ERROR_CHECK(onewire_del_device_iter(iter));

    // insertion sort by ascending ROM address (N is at most a handful of sensors)
    for (uint8_t i = 1; i < found_count; i++) {
        for (int8_t j = i; j > 0 && found_addr[j - 1] > found_addr[j]; j--) {
            onewire_device_address_t tmp_addr = found_addr[j - 1];
            found_addr[j - 1] = found_addr[j];
            found_addr[j] = tmp_addr;
            ds18b20_device_handle_t tmp_handle = found[j - 1];
            found[j - 1] = found[j];
            found[j] = tmp_handle;
        }
    }

    for (uint8_t i = 0; i < found_count; i++) {
        sensors[i] = found[i];
    }
    sensor_count = found_count;

    if (sensor_count < MAX_DS18B20) {
        ESP_LOGW(TAG, "Expected %d DS18B20 sensors, found %d - missing sensors won't report", MAX_DS18B20, sensor_count);
    } else {
        ESP_LOGI(TAG, "Found all %d DS18B20 sensors", sensor_count);
    }
}

void ds18b20_task(void *pvParameters)
{
    uint8_t endpoints[MAX_DS18B20] = DS18B20_ENDPOINTS;

    if (sensor_count == 0) {
        ESP_LOGE(TAG, "No DS18B20 sensors found, nothing to report");
    }

    while (1) {
        if (sensor_count > 0 && bus != NULL) {
            esp_err_t err = ds18b20_trigger_temperature_conversion_for_all(bus);
            if (err == ESP_OK) {
                for (uint8_t i = 0; i < sensor_count; i++) {
                    float temperature;
                    err = ds18b20_get_temperature(sensors[i], &temperature);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "Sensor[%d]: failed to read temperature: %s", i, esp_err_to_name(err));
                        continue;
                    }
                    ESP_LOGI(TAG, "Sensor[%d] (endpoint %d): %.2f C", i, endpoints[i], temperature);
                    if (temperature >= TEMP_MIN && temperature <= TEMP_MAX) {
                        int16_t zb_temperature = (int16_t)lroundf(temperature * 100.0f);
                        reportAttribute(endpoints[i], ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &zb_temperature);
                    } else {
                        ESP_LOGW(TAG, "Sensor[%d]: implausible value %.2f C, not reporting", i, temperature);
                    }
                }
            } else {
                ESP_LOGW(TAG, "Failed to trigger temperature conversion: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(TEMP_REPORT_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}
