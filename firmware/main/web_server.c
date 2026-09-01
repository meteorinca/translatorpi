#include "web_server.h"
#include "config.h"
#include "state_machine.h"
#include "time_sync.h"
#include "mdns_manager.h"
#include "wifi_manager.h"
#include "ws_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "WEB_SERVER";
static httpd_handle_t s_server = NULL;

static const char HTML_INDEX[] = 
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>ESP32-C3 Translator Node</title>"
"<style>"
":root { --bg:#0f172a; --card:#1e293b; --text:#f8fafc; --accent:#38bdf8; --green:#22c55e; --amber:#f59e0b; --red:#ef4444; }"
"* { box-sizing: border-box; margin:0; padding:0; font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif; }"
"body { background: var(--bg); color: var(--text); padding: 16px; display:flex; justify-content:center; }"
".container { max-width: 500px; width: 100%; display: flex; flex-direction: column; gap: 14px; }"
".card { background: var(--card); border-radius: 14px; padding: 16px; border: 1px solid rgba(255,255,255,0.08); box-shadow: 0 4px 20px rgba(0,0,0,0.3); }"
".header { display: flex; justify-content: space-between; align-items: center; }"
".badge { font-size: 12px; font-weight: 700; padding: 4px 10px; border-radius: 20px; text-transform: uppercase; }"
".badge-ready { background: rgba(34,197,94,0.15); color: var(--green); border: 1px solid var(--green); }"
".badge-active { background: rgba(56,189,248,0.15); color: var(--accent); border: 1px solid var(--accent); }"
".badge-wait { background: rgba(245,158,11,0.15); color: var(--amber); border: 1px solid var(--amber); }"
".grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }"
".btn { background: #334155; color: #fff; border: none; border-radius: 8px; padding: 12px; font-size: 14px; font-weight: 600; cursor: pointer; transition: 0.15s ease; display:flex; align-items:center; justify-content:center; gap:6px; }"
".btn:active { transform: scale(0.97); }"
".btn-primary { background: #0284c7; }"
".btn-primary:hover { background: #0369a1; }"
".btn-amber { background: #d97706; }"
".btn-amber:hover { background: #b45309; }"
".btn-red { background: #dc2626; }"
".btn-ptt { width: 100%; padding: 18px; font-size: 16px; background: #dc2626; border-radius: 12px; margin-top: 8px; }"
".btn-ptt.recording { background: #16a34a; animation: pulse 1s infinite; }"
"@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }"
".lang-bar { display: flex; align-items: center; justify-content: space-between; background: #0f172a; padding: 12px; border-radius: 10px; margin: 10px 0; border: 1px solid rgba(255,255,255,0.05); font-size: 18px; font-weight: 700; }"
".stat { font-size: 12px; color: #94a3b8; }"
".stat span { color: #f8fafc; font-weight: 600; }"
"select { background:#0f172a; color:#fff; border:1px solid #475569; padding:8px; border-radius:6px; font-size:14px; width:100%; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"  <div class='card'>"
"    <div class='header'>"
"      <div>"
"        <h2 id='device_title'>Translator Node</h2>"
"        <div class='stat'>mDNS: <span id='mdns_name'>...</span></div>"
"      </div>"
"      <span id='status_badge' class='badge badge-ready'>READY</span>"
"    </div>"
"    <div class='grid' style='margin-top:14px;'>"
"      <div class='stat'>IP: <span id='ip_addr'>...</span></div>"
"      <div class='stat'>Heap: <span id='free_heap'>...</span> KB</div>"
"      <div class='stat'>Wi-Fi RSSI: <span id='wifi_rssi'>...</span> dBm</div>"
"      <div class='stat'>Uptime: <span id='uptime'>...</span>s</div>"
"    </div>"
"  </div>"

"  <div class='card'>"
"    <h3 style='margin-bottom:6px;'>Time & Epoch Sync</h3>"
"    <div class='stat' style='font-size:14px; margin-bottom:10px;'>RTC Time: <span id='rtc_time' style='color:var(--accent);'>...</span></div>"
"    <button class='btn btn-primary' style='width:100%;' onclick='syncBrowserTime()'>Sync Browser Time (Epoch)</button>"
"  </div>"

