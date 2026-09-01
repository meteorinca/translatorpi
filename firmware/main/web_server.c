#include "web_server.h"
#include "config.h"
#include "state_machine.h"
#include "time_sync.h"
#include "mdns_manager.h"
#include "wifi_manager.h"
#include "ws_client.h"
#include "ota_mgr.h"
#include "dog_actions.h"
#include "sound_player.h"
#include "led.h"

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
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>Dogbot Translator</title>"
"<style>"
"* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; font-family: system-ui, -apple-system, sans-serif; }"
"body { background: #0d0d1a; color: #e0e0f0; padding: 14px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }"
".container { width: 100%; max-width: 460px; display: flex; flex-direction: column; gap: 12px; }"
".card { background: #131326; border: 1px solid #1e1e3a; border-radius: 14px; padding: 16px; box-shadow: 0 4px 24px rgba(0,0,0,0.4); }"
"h1 { font-size: 20px; font-weight: 700; color: #fff; text-align: center; margin-bottom: 4px; letter-spacing: 0.5px; }"
"h2 { font-size: 15px; font-weight: 700; color: #a0a0d0; margin-bottom: 10px; display: flex; justify-content: space-between; align-items: center; }"
".status-badge { font-size: 11px; font-weight: 700; padding: 3px 10px; border-radius: 20px; background: rgba(0,229,160,0.15); color: #00e5a0; border: 1px solid #00e5a0; text-transform: uppercase; }"
".status-badge.wait { background: rgba(245,158,11,0.15); color: #f59e0b; border-color: #f59e0b; }"
".status-badge.err { background: rgba(239,68,68,0.15); color: #ef4444; border-color: #ef4444; }"
".time-big { font-size: 32px; font-weight: 700; color: #fff; font-family: monospace; text-align: center; margin: 6px 0; }"
".epoch-line { font-size: 12px; color: #8888b0; text-align: center; }"
".action-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin-top: 8px; }"
".act-btn { padding: 10px 4px; font-size: 12px; font-weight: 600; background: linear-gradient(135deg, #1a1a35, #20203d); border: 1px solid #2a2a50; border-radius: 10px; color: #a0a0d0; cursor: pointer; transition: all 0.15s; text-align: center; }"
".act-btn:hover, .act-btn:active { background: linear-gradient(135deg, #7c6af7, #5b4de8); border-color: #7c6af7; color: #fff; transform: scale(0.97); }"
".btn-primary { width: 100%; padding: 12px; border: none; border-radius: 10px; background: linear-gradient(135deg, #7c6af7, #5b4de8); color: #fff; font-size: 14px; font-weight: 700; cursor: pointer; transition: 0.15s; }"
".btn-primary:active { transform: scale(0.98); opacity: 0.9; }"
".btn-amber { background: linear-gradient(135deg, #d97706, #b45309); border: none; color: #fff; padding: 6px 14px; border-radius: 8px; font-weight: 700; cursor: pointer; }"
".btn-red { background: #dc2626; color: #fff; border: none; border-radius: 10px; padding: 12px; font-size: 14px; font-weight: 700; cursor: pointer; width: 100%; }"
".btn-ptt { width: 100%; padding: 16px; border: none; border-radius: 12px; background: #dc2626; color: #fff; font-size: 16px; font-weight: 700; cursor: pointer; margin-top: 8px; transition: 0.15s; }"
".btn-ptt.recording { background: #00e5a0; color: #0a0a1e; animation: pulse 1s infinite; }"
"@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }"
".lang-bar { display: flex; align-items: center; justify-content: space-between; background: #0a0a1e; padding: 12px; border-radius: 10px; margin: 10px 0; border: 1px solid #1e1e3a; font-size: 18px; font-weight: 700; }"
".grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }"
"select, input[type='text'] { background: #0a0a1e; color: #fff; border: 1px solid #2a2a50; padding: 9px; border-radius: 8px; font-size: 13px; width: 100%; outline: none; }"
".bar-box { background: #0a0a1e; border-radius: 6px; height: 8px; margin-bottom: 8px; overflow: hidden; }"
"#ota-bar { width: 0%; height: 100%; background: linear-gradient(90deg, #00e5a0, #7c6af7); transition: width 0.2s; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"  <h1>Dogbot Translator Node</h1>"

