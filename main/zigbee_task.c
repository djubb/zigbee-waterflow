#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zigbee.h"
#include "tasks.h"
#include "config.h"

#define DEFINE_PSTRING(var, str)   \
    const struct                   \
    {                              \
        unsigned char len;         \
        char content[sizeof(str)]; \
    }(var) = {sizeof(str) - 1, (str)}

static const char *TAG = "zigbee_task";

typedef struct {
    uint8_t endpoint;
    const char *model_suffix;
} flow_endpoint_desc_t;

static const flow_endpoint_desc_t FLOW_ENDPOINTS[] = {
    {FLOW_SUPPLY_ENDPOINT, "SUPPLY"},
    {FLOW_RETURN_ENDPOINT, "RETURN"},
};
#define FLOW_ENDPOINT_COUNT (sizeof(FLOW_ENDPOINTS) / sizeof(FLOW_ENDPOINTS[0]))

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    ESP_LOGI(TAG, "Received message: endpoint(0x%x), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id)
    {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void create_flow_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint, const char *model_suffix)
{
    // ------------------------------ Cluster BASIC ------------------------------
    esp_zb_basic_cluster_cfg_t basic_cluster_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x04, // DC source (always USB powered)
    };
    static uint32_t ApplicationVersion = 0x0001;
    static uint32_t StackVersion = 0x0002;
    static uint32_t HWVersion = 0x0001;
    DEFINE_PSTRING(ManufacturerName, "DIY");
    char model_name[16];
    snprintf(model_name, sizeof(model_name), "YF-B5-%s", model_suffix);
    struct
    {
        unsigned char len;
        char content[16];
    } ModelIdentifier = {(unsigned char)strlen(model_name), {0}};
    memcpy(ModelIdentifier.content, model_name, strlen(model_name));
    DEFINE_PSTRING(DateCode, "20261109");

    esp_zb_attribute_list_t *esp_zb_basic_cluster = esp_zb_basic_cluster_create(&basic_cluster_cfg);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &ApplicationVersion);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &StackVersion);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &HWVersion);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)&ManufacturerName);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)&ModelIdentifier);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, (void *)&DateCode);

    // ------------------------------ Cluster IDENTIFY ------------------------------
    esp_zb_identify_cluster_cfg_t identify_cluster_cfg = {
        .identify_time = 0,
    };
    esp_zb_attribute_list_t *esp_zb_identify_cluster = esp_zb_identify_cluster_create(&identify_cluster_cfg);

    // ------------------------------ Cluster Flow Measurement ------------------------------
    // MeasuredValue is defined by the ZCL spec as 10 x flow in m3/h. Firmware
    // writes flow_task.c's L/min reading through that same scaling (see
    // lpm_to_zcl_flow() there) - min/max here bound what's considered valid.
    esp_zb_flow_meas_cluster_cfg_t flow_meas_cfg = {
        .measured_value = 0,
        .min_value = 0,
        .max_value = (int16_t)(FLOW_MAX_LPM * 0.6f),
    };
    esp_zb_attribute_list_t *esp_zb_flow_meas_cluster = esp_zb_flow_meas_cluster_create(&flow_meas_cfg);

    // ------------------------------ Create cluster list ------------------------------
    esp_zb_cluster_list_t *esp_zb_cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(esp_zb_cluster_list, esp_zb_basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(esp_zb_cluster_list, esp_zb_identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_flow_meas_cluster(esp_zb_cluster_list, esp_zb_flow_meas_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        // ZCL HA device IDs have no dedicated "Flow Sensor" entry (unlike
        // ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID for temperature) - ZHA builds
        // its entity from the clusters present on the endpoint regardless of
        // device_id, so SIMPLE_SENSOR is the usual choice for this case.
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep_list, esp_zb_cluster_list, endpoint_config);
}

static void create_temp_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint, uint8_t index)
{
    // ------------------------------ Cluster BASIC ------------------------------
    esp_zb_basic_cluster_cfg_t basic_cluster_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x04, // DC source (always USB powered)
    };
    static uint32_t ApplicationVersion = 0x0001;
    static uint32_t StackVersion = 0x0002;
    static uint32_t HWVersion = 0x0001;
    DEFINE_PSTRING(ManufacturerName, "DIY");
    static char model_buf[MAX_DS18B20][16];
    snprintf(model_buf[index], sizeof(model_buf[index]), "DS18B20-%d", index + 1);
    struct
    {
        unsigned char len;
        char content[16];
    } ModelIdentifier = {(unsigned char)strlen(model_buf[index]), {0}};
    memcpy(ModelIdentifier.content, model_buf[index], strlen(model_buf[index]));
    DEFINE_PSTRING(DateCode, "20261109");

    esp_zb_attribute_list_t *esp_zb_basic_cluster = esp_zb_basic_cluster_create(&basic_cluster_cfg);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &ApplicationVersion);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &StackVersion);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &HWVersion);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)&ManufacturerName);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)&ModelIdentifier);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, (void *)&DateCode);

    // ------------------------------ Cluster IDENTIFY ------------------------------
    esp_zb_identify_cluster_cfg_t identify_cluster_cfg = {
        .identify_time = 0,
    };
    esp_zb_attribute_list_t *esp_zb_identify_cluster = esp_zb_identify_cluster_create(&identify_cluster_cfg);

    // ------------------------------ Cluster Temperature ------------------------------
    esp_zb_temperature_meas_cluster_cfg_t temperature_meas_cfg = {
        .measured_value = 0,
        .min_value = TEMP_MIN * 100,
        .max_value = TEMP_MAX * 100,
    };
    esp_zb_attribute_list_t *esp_zb_temperature_meas_cluster = esp_zb_temperature_meas_cluster_create(&temperature_meas_cfg);

    // ------------------------------ Create cluster list ------------------------------
    esp_zb_cluster_list_t *esp_zb_cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(esp_zb_cluster_list, esp_zb_basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(esp_zb_cluster_list, esp_zb_identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_temperature_meas_cluster(esp_zb_cluster_list, esp_zb_temperature_meas_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep_list, esp_zb_cluster_list, endpoint_config);
}

// Pre-register a reporting-table entry for a measurement attribute. We don't
// rely on the min/max/delta values here to control our actual reporting
// cadence (ZHA's own Configure Reporting, sent once during pairing, can -
// and does - override them, and isn't repeated on later reboots) - this
// call only exists so the table entry is present at all. Without it,
// esp_zb_zcl_report_attr_cmd_req() hits a hard ZBOSS assertion
// (zcl_general_commands.c:612) and the device crash-reboot-loops the moment
// a sensor task tries to send its first explicit report.
//
// Shared between flow (Flow Measurement cluster) and temperature
// (Temperature Measurement cluster) endpoints - same mechanism, different
// cluster/attribute IDs and cadence/delta per sensor type.
static void ensure_reporting_entry(uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id, uint16_t max_interval_s, uint16_t delta_u16)
{
    esp_zb_zcl_reporting_info_t reporting_info = {
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep = endpoint,
        .cluster_id = cluster_id,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info.min_interval = 1,
        .u.send_info.max_interval = max_interval_s,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = max_interval_s,
        .u.send_info.delta.u16 = delta_u16,
        .attr_id = attr_id,
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&reporting_info);
}

/* initialize Zigbee stack with Zigbee router config */
void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    uint8_t temp_endpoints[MAX_DS18B20] = DS18B20_ENDPOINTS;

    esp_zb_ep_list_t *esp_zb_ep_list = esp_zb_ep_list_create();
    for (uint8_t i = 0; i < FLOW_ENDPOINT_COUNT; i++) {
        create_flow_endpoint(esp_zb_ep_list, FLOW_ENDPOINTS[i].endpoint, FLOW_ENDPOINTS[i].model_suffix);
    }
    for (uint8_t i = 0; i < MAX_DS18B20; i++) {
        create_temp_endpoint(esp_zb_ep_list, temp_endpoints[i], i);
    }

    // ------------------------------ Register Device ------------------------------
    esp_zb_device_register(esp_zb_ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    for (uint8_t i = 0; i < FLOW_ENDPOINT_COUNT; i++) {
        ensure_reporting_entry(FLOW_ENDPOINTS[i].endpoint, ESP_ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT,
                                ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_VALUE_ID, FLOW_REPORT_INTERVAL_MS / 1000, 1);
    }
    for (uint8_t i = 0; i < MAX_DS18B20; i++) {
        ensure_reporting_entry(temp_endpoints[i], ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                                ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, TEMP_REPORT_INTERVAL_MS / 1000, 10 /* 0.1 C */);
    }

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}