"  <div class='card'>"
"    <h3>Language Configuration</h3>"
"    <div class='lang-bar'>"
"      <span id='src_disp'>EN</span>"
"      <button class='btn btn-amber' style='padding:6px 14px;' onclick='swapLang()'>Swap</button>"
"      <span id='dst_disp'>ZH</span>"
"    </div>"
"    <div class='grid'>"
"      <div>"
"        <label class='stat'>Source Lang</label>"
"        <select id='sel_src' onchange='changeLang()'>"
"          <option value='en'>English (en)</option>"
"          <option value='zh'>Chinese (zh)</option>"
"          <option value='es'>Spanish (es)</option>"
"          <option value='ja'>Japanese (ja)</option>"
"          <option value='ar'>Arabic (ar)</option>"
"          <option value='ko'>Korean (ko)</option>"
"        </select>"
"      </div>"
"      <div>"
"        <label class='stat'>Target Lang</label>"
"        <select id='sel_dst' onchange='changeLang()'>"
"          <option value='zh'>Chinese (zh)</option>"
"          <option value='en'>English (en)</option>"
"          <option value='es'>Spanish (es)</option>"
"          <option value='ja'>Japanese (ja)</option>"
"          <option value='ar'>Arabic (ar)</option>"
"          <option value='ko'>Korean (ko)</option>"
"        </select>"
"      </div>"
"    </div>"
"  </div>"

"  <div class='card'>"
"    <h3>Live Audio PTT Test</h3>"
"    <button id='ptt_btn' class='btn btn-ptt' onmousedown='pttPress()' onmouseup='pttRelease()' ontouchstart='pttPress()' ontouchend='pttRelease()'>Hold to Record Voice</button>"
"  </div>"

"  <div class='card'>"
"    <button class='btn btn-red' style='width:100%;' onclick='rebootDevice()'>Reboot Device</button>"
"  </div>"
"</div>"

