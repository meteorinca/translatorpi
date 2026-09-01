#include "mdns_manager.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "MDNS_MGR";
static char s_hostname[32] = {0};

esp_err_t mdns_manager_init(void)
{
    snprintf(s_hostname, sizeof(s_hostname), "%s%d", MDNS_HOSTNAME_PREFIX, DEVICE_NUMBER);

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(mdns_hostname_set(s_hostname));
    ESP_LOGI(TAG, "mDNS hostname set to: http://%s.local", s_hostname);

    char instance_name[64];
    snprintf(instance_name, sizeof(instance_name), "Translator Node #%d", DEVICE_NUMBER);
    ESP_ERROR_CHECK(mdns_instance_name_set(instance_name));

    // Register HTTP Web Dashboard service on port 80
    mdns_txt_item_t service_txt_data[] = {
        {"device", DEVICE_NAME_STR},
        {"number", "1"},
        {"version", "1.0.0"},
    };
    char num_str[8];
    snprintf(num_str, sizeof(num_str), "%d", DEVICE_NUMBER);
    service_txt_data[1].value = num_str;

    ESP_ERROR_CHECK(mdns_service_add("Translator-WebUI", "_http", "_tcp", 80, service_txt_data, 3));
    ESP_LOGI(TAG, "mDNS HTTP service registered on port 80");

    return ESP_OK;
}

const char* mdns_manager_get_hostname(void)
{
    return s_hostname;
}