"  <div class='card' style='text-align:center;'>"
"    <div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:4px;'>"
"      <span id='node_title' style='font-size:13px; color:#8888b0;'>...</span>"
"      <span id='conn-badge' class='status-badge'>Online</span>"
"    </div>"
"    <div class='time-big' id='clock'>--:--:--</div>"
"    <div class='epoch-line'>"
"      Epoch: <span id='epoch'>-</span> &nbsp;&bull;&nbsp; Synced: <span id='synced' style='color:#00e5a0;'>no</span>"
"    </div>"
"    <div style='display:flex; justify-content:space-around; font-size:11px; color:#8888b0; margin-top:10px;'>"
"      <div>IP: <span id='ip_addr' style='color:#fff;'>...</span></div>"
"      <div>Heap: <span id='free_heap' style='color:#fff;'>...</span>KB</div>"
"      <div>RSSI: <span id='wifi_rssi' style='color:#fff;'>...</span>dBm</div>"
"    </div>"
"  </div>"

"  <div class='card'>"
"    <h2><span>Dogbot Actions</span> <button class='act-btn' style='padding:4px 10px;' onclick=\"dog('bark')\">&nbsp;Bark</button></h2>"
"    <div class='action-grid' id='action-grid'></div>"
"  </div>"

"  <div class='card'>"
"    <h2>Voice Translator</h2>"
"    <div class='lang-bar'>"
"      <span id='src_disp'>EN</span>"
"      <button class='btn-amber' onclick='swapLang()'>Cycle Lang</button>"
"      <span id='dst_disp'>ES</span>"
"    </div>"
"    <div class='grid'>"
"      <div>"
"        <label style='font-size:11px; color:#8888b0;'>Source (Fixed EN)</label>"
"        <select id='sel_src' onchange='changeLang()' disabled>"
"          <option value='en' selected>English (en)</option>"
"        </select>"
"      </div>"
"      <div>"
"        <label style='font-size:11px; color:#8888b0;'>Target</label>"
"        <select id='sel_dst' onchange='changeLang()'>"
"          <option value='es' selected>Spanish (es)</option>"
"          <option value='zh'>Chinese (zh)</option>"
"          <option value='ja'>Japanese (ja)</option>"
"          <option value='ar'>Arabic (ar)</option>"
"          <option value='ko'>Korean (ko)</option>"
"        </select>"
"      </div>"
"    </div>"
"    <button id='ptt_btn' class='btn-ptt' onmousedown='pttPress()' onmouseup='pttRelease()' ontouchstart='pttPress()' ontouchend='pttRelease()'>Hold to Record Voice</button>"
"  </div>"

"  <div class='card'>"
"    <h2>Audio &amp; Sounds</h2>"
"    <div class='action-grid'>"
"      <button class='act-btn' onclick=\"playSound('bark')\">Dog Bark</button>"
"      <button class='act-btn' onclick=\"playSound('hi')\">Say Hi</button>"
"      <button class='act-btn' onclick=\"playSound('chirp')\">Chirp</button>"
"      <button class='act-btn' onclick=\"playSound('boot')\">Boot Melody</button>"
"    </div>"
"  </div>"

"  <div class='card'>"
"    <h2>OTA Firmware Update</h2>"
"    <input type='file' id='ota-file' accept='.bin' style='color:#a0a0d0; margin-bottom:10px; width:100%;' />"
"    <div class='bar-box'><div id='ota-bar'></div></div>"
"    <div id='ota-msg' style='font-size:12px; margin-bottom:8px; min-height:16px;'></div>"
"    <button class='btn-primary' id='ota-btn' onclick='doOTA()'>Flash Firmware OTA</button>"
"  </div>"

"  <div class='card'>"
"    <button class='btn-red' onclick='rebootDevice()'>Reboot Device</button>"
"  </div>"
"</div>"

