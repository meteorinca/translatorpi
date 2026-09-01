#include "time_sync.h"
#include <sys/time.h>
#include <string.h>
#include "esp_sntp.h"
#include "esp_log.h"

static const char *TAG = "TIME_SYNC";
static bool s_is_synced = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    s_is_synced = true;
    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "SNTP time synchronized: %s (Epoch: %ld)", strftime_buf, (long)now);
}

esp_err_t time_sync_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP client...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // Set timezone to UTC or user configurable
    setenv("TZ", "UTC", 1);
    tzset();

    return ESP_OK;
}

void time_sync_set_epoch(time_t epoch_sec)
{
    struct timeval tv = {
        .tv_sec = epoch_sec,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);
    s_is_synced = true;

    struct tm timeinfo;
    localtime_r(&epoch_sec, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Manual/WebUI epoch time set: %s (Epoch: %ld)", strftime_buf, (long)epoch_sec);
}

time_t time_sync_get_epoch(void)
{
    time_t now;
    time(&now);
    return now;
}

bool time_sync_is_synced(void)
{
    return s_is_synced;
}

void time_sync_get_time_str(char *buf, size_t max_len)
{
    time_t now = time_sync_get_epoch();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(buf, max_len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}
