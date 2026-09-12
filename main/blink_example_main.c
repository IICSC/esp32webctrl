/*
 * ESP32 Web Control Panel
 *
 * This example implements a web server that allows monitoring and controlling
 * GPIO pins, PWM outputs for servos, and LED status.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

// HTTP Server
#include "esp_http_server.h"

// Hardware drivers
#include "sdkconfig.h"
#include "soc/gpio_periph.h"

static const char *TAG = "ESP32_WEB_CONTROL";

#define NVS_NAMESPACE "wifi_config"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "password"

#define EXAMPLE_ESP_MAXIMUM_RETRY 5
#define MAX_SCAN_RESULTS 20

/* Onboard plain LED (active high) */
#define LED_GPIO 2

static char saved_ssid[64] = {0};
static char saved_pass[64] = {0};

static void wifi_load_credentials(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(saved_ssid);
        nvs_get_str(handle, NVS_KEY_SSID, saved_ssid, &len);
        len = sizeof(saved_pass);
        nvs_get_str(handle, NVS_KEY_PASS, saved_pass, &len);
        nvs_close(handle);
    }
}

static void wifi_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_SSID, ssid);
        nvs_set_str(handle, NVS_KEY_PASS, pass);
        nvs_commit(handle);
        nvs_close(handle);
    }
    strncpy(saved_ssid, ssid, sizeof(saved_ssid) - 1);
    strncpy(saved_pass, pass, sizeof(saved_pass) - 1);
}

/* The event group bits */
#define WIFI_CONNECTED_BIT         BIT0
#define WIFI_FAIL_BIT              BIT1

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

/* HTTP Server Handle */
static httpd_handle_t server = NULL;

/* Servo configuration */
typedef struct {
    int gpio;
    ledc_channel_t channel;
    bool initialized;
    int current_angle; // 0-180 degrees
} servo_config_t;

#define MAX_SERVOS 8
static servo_config_t servos[MAX_SERVOS] = {0};

/* GPIO states */
#define MAX_GPIOS 40
static bool gpio_states[MAX_GPIOS] = {0};
static bool gpio_modes[MAX_GPIOS] = {0}; // 0 = Input, 1 = Output

