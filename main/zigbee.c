#include "freertos/FreeRTOS.h"
#include "esp_zigbee_core.h"
#include "esp_log.h"

static const char *TAG = "zigbee";

// Updates the attribute's stored value and lets the stack's own reporting
// engine (configured once via esp_zb_zcl_update_reporting_info in
// zigbee_task.c) decide when to actually transmit a Report Attributes
// frame. Calling esp_zb_zcl_report_attr_cmd_req() explicitly on every read
// instead reliably crashed with a ZBOSS assertion in
// zcl_general_commands.c:612 on this esp-zigbee-lib version, regardless of
// reporting-table setup or the manuf_code field - so actual report cadence
// is whatever ZHA negotiates (its own Configure Reporting during pairing
// can override our configured default), not guaranteed to match
// REPORT_INTERVAL_MS exactly.
void reportAttribute(uint8_t endpoint, uint16_t clusterID, uint16_t attributeID, void *value)
{
    // The Zigbee stack runs in its own task - any call into esp-zigbee-lib
    // from another task (like the sensor task) must be wrapped in
    // esp_zb_lock_acquire/release.
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(endpoint, clusterID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attributeID, value, false);
    esp_zb_lock_release();
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set_attribute_val cluster=0x%04x attr=0x%04x: status 0x%02x", clusterID, attributeID, status);
    }
}
