#pragma once

#include <stdint.h>
#include "esp_zigbee_core.h"
#include "zboss_api.h"

/* Zigbee configuration */
#define MAX_CHILDREN 10                                                   /* max number of connected devices (router role) */
#define INSTALLCODE_POLICY_ENABLE false /* enable the install code policy for security */
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK /* Zigbee primary channel mask use in the example */

// This device is always USB-powered, so it joins the network as a Router
// instead of a sleepy End Device - no light/deep sleep handling needed, and
// it helps strengthen the mesh for battery-powered end devices nearby.
#define ESP_ZB_ZR_CONFIG()                                \
    {                                                     \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,         \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE, \
        .nwk_cfg.zczr_cfg = {                             \
            .max_children = MAX_CHILDREN,                 \
        },                                                \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()    \
    {                                    \
        .radio_mode = ZB_RADIO_MODE_NATIVE, \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                       \
    {                                                      \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }


void reportAttribute(uint8_t endpoint, uint16_t clusterID, uint16_t attributeID, void *value);