/* Index.html content */
const char index_html[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Web Control Panel</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f0f0f0; }
        h1 { color: #333; text-align: center; }
        .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }
        .section { margin-bottom: 30px; padding: 15px; border: 1px solid #ddd; border-radius: 5px; background-color: #f9f9f9; }
        .status-item { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #eee; }
        button { background-color: #4CAF50; border: none; color: white; padding: 10px 15px; text-align: center; font-size: 16px; margin: 4px 2px; cursor: pointer; border-radius: 4px; }
        button:hover { opacity: 0.8; }
        button.danger { background-color: #f44336; }
        input[type="number"], input[type="range"] { width: 80px; padding: 5px; margin: 5px; }
        select { padding: 5px; margin: 5px; }
        .control-row { display: flex; align-items: center; margin: 10px 0; }
        .control-label { min-width: 150px; display: inline-block; }
        table { width: 100%; border-collapse: collapse; }
        th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
        th { background-color: #f2f2f2; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ESP32 Web Control Panel</h1>
        <div class="section">
            <h2>WiFi 设置</h2>
            <p>当前WiFi: <span id="wifi_ssid">-</span></p>
            <button onclick="window.location.href='/wifi'">WiFi 配网设置</button>
        </div>

        <div class="section">
            <h2>System Status</h2>
            <div class="status-item"><span>ESP32 Status:</span><span id="status">Loading...</span></div>
            <div class="status-item"><span>IP Address:</span><span id="ip">-</span></div>
            <div class="status-item"><span>Uptime:</span><span id="uptime">-</span></div>
            <div class="status-item"><span>Free Heap:</span><span id="heap">-</span></div>
        </div>

        <div class="section">
            <h2>GPIO Control</h2>
            <div class="control-row">
                <label class="control-label">Select GPIO Pin:</label>
                <select id="gpio_select">
                    <option value="2">GPIO 2</option><option value="4">GPIO 4</option>
                    <option value="12">GPIO 12</option><option value="13">GPIO 13</option>
                    <option value="14">GPIO 14</option><option value="15">GPIO 15</option>
                    <option value="16">GPIO 16</option><option value="17">GPIO 17</option>
                    <option value="18">GPIO 18</option><option value="19">GPIO 19</option>
                    <option value="21">GPIO 21</option><option value="22">GPIO 22</option>
                    <option value="23">GPIO 23</option><option value="25">GPIO 25</option>
                    <option value="26">GPIO 26</option><option value="27">GPIO 27</option>
                    <option value="32">GPIO 32</option><option value="33">GPIO 33</option>
                </select>
                <button onclick="setGpioMode()">Set as Output</button>
                <button class="danger" onclick="setGpioInput()">Set as Input</button>
            </div>
            <div class="control-row">
                <label class="control-label">Control GPIO:</label>
                <button onclick="setGpioHigh()">Set High</button>
                <button class="danger" onclick="setGpioLow()">Set Low</button>
                <button onclick="toggleGpio()">Toggle</button>
                <span>Current State: <span id="gpio_state">Unknown</span></span>
            </div>
        </div>

        <div class="section">
            <h2>Servo Control</h2>
            <div class="control-row">
                <label class="control-label">Select Servo Pin:</label>
                <select id="servo_select">
                    <option value="2">GPIO 2</option><option value="4">GPIO 4</option>
                    <option value="12">GPIO 12</option><option value="13">GPIO 13</option>
                    <option value="14">GPIO 14</option><option value="15">GPIO 15</option>
                    <option value="16">GPIO 16</option><option value="17">GPIO 17</option>
                    <option value="18">GPIO 18</option><option value="19">GPIO 19</option>
                    <option value="21">GPIO 21</option><option value="22">GPIO 22</option>
                    <option value="23">GPIO 23</option><option value="25">GPIO 25</option>
                    <option value="26">GPIO 26</option><option value="27">GPIO 27</option>
                    <option value="32">GPIO 32</option><option value="33">GPIO 33</option>
                </select>
                <input type="number" id="angle_input" value="90" min="0" max="180">
                <button onclick="setServoAngle()">Set Angle</button>
                <input type="range" id="angle_slider" min="0" max="180" value="90" onchange="sliderChanged()">
            </div>
        </div>

        <div class="section">
            <h2>LED Control</h2>
            <div class="control-row">
                <label class="control-label">Onboard LED (GPIO2):</label>
                <button onclick="setLedOn()">Turn On</button>
                <button class="danger" onclick="setLedOff()">Turn Off</button>
            </div>
        </div>

        <div class="section">
            <h2>Connected Devices</h2>
            <table>
                <tr><th>Pin</th><th>Type</th><th>Status</th><th>Value</th></tr>
                <tbody id="device_table"></tbody>
            </table>
        </div>
    </div>

    <script>
        function updateStatus() {
            fetch('/api/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('status').textContent = data.status;
                    document.getElementById('wifi_ssid').textContent = data.wifi_ssid || '-';
                    document.getElementById('ip').textContent = data.ip;
                    document.getElementById('uptime').textContent = data.uptime + ' seconds';
                    document.getElementById('heap').textContent = data.heap + ' bytes';
                    
                    const tableBody = document.getElementById('device_table');
                    tableBody.innerHTML = '';
                    data.devices.forEach(device => {
                        const row = tableBody.insertRow();
                        row.insertCell().textContent = device.pin;
                        row.insertCell().textContent = device.type;
                        row.insertCell().textContent = device.status;
                        row.insertCell().textContent = device.value;
                    });
                    
                    const selectedGpio = document.getElementById('gpio_select').value;
                    const gpioDevice = data.devices.find(d => d.pin === parseInt(selectedGpio));
                    if(gpioDevice && gpioDevice.type === 'GPIO') {
                        document.getElementById('gpio_state').textContent = gpioDevice.value;
                    }
                })
                .catch(error => console.error('Error:', error));
        }

        function setGpioMode() {
            const pin = document.getElementById('gpio_select').value;
            fetch(`/api/gpio/${pin}/mode/output`, {method: 'POST'}).then(() => updateStatus());
        }

        function setGpioInput() {
            const pin = document.getElementById('gpio_select').value;
            fetch(`/api/gpio/${pin}/mode/input`, {method: 'POST'}).then(() => updateStatus());
        }

        function setGpioHigh() {
            const pin = document.getElementById('gpio_select').value;
            fetch(`/api/gpio/${pin}/high`, {method: 'POST'}).then(() => updateStatus());
        }

        function setGpioLow() {
            const pin = document.getElementById('gpio_select').value;
            fetch(`/api/gpio/${pin}/low`, {method: 'POST'}).then(() => updateStatus());
        }

        function toggleGpio() {
            const pin = document.getElementById('gpio_select').value;
            fetch(`/api/gpio/${pin}/toggle`, {method: 'POST'}).then(() => updateStatus());
        }

        function setServoAngle() {
            const pin = document.getElementById('servo_select').value;
            const angle = document.getElementById('angle_input').value;
            fetch(`/api/servo/${pin}/${angle}`, {method: 'POST'})
                .then(() => {
                    document.getElementById('angle_slider').value = angle;
                    updateStatus();
                });
        }

        function sliderChanged() {
            const angle = document.getElementById('angle_slider').value;
            document.getElementById('angle_input').value = angle;
            setServoAngle();
        }

        function setLedOn() {
            fetch('/api/led/on', {method: 'POST'}).then(() => updateStatus());
        }

        function setLedOff() {
            fetch('/api/led/off', {method: 'POST'}).then(() => updateStatus());
        }

        setInterval(updateStatus, 2000);
        updateStatus();
    </script>
</body>
</html>
)rawliteral";

/* Initialize servo on specified GPIO */
esp_err_t initialize_servo(int gpio_num, ledc_channel_t channel);

/* Set servo angle (0-180 degrees) */
esp_err_t set_servo_angle(int gpio_num, int angle);

esp_err_t initialize_servo(int gpio_num, ledc_channel_t channel) {
    int slot = -1;
    for(int i = 0; i < MAX_SERVOS; i++) {
        if(!servos[i].initialized) {
            slot = i;
            break;
        }
    }
    
    if(slot == -1) {
        ESP_LOGE(TAG, "No free servo slots");
        return ESP_FAIL;
    }
    
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = channel,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = gpio_num,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    
    servos[slot].gpio = gpio_num;
    servos[slot].channel = channel;
    servos[slot].initialized = true;
    servos[slot].current_angle = 90;
    
    ESP_LOGI(TAG, "Initialized servo on GPIO %d, channel %d", gpio_num, channel);
    set_servo_angle(gpio_num, 90);
    
    return ESP_OK;
}

esp_err_t set_servo_angle(int gpio_num, int angle) {
    if(angle < 0 || angle > 180) {
        ESP_LOGE(TAG, "Invalid angle: %d", angle);
        return ESP_ERR_INVALID_ARG;
    }
    
    int slot = -1;
    for(int i = 0; i < MAX_SERVOS; i++) {
        if(servos[i].initialized && servos[i].gpio == gpio_num) {
            slot = i;
            break;
        }
    }
    
    if(slot == -1) {
        ESP_LOGE(TAG, "Servo not found on GPIO %d", gpio_num);
        return ESP_ERR_NOT_FOUND;
    }
    
    uint32_t duty = 409 + (angle / 180.0) * (2048 - 409);
    
    ESP_LOGI(TAG, "Setting servo GPIO %d to angle %d, duty %"PRIu32, gpio_num, angle, duty);
    
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, servos[slot].channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, servos[slot].channel));
    
    servos[slot].current_angle = angle;
    
    return ESP_OK;
}

const char wifi_html[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 WiFi配网</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f0f0f0; }
        h1 { color: #333; text-align: center; }
        .container { max-width: 500px; margin: 0 auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }
        .form-group { margin-bottom: 20px; }
        label { display: block; margin-bottom: 5px; font-weight: bold; color: #555; }
        input[type="text"] { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; font-size: 16px; }
        button { background-color: #4CAF50; border: none; color: white; padding: 12px 24px; text-align: center; font-size: 16px; cursor: pointer; border-radius: 4px; width: 100%; }
        button:hover { opacity: 0.8; }
        .back-btn { background-color: #2196F3; }
        #message { margin-top: 15px; padding: 10px; border-radius: 4px; text-align: center; display: none; }
        .success { background-color: #d4edda; color: #155724; }
        .error { background-color: #f8d7da; color: #721c24; }
        .ap-item { display: flex; justify-content: space-between; padding: 8px 10px; border: 1px solid #ddd; border-radius: 4px; margin-top: 5px; cursor: pointer; background: #fafafa; }
        .ap-item:hover { background: #e8f0fe; }
        .signal { color: #666; font-size: 14px; }
        .scan-status { color: #666; font-size: 14px; margin-top: 5px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>WiFi 配网设置</h1>
        <div class="form-group">
            <button onclick="scanWifi()">扫描WiFi</button>
            <div id="ap_list" class="scan-status">正在扫描...</div>
        </div>
        <div class="form-group">
            <label for="ssid">WiFi 名称 (SSID)：</label>
            <input type="text" id="ssid" placeholder="请输入WiFi名称">
        </div>
        <div class="form-group">
            <label for="password">WiFi 密码：</label>
            <input type="text" id="password" placeholder="请输入WiFi密码">
        </div>
        <button onclick="saveWifi()">保存并连接</button>
        <button class="back-btn" onclick="window.location.href='/'">返回控制面板</button>
        <div id="message"></div>
    </div>
    <script>
        document.getElementById('ssid').focus();

        function saveWifi() {
            const ssid = document.getElementById('ssid').value.trim();
            const password = document.getElementById('password').value.trim();
            if (!ssid) {
                showMessage('请输入WiFi名称', 'error');
                return;
            }
            fetch('/api/wifi', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ ssid: ssid, password: password })
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    showMessage('WiFi设置成功！正在重新连接...', 'success');
                    setTimeout(() => { window.location.href = '/'; }, 2000);
                } else {
                    showMessage('设置失败：' + data.message, 'error');
                }
            })
            .catch(error => {
                showMessage('网络错误，请重试', 'error');
            });
        }

        function showMessage(msg, type) {
            const el = document.getElementById('message');
            el.textContent = msg;
            el.className = type;
            el.style.display = 'block';
        }

        function scanWifi() {
            const list = document.getElementById('ap_list');
            list.textContent = '正在扫描，请稍候...';
            fetch('/api/wifi/scan')
                .then(response => {
                    if (!response.ok) throw new Error('scan failed');
                    return response.json();
                })
                .then(aps => {
                    list.innerHTML = '';
                    if (!aps.length) {
                        list.textContent = '未找到WiFi网络';
                        return;
                    }
                    aps.forEach(ap => {
                        const item = document.createElement('div');
                        item.className = 'ap-item';
                        const name = document.createElement('span');
                        name.textContent = ap.ssid + (ap.auth ? ' [加密]' : ' [开放]');
                        const sig = document.createElement('span');
                        sig.className = 'signal';
                        sig.textContent = ap.rssi + ' dBm';
                        item.appendChild(name);
                        item.appendChild(sig);
                        item.onclick = () => {
                            document.getElementById('ssid').value = ap.ssid;
                            document.getElementById('password').focus();
                        };
                        list.appendChild(item);
                    });
                })
                .catch(() => {
                    list.textContent = '扫描失败，请重试';
                });
        }

        scanWifi();
    </script>
</body>
</html>
)rawliteral";

/* Main index page handler */
esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

/* WiFi config page handler */
esp_err_t wifi_config_page_handler(httpd_req_t *req) {
    httpd_resp_send(req, wifi_html, strlen(wifi_html));
    return ESP_OK;
}

/* WiFi scan handler: returns visible APs as JSON array */
esp_err_t wifi_scan_handler(httpd_req_t *req) {
    static wifi_ap_record_t records[MAX_SCAN_RESULTS];

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true); /* blocking scan */
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed");
        return ESP_OK;
    }

    uint16_t number = MAX_SCAN_RESULTS;
    if (esp_wifi_scan_get_ap_records(&number, records) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    char buf[160];
    char esc[2 * 32 + 1];
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < number; i++) {
        /* escape quotes/backslashes in SSID for valid JSON */
        int j = 0;
        for (int k = 0; records[i].ssid[k] && k < 32; k++) {
            char c = records[i].ssid[k];
            if (c == '"' || c == '\\') esc[j++] = '\\';
            esc[j++] = c;
        }
        esc[j] = '\0';
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 i > 0 ? "," : "", esc, records[i].rssi, records[i].authmode);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* Save WiFi credentials handler */
esp_err_t save_wifi_handler(httpd_req_t *req) {
    char body[256];
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    body[recv_len] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};
    char *ssid_pos = strstr(body, "\"ssid\"");
    char *pass_pos = strstr(body, "\"password\"");
    if (ssid_pos) {
        ssid_pos = strchr(ssid_pos, ':');
        if (ssid_pos) {
            ssid_pos++;
            while (*ssid_pos == ' ' || *ssid_pos == '"') ssid_pos++;
            char *end = ssid_pos;
            while (*end && *end != '"' && *end != '}') end++;
            *end = '\0';
            strncpy(ssid, ssid_pos, sizeof(ssid) - 1);
        }
    }
    if (pass_pos) {
        pass_pos = strchr(pass_pos, ':');
        if (pass_pos) {
            pass_pos++;
            while (*pass_pos == ' ' || *pass_pos == '"') pass_pos++;
            char *end = pass_pos;
            while (*end && *end != '"' && *end != '}') end++;
            *end = '\0';
            strncpy(pass, pass_pos, sizeof(pass) - 1);
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "application/json");
        const char *resp = "{\"success\":false,\"message\":\"SSID is empty\"}";
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    wifi_save_credentials(ssid, pass);

    /* Apply the new credentials immediately and reconnect */
    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(pass) == 0) {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid WiFi config\"}");
        return ESP_OK;
    }
    s_retry_num = 0;
    esp_wifi_disconnect();
    esp_wifi_connect();

    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"success\":true,\"message\":\"WiFi credentials saved\"}";
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* Get system status handler */
esp_err_t status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    char ip_str[IP4ADDR_STRLEN_MAX];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, IP4ADDR_STRLEN_MAX);
    
    char json_response[2048];
    int len = snprintf(json_response, sizeof(json_response),
        "{\"status\":\"Running\",\"ip\":\"%s\",\"uptime\":%"PRIu32",\"heap\":%"PRIu32",\"wifi_ssid\":\"%s\",\"devices\":[",
        ip_str,
        xTaskGetTickCount() * portTICK_PERIOD_MS / 1000,
        esp_get_free_heap_size(),
        saved_ssid
    );
    
    bool first_device = true;
    
    for(int i = 0; i < MAX_GPIOS; i++) {
        if (i < GPIO_NUM_MAX && gpio_modes[i]) {
            char device_str[128];
            int device_len = snprintf(device_str, sizeof(device_str), 
                "%s{\"pin\":%d,\"type\":\"GPIO\",\"status\":\"Configured\",\"value\":\"%s\"}",
                first_device ? "" : ",",
                i, gpio_states[i] ? "HIGH" : "LOW"
            );
            
            if (len + device_len < sizeof(json_response)) {
                strcat(json_response, device_str);
                len += device_len;
                first_device = false;
            }
        }
    }
    
    for(int i = 0; i < MAX_SERVOS; i++) {
        if(servos[i].initialized) {
            char device_str[128];
            int device_len = snprintf(device_str, sizeof(device_str),
                "%s{\"pin\":%d,\"type\":\"Servo\",\"status\":\"Configured\",\"value\":\"%d deg\"}",
                first_device ? "" : ",",
                servos[i].gpio, servos[i].current_angle
            );
            
            if (len + device_len < sizeof(json_response)) {
                strcat(json_response, device_str);
                len += device_len;
                first_device = false;
            }
        }
    }
    
    if (len < sizeof(json_response)) {
        strcat(json_response, "]}");
    } else {
        json_response[sizeof(json_response) - 2] = ']';
        json_response[sizeof(json_response) - 1] = '}';
    }
    
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

/* Extract GPIO number from URL */
int extract_gpio_number(const char* url) {
    const char* start = strstr(url, "/api/gpio/");
    if(start) {
        start += 10;
        return atoi(start);
    }
    return -1;
}

/* Extract servo command from URL */
bool extract_servo_command(const char* url, int* gpio_num, int* angle) {
    const char* base = "/api/servo/";
    const char* start = strstr(url, base);
    if(start) {
        start += strlen(base);
        *gpio_num = atoi(start);
        
        const char* slash = strchr(start, '/');
        if(slash) {
            *angle = atoi(slash + 1);
            return true;
        }
    }
    return false;
}

/* GPIO mode handlers */
esp_err_t gpio_output_handler(httpd_req_t *req) {
    int gpio_num = extract_gpio_number(req->uri);
    
    if(gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPIO pin");
        return ESP_OK;
    }
    
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << gpio_num);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    gpio_modes[gpio_num] = 1;
    gpio_states[gpio_num] = 0;
    gpio_set_level(gpio_num, 0);
    
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t gpio_input_handler(httpd_req_t *req) {
    int gpio_num = extract_gpio_number(req->uri);
    
    if(gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPIO pin");
        return ESP_OK;
    }
    
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << gpio_num);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    gpio_modes[gpio_num] = 0;
    
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t gpio_high_handler(httpd_req_t *req) {
    int gpio_num = extract_gpio_number(req->uri);
    
    if(gpio_num < 0 || gpio_num >= GPIO_NUM_MAX || !gpio_modes[gpio_num]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPIO or not output mode");
        return ESP_OK;
    }
    
    gpio_set_level(gpio_num, 1);
    gpio_states[gpio_num] = 1;
    
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t gpio_low_handler(httpd_req_t *req) {
    int gpio_num = extract_gpio_number(req->uri);
    
    if(gpio_num < 0 || gpio_num >= GPIO_NUM_MAX || !gpio_modes[gpio_num]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPIO or not output mode");
        return ESP_OK;
    }
    
    gpio_set_level(gpio_num, 0);
    gpio_states[gpio_num] = 0;
    
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t gpio_toggle_handler(httpd_req_t *req) {
    int gpio_num = extract_gpio_number(req->uri);
    
    if(gpio_num < 0 || gpio_num >= GPIO_NUM_MAX || !gpio_modes[gpio_num]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPIO or not output mode");
        return ESP_OK;
    }
    
    int current_level = gpio_get_level(gpio_num);
    gpio_set_level(gpio_num, !current_level);
    gpio_states[gpio_num] = !current_level;
    
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GPIO action dispatcher: /api/gpio/<pin>/<action> */
esp_err_t gpio_action_handler(httpd_req_t *req) {
    const char *p = strstr(req->uri, "/api/gpio/");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    p += strlen("/api/gpio/");
    const char *slash = strchr(p, '/');
    const char *action = slash ? slash + 1 : "";

    if (strcmp(action, "mode/output") == 0) return gpio_output_handler(req);
    if (strcmp(action, "mode/input") == 0)  return gpio_input_handler(req);
    if (strcmp(action, "high") == 0)        return gpio_high_handler(req);
    if (strcmp(action, "low") == 0)         return gpio_low_handler(req);
    if (strcmp(action, "toggle") == 0)      return gpio_toggle_handler(req);

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown GPIO action");
    return ESP_OK;
}

/* Servo control handler */
esp_err_t servo_control_handler(httpd_req_t *req) {
    int gpio_num, angle;
    
    if(!extract_servo_command(req->uri, &gpio_num, &angle)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid servo command");
        return ESP_OK;
    }
    
    if(gpio_num < 0 || gpio_num >= GPIO_NUM_MAX || angle < 0 || angle > 180) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPIO or angle");
        return ESP_OK;
    }
    
    bool servo_found = false;
    for(int i = 0; i < MAX_SERVOS; i++) {
        if(servos[i].initialized && servos[i].gpio == gpio_num) {
            servo_found = true;
            break;
        }
    }
    
    if(!servo_found) {
        ledc_channel_t free_channel = LEDC_CHANNEL_MAX;
        for(int ch = 0; ch < 8; ch++) {
            bool used = false;
            for(int i = 0; i < MAX_SERVOS; i++) {
                if(servos[i].initialized && servos[i].channel == ch) {
                    used = true;
                    break;
                }
            }
            if(!used) {
                free_channel = ch;
                break;
            }
        }
        
        if(free_channel != LEDC_CHANNEL_MAX) {
            initialize_servo(gpio_num, free_channel);
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No free LEDC channels");
            return ESP_OK;
        }
    }
    
    esp_err_t result = set_servo_angle(gpio_num, angle);
    if(result != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set servo angle");
        return ESP_OK;
    }
    
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* LED control handlers (plain onboard LED, active high) */
esp_err_t led_on_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "LED on requested (GPIO %d)", LED_GPIO);
    gpio_set_level(LED_GPIO, 1);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t led_off_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "LED off requested (GPIO %d)", LED_GPIO);
    gpio_set_level(LED_GPIO, 0);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* LED action dispatcher: /api/led/<action> */
esp_err_t led_action_handler(httpd_req_t *req) {
    const char *p = strstr(req->uri, "/api/led/");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    p += strlen("/api/led/");
    if (strncmp(p, "on", 2) == 0)         return led_on_handler(req);
    if (strncmp(p, "off", 3) == 0)        return led_off_handler(req);

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown LED action");
    return ESP_OK;
}

/* Register handlers for the web server */
httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP server on port: %d", config.server_port);
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t main_page = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = index_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &main_page);

        httpd_uri_t wifi_config_page = {
            .uri       = "/wifi",
            .method    = HTTP_GET,
            .handler   = wifi_config_page_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &wifi_config_page);

        httpd_uri_t wifi_scan = {
            .uri       = "/api/wifi/scan",
            .method    = HTTP_GET,
            .handler   = wifi_scan_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &wifi_scan);

        httpd_uri_t save_wifi = {
            .uri       = "/api/wifi",
            .method    = HTTP_POST,
            .handler   = save_wifi_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &save_wifi);
        
        httpd_uri_t api_status = {
            .uri       = "/api/status",
            .method    = HTTP_GET,
            .handler   = status_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &api_status);
        
        httpd_uri_t gpio_actions = {
            .uri       = "/api/gpio/*",
            .method    = HTTP_POST,
            .handler   = gpio_action_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &gpio_actions);
        
        httpd_uri_t servo_control = {
            .uri       = "/api/servo/*",
            .method    = HTTP_POST,
            .handler   = servo_control_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &servo_control);
        
        httpd_uri_t led_actions = {
            .uri       = "/api/led/*",
            .method    = HTTP_POST,
            .handler   = led_action_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &led_actions);
        
        return server;
    }
    
    ESP_LOGI(TAG, "Error starting HTTP server!");
    return NULL;
}

/* Event handler for Wi-Fi events */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (saved_ssid[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize Wi-Fi station */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    if (saved_ssid[0] != '\0') {
        strlcpy((char *)wifi_config.sta.ssid, saved_ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, saved_pass, sizeof(wifi_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    }

    /* Provisioning AP: always available for configuration */
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32-Control",
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished. Setup AP 'ESP32-Control' (pass: 12345678) -> http://192.168.4.1");
    if (saved_ssid[0] == '\0') {
        ESP_LOGI(TAG, "No saved WiFi credentials yet.");
    }
}

/* Initialize onboard LED GPIO (plain LED, active high) */
static void led_init(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32 Web Control Panel Starting");

    // Load saved WiFi credentials
    wifi_load_credentials();

    // Initialize onboard LED
    led_init();
    
    // Initialize Wi-Fi
    wifi_init_sta();
    
    // Start the web server
    server = start_webserver();
    
    if (server == NULL) {
        ESP_LOGE(TAG, "Could not start web server");
        return;
    }
    
    ESP_LOGI(TAG, "Web server started successfully");
    
    // Print access info (STA IP is logged by event_handler once obtained)
    ESP_LOGI(TAG, "Configure WiFi via AP 'ESP32-Control' at http://192.168.4.1");
    
    // Main loop
    while(1) {
        ESP_LOGI(TAG, "System running - Free heap: %"PRIu32" bytes, Uptime: %"PRIu32"s", 
                 esp_get_free_heap_size(), xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}