"<script>"
"var ACTIONS = {1:'Lie', 2:'Bow', 3:'Lean', 4:'Wiggle', 5:'Rock', 6:'Sway', 7:'Shake', 8:'Poke', 9:'Kick', 10:'JumpFw', 11:'JumpBw', 12:'Stand'};"
"(function(){"
"  var g = document.getElementById('action-grid');"
"  for(var k in ACTIONS){"
"    var b = document.createElement('button');"
"    b.className = 'act-btn';"
"    b.textContent = ACTIONS[k];"
"    (function(act){ b.onclick = function(){ dog(act); }; })(k);"
"    g.appendChild(b);"
"  }"
"})();"

"function dog(act){"
"  fetch('/dog', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({action:''+act}) });"
"}"

"function playSound(name){"
"  fetch('/tts?name=' + encodeURIComponent(name));"
"}"

"var timeOffset = 0, isSynced = false;"
"function syncT(){"
"  fetch('/time')"
"    .then(function(r){ return r.json(); })"
"    .then(function(d){"
"      if(d.synced){"
"        timeOffset = (d.epoch * 1000) - Date.now();"
"        isSynced = true;"
"        document.getElementById('synced').textContent = 'NTP';"
"      } else {"
"        var ep = Math.floor(Date.now()/1000);"
"        fetch('/sync_time?epoch=' + ep);"
"        timeOffset = 0;"
"        isSynced = true;"
"        document.getElementById('synced').textContent = 'Browser';"
"      }"
"    }).catch(function(){});"
"}"
"syncT();"
"setInterval(syncT, 10000);"

"setInterval(function(){"
"  var d = new Date(Date.now() + timeOffset);"
"  var f = d.getFullYear() + '-' + ('0'+(d.getMonth()+1)).slice(-2) + '-' + ('0'+d.getDate()).slice(-2) + ' ' +"
"          ('0'+d.getHours()).slice(-2) + ':' + ('0'+d.getMinutes()).slice(-2) + ':' + ('0'+d.getSeconds()).slice(-2);"
"  document.getElementById('clock').textContent = f;"
"  document.getElementById('epoch').textContent = Math.floor(d.getTime() / 1000);"
"}, 1000);"

"let isRecording = false;"
"async function fetchStatus(){"
"  try {"
"    const res = await fetch('/status');"
"    const d = await res.json();"
"    document.getElementById('node_title').innerText = d.device + ' #' + d.number + ' (' + d.hostname + '.local)';"
"    document.getElementById('ip_addr').innerText = d.ip;"
"    document.getElementById('free_heap').innerText = Math.round(d.heap / 1024);"
"    document.getElementById('wifi_rssi').innerText = d.rssi;"
"    document.getElementById('src_disp').innerText = d.src.toUpperCase();"
"    document.getElementById('dst_disp').innerText = d.dst.toUpperCase();"
"    document.getElementById('sel_src').value = d.src;"
"    document.getElementById('sel_dst').value = d.dst;"
"    const badge = document.getElementById('conn-badge');"
"    badge.innerText = d.state;"
"    badge.className = 'status-badge ' + (d.state==='READY'?'':(d.state==='RECORDING'?'wait':'err'));"
"  } catch(e){}"
"}"

"async function swapLang(){"
"  await fetch('/api/swap', {method:'POST'});"
"  fetchStatus();"
"}"

"async function changeLang(){"
"  const src = document.getElementById('sel_src').value;"
"  const dst = document.getElementById('sel_dst').value;"
"  await fetch('/api/lang?src=' + src + '&dst=' + dst, {method:'POST'});"
"  fetchStatus();"
"}"

"async function pttPress(){"
"  if(!isRecording){"
"    isRecording = true;"
"    document.getElementById('ptt_btn').innerText = 'Recording (Release to Send)...';"
"    document.getElementById('ptt_btn').classList.add('recording');"
"    await fetch('/api/record/start', {method:'POST'});"
"  }"
"}"

"async function pttRelease(){"
"  if(isRecording){"
"    isRecording = false;"
"    document.getElementById('ptt_btn').innerText = 'Hold to Record Voice';"
"    document.getElementById('ptt_btn').classList.remove('recording');"
"    await fetch('/api/record/stop', {method:'POST'});"
"    fetchStatus();"
"  }"
"}"

