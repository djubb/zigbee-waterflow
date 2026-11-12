#include "nvs_flash.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zigbee.h"
#include "tasks.h"
#include "driver/gpio.h"
#include "led_strip.h"

static const char *TAG = "WATERFLOW";

TaskHandle_t xFlowTask;
TaskHandle_t xDs18b20Task;
TaskHandle_t xZigbeeTask;

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee bdb commissioning");
}

static void start_sensor_reporting(void)
{
    static bool started = false;
    if (!started) {
        xTaskCreate(flow_task, "flow_task", 4096, NULL, 5, &xFlowTask);
        xTaskCreate(ds18b20_task, "ds18b20_task", 4096, NULL, 5, &xDs18b20Task);
        started = true;
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type)
    {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK)
        {
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted, already joined");
                start_sensor_reporting();
            }
        }
        else
        {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s). Resetting the esp after 2s", esp_err_to_name(err_status));
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            esp_restart();
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK)
        {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            start_sensor_reporting();
        }
        else
        {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static void turn_off_onboard_led(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = 8,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t led_strip;
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip) == ESP_OK) {
        led_strip_clear(led_strip);
        led_strip_del(led_strip);
    }
}

void configure_internal_antenna(void) {
    gpio_reset_pin(GPIO_NUM_3);
    gpio_reset_pin(GPIO_NUM_14);
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_3, 0);//turn on antenna selection
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_14, 0);//use internal antenna
}

void app_main(void)
{
    turn_off_onboard_led();
    configure_internal_antenna();
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    /* scan the 1-Wire bus once, before the Zigbee stack starts up */
    ds18b20_scan_bus();
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, &xZigbeeTask);
}