"<script>"
"let isRecording = false;"
"async function fetchStatus() {"
"  try {"
"    const res = await fetch('/api/status');"
"    const d = await res.json();"
"    document.getElementById('device_title').innerText = d.device + ' #' + d.number;"
"    document.getElementById('mdns_name').innerText = 'http://' + d.hostname + '.local';"
"    document.getElementById('ip_addr').innerText = d.ip;"
"    document.getElementById('free_heap').innerText = Math.round(d.heap / 1024);"
"    document.getElementById('wifi_rssi').innerText = d.rssi;"
"    document.getElementById('uptime').innerText = d.uptime;"
"    document.getElementById('rtc_time').innerText = d.time;"
"    document.getElementById('src_disp').innerText = d.src.toUpperCase();"
"    document.getElementById('dst_disp').innerText = d.dst.toUpperCase();"
"    document.getElementById('sel_src').value = d.src;"
"    document.getElementById('sel_dst').value = d.dst;"
"    const badge = document.getElementById('status_badge');"
"    badge.innerText = d.state;"
"    badge.className = 'badge ' + (d.state==='READY'?'badge-ready':(d.state==='RECORDING'?'badge-active':'badge-wait'));"
"  } catch(e) {}"
"}"
"async function syncBrowserTime() {"
"  const epoch = Math.floor(Date.now() / 1000);"
"  await fetch('/api/time?epoch=' + epoch, {method:'POST'});"
"  fetchStatus();"
"}"
"async function swapLang() {"
"  await fetch('/api/swap', {method:'POST'});"
"  fetchStatus();"
"}"
"async function changeLang() {"
"  const src = document.getElementById('sel_src').value;"
"  const dst = document.getElementById('sel_dst').value;"
"  await fetch('/api/lang?src=' + src + '&dst=' + dst, {method:'POST'});"
"  fetchStatus();"
"}"
"async function pttPress() {"
"  if(!isRecording) {"
"    isRecording = true;"
"    document.getElementById('ptt_btn').innerText = 'Recording (Release to Send)...';"
"    document.getElementById('ptt_btn').classList.add('recording');"
"    await fetch('/api/record/start', {method:'POST'});"
"  }"
"}"
"async function pttRelease() {"
"  if(isRecording) {"
"    isRecording = false;"
"    document.getElementById('ptt_btn').innerText = 'Hold to Record Voice';"
"    document.getElementById('ptt_btn').classList.remove('recording');"
"    await fetch('/api/record/stop', {method:'POST'});"
"    fetchStatus();"
"  }"
"}"
"async function rebootDevice() {"
"  if(confirm('Reboot ESP32?')) {"
"    await fetch('/api/reboot', {method:'POST'});"
"    alert('Rebooting...');"
"  }"
"}"
"// Auto-sync browser epoch on first load\n"
"window.addEventListener('load', () => {"
"  syncBrowserTime();"
"  fetchStatus();"
"  setInterval(fetchStatus, 2000);"
"});"
"</script>"
"</body>"
"</html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_INDEX, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char src_lang[8] = {0}, dst_lang[8] = {0};
    state_machine_get_languages(src_lang, dst_lang);

    char time_str[64] = {0};
    time_sync_get_time_str(time_str, sizeof(time_str));

    wifi_ap_record_t ap_info;
    int rssi = -100;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    char ip_str[32] = "0.0.0.0";
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    }

    char json[512];
    snprintf(json, sizeof(json),
             "{\"device\":\"%s\",\"number\":%d,\"hostname\":\"%s\",\"ip\":\"%s\","
             "\"rssi\":%d,\"heap\":%lu,\"uptime\":%lld,\"time\":\"%s\",\"epoch\":%ld,"
             "\"src\":\"%s\",\"dst\":\"%s\",\"state\":\"%s\"}",
             DEVICE_NAME_PREFIX, DEVICE_NUMBER, mdns_manager_get_hostname(), ip_str,
             rssi, (unsigned long)esp_get_free_heap_size(),
             (long long)(esp_timer_get_time() / 1000000ULL),
             time_str, (long)time_sync_get_epoch(),
             src_lang, dst_lang, state_machine_get_current_state_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t time_handler(httpd_req_t *req)
{
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(query, "epoch", param, sizeof(param)) == ESP_OK) {
            time_t epoch = (time_t)atol(param);
            if (epoch > 1000000) {
                time_sync_set_epoch(epoch);
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t swap_handler(httpd_req_t *req)
{
    state_machine_trigger_swap();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"action\":\"swap\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t lang_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char src[8] = {0}, dst[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "src", src, sizeof(src));
        httpd_query_key_value(query, "dst", dst, sizeof(dst));
        if (strlen(src) > 0 || strlen(dst) > 0) {
            state_machine_set_languages(src, dst);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t record_start_handler(httpd_req_t *req)
{
    state_machine_web_record_start();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"recording\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t record_stop_handler(httpd_req_t *req)
{
    state_machine_web_record_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"recording\":false}", HTTPD_RESP_USE_STRLEN);
}

static void delayed_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"status\":\"rebooting\"}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting HTTP Web Dashboard on port %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_register_uri_handler(s_server, &uri_index);

    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_time_post = { .uri = "/api/time", .method = HTTP_POST, .handler = time_handler };
    httpd_register_uri_handler(s_server, &uri_time_post);

    httpd_uri_t uri_time_get = { .uri = "/api/time", .method = HTTP_GET, .handler = time_handler };
    httpd_register_uri_handler(s_server, &uri_time_get);

    httpd_uri_t uri_swap = { .uri = "/api/swap", .method = HTTP_POST, .handler = swap_handler };
    httpd_register_uri_handler(s_server, &uri_swap);

    httpd_uri_t uri_lang = { .uri = "/api/lang", .method = HTTP_POST, .handler = lang_handler };
    httpd_register_uri_handler(s_server, &uri_lang);

    httpd_uri_t uri_rec_start = { .uri = "/api/record/start", .method = HTTP_POST, .handler = record_start_handler };
    httpd_register_uri_handler(s_server, &uri_rec_start);

    httpd_uri_t uri_rec_stop = { .uri = "/api/record/stop", .method = HTTP_POST, .handler = record_stop_handler };
    httpd_register_uri_handler(s_server, &uri_rec_stop);

    httpd_uri_t uri_reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler };
    httpd_register_uri_handler(s_server, &uri_reboot);

    ESP_LOGI(TAG, "Web Dashboard running at http://%s.local/ or http://<ESP_IP>/", mdns_manager_get_hostname());
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