"function doOTA(){"
"  var f = document.getElementById('ota-file').files[0];"
"  var msg = document.getElementById('ota-msg');"
"  var bar = document.getElementById('ota-bar');"
"  var btn = document.getElementById('ota-btn');"
"  if(!f){"
"    msg.textContent = 'Pick a .bin file first';"
"    msg.style.color = '#f59e0b';"
"    return;"
"  }"
"  btn.disabled = true; btn.style.opacity = '.5';"
"  msg.style.color = '#7c6af7';"
"  msg.textContent = 'Uploading ' + f.name + ' (' + Math.round(f.size/1024) + 'KB)...';"
"  bar.style.width = '0%';"
"  var xhr = new XMLHttpRequest();"
"  xhr.open('POST', '/ota', true);"
"  xhr.setRequestHeader('Content-Type', 'application/octet-stream');"
"  xhr.upload.onprogress = function(e){"
"    if(e.lengthComputable){"
"      var pct = Math.round((e.loaded / e.total) * 100);"
"      bar.style.width = pct + '%';"
"      msg.textContent = 'Flashing... ' + pct + '% (LED blinking)';"
"    }"
"  };"
"  xhr.onload = function(){"
"    bar.style.width = '100%';"
"    if(xhr.status === 200){"
"      msg.style.color = '#00e5a0';"
"      var t = 10;"
"      var iv = setInterval(function(){"
"        msg.textContent = 'OTA OK! Rebooting... reload in ' + t + 's';"
"        if(--t < 0){ clearInterval(iv); location.reload(); }"
"      }, 1000);"
"    } else {"
"      msg.style.color = '#ef4444';"
"      msg.textContent = 'OTA failed: HTTP ' + xhr.status + ' (' + xhr.responseText + ')';"
"      btn.disabled = false; btn.style.opacity = '1';"
"    }"
"  };"
"  xhr.onerror = function(){"
"    msg.style.color = '#ef4444';"
"    msg.textContent = 'Upload error';"
"    btn.disabled = false; btn.style.opacity = '1';"
"  };"
"  xhr.send(f);"
"}"

"async function rebootDevice(){"
"  if(confirm('Reboot ESP32?')){"
"    await fetch('/api/reboot', {method:'POST'});"
"    alert('Rebooting...');"
"  }"
"}"

"window.addEventListener('load', () => {"
"  fetchStatus();"
"  setInterval(fetchStatus, 4000);"
"});"
"</script>"
"</body>"
"</html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const char *ptr = HTML_INDEX;
    size_t total = strlen(HTML_INDEX);
    while (total > 0) {
        size_t chunk = (total > 1024) ? 1024 : total;
        esp_err_t err = httpd_resp_send_chunk(req, ptr, chunk);
        if (err != ESP_OK) return err;
        ptr += chunk;
        total -= chunk;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
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
    char json[128];
    snprintf(json, sizeof(json), "{\"synced\":%s,\"epoch\":%ld}",
             time_sync_is_synced() ? "true" : "false", (long)time_sync_get_epoch());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sync_time_handler(httpd_req_t *req)
{
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "epoch", param, sizeof(param)) == ESP_OK) {
            time_t t = (time_t)strtol(param, NULL, 10);
            if (t > 1000000) {
                time_sync_set_epoch(t);
            }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t dog_handler(httpd_req_t *req)
{
    char act[32] = {0};
    char buf[128] = {0};

    if (req->method == HTTP_GET) {
        if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
            httpd_query_key_value(buf, "action", act, sizeof(act));
        }
    } else {
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret > 0) {
            buf[ret] = '\0';
            char key[16] = {0};
            if (sscanf(buf, "{\"%[^\"]\":\"%[^\"]\"}", key, act) < 2) {
                strncpy(act, buf, sizeof(act) - 1);
            }
        }
    }

    bool ok = false;
    if (strlen(act) > 0) {
        if (strcmp(act, "bark") == 0) {
            sound_play_bark();
            ok = true;
        } else if (strcmp(act, "hi") == 0) {
            sound_play_named("hi");
            dog_action_send("bow");
            ok = true;
        } else {
            ok = dog_action_send(act);
        }
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? "{\"code\":200,\"status\":\"ok\"}" : "{\"code\":400,\"status\":\"unknown\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t tts_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char sound[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", sound, sizeof(sound));
    }
    if (strlen(sound) > 0) {
        sound_play_named(sound);
    } else {
        sound_play_bark();
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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

#define OTA_BUF_SIZE 4096

static esp_err_t ota_options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Incoming OTA firmware: %d bytes", total);

    ota_handle_t h = {0};
    if (ota_begin(&h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    static char buf[OTA_BUF_SIZE];
    int remaining = total;
    while (remaining > 0) {
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA recv error (%d), aborting", received);
            ota_abort(&h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        if (ota_write(&h, buf, received) != ESP_OK) {
            ota_abort(&h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write error");
            return ESP_FAIL;
        }
        remaining -= received;
        if (((total - remaining) % 65536) < OTA_BUF_SIZE) {
            ESP_LOGI(TAG, "OTA Progress: %d / %d bytes", total - remaining, total);
        }
    }

    if (ota_end(&h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ota\":\"ok\",\"restart\":true}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting HTTP Web Dashboard on port %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_register_uri_handler(s_server, &uri_index);

    httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_api_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(s_server, &uri_api_status);

    httpd_uri_t uri_time = { .uri = "/time", .method = HTTP_GET, .handler = time_handler };
    httpd_register_uri_handler(s_server, &uri_time);

    httpd_uri_t uri_api_time = { .uri = "/api/time", .method = HTTP_GET, .handler = time_handler };
    httpd_register_uri_handler(s_server, &uri_api_time);

    httpd_uri_t uri_api_time_post = { .uri = "/api/time", .method = HTTP_POST, .handler = time_handler };
    httpd_register_uri_handler(s_server, &uri_api_time_post);

    httpd_uri_t uri_sync_time = { .uri = "/sync_time", .method = HTTP_GET, .handler = sync_time_handler };
    httpd_register_uri_handler(s_server, &uri_sync_time);

    httpd_uri_t uri_dog_post = { .uri = "/dog", .method = HTTP_POST, .handler = dog_handler };
    httpd_register_uri_handler(s_server, &uri_dog_post);

    httpd_uri_t uri_dog_get = { .uri = "/dog", .method = HTTP_GET, .handler = dog_handler };
    httpd_register_uri_handler(s_server, &uri_dog_get);

    httpd_uri_t uri_dog_opt = { .uri = "/dog", .method = HTTP_OPTIONS, .handler = ota_options_handler };
    httpd_register_uri_handler(s_server, &uri_dog_opt);

    httpd_uri_t uri_tts = { .uri = "/tts", .method = HTTP_GET, .handler = tts_handler };
    httpd_register_uri_handler(s_server, &uri_tts);

    httpd_uri_t uri_swap = { .uri = "/api/swap", .method = HTTP_POST, .handler = swap_handler };
    httpd_register_uri_handler(s_server, &uri_swap);

    httpd_uri_t uri_cycle = { .uri = "/api/cycle", .method = HTTP_POST, .handler = swap_handler };
    httpd_register_uri_handler(s_server, &uri_cycle);

    httpd_uri_t uri_lang = { .uri = "/api/lang", .method = HTTP_POST, .handler = lang_handler };
    httpd_register_uri_handler(s_server, &uri_lang);

    httpd_uri_t uri_rec_start = { .uri = "/api/record/start", .method = HTTP_POST, .handler = record_start_handler };
    httpd_register_uri_handler(s_server, &uri_rec_start);

    httpd_uri_t uri_rec_stop = { .uri = "/api/record/stop", .method = HTTP_POST, .handler = record_stop_handler };
    httpd_register_uri_handler(s_server, &uri_rec_stop);

    httpd_uri_t uri_reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler };
    httpd_register_uri_handler(s_server, &uri_reboot);

    httpd_uri_t uri_ota_post = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler };
    httpd_register_uri_handler(s_server, &uri_ota_post);

    httpd_uri_t uri_ota_opt = { .uri = "/ota", .method = HTTP_OPTIONS, .handler = ota_options_handler };
    httpd_register_uri_handler(s_server, &uri_ota_opt);

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
