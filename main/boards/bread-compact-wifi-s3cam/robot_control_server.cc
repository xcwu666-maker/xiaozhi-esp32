#include "robot_control_server.h"

#include <wifi_manager.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <dhcpserver/dhcpserver.h>
#include <lwip/ip4_addr.h>

#define TAG "RobotControlServer"

namespace {
constexpr const char* kApSsid = "小蠖";
constexpr const char* kApPassword = "12345678";
constexpr int kApChannel = 1;
constexpr int kApMaxConnections = 6;

constexpr uart_port_t kBearPiUart = UART_NUM_2;
constexpr int kBearPiTxPin = GPIO_NUM_43;
constexpr int kBearPiRxPin = UART_PIN_NO_CHANGE;
constexpr int kBearPiBaudRate = 9600;

constexpr const char* kHtmlHomePage = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
  <title>小蠖机器人控制台</title>
  <style>
    :root{--bg:#101417;--panel:#1a2024;--panel2:#222a2f;--line:#344047;--text:#f4f7f8;--muted:#9aa8af;--accent:#35c7b0;--accent2:#ffb454;--danger:#ef5d62;--shadow:0 12px 32px rgba(0,0,0,.28)}
    *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
    html,body{margin:0;min-height:100%;background:var(--bg);color:var(--text);font-family:"Segoe UI","Microsoft YaHei",sans-serif;letter-spacing:0}
    body{padding:max(14px,env(safe-area-inset-top)) max(14px,env(safe-area-inset-right)) max(18px,env(safe-area-inset-bottom)) max(14px,env(safe-area-inset-left));overscroll-behavior:none}
    button{font:inherit}
    .app{width:min(100%,800px);margin:auto}
    header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:2px 0 14px}
    .brand{display:flex;align-items:center;gap:11px;min-width:0}
    .logo{width:38px;height:38px;border:2px solid var(--accent);display:grid;place-items:center;border-radius:8px;color:var(--accent);font-weight:800;font-size:18px}
    h1{font-size:18px;line-height:1.2;margin:0;font-weight:700}
    .subtitle{font-size:12px;color:var(--muted);margin-top:3px}
    .link-state{display:flex;align-items:center;gap:7px;color:var(--muted);font-size:12px;white-space:nowrap}
    .dot{width:8px;height:8px;border-radius:50%;background:#69767c;box-shadow:0 0 0 3px rgba(105,118,124,.14)}
    .dot.online{background:var(--accent);box-shadow:0 0 0 3px rgba(53,199,176,.16)}
    .camera{position:relative;overflow:hidden;background:#050708;border:1px solid var(--line);border-radius:8px;box-shadow:var(--shadow);aspect-ratio:4/3}
    #cameraFrame{display:block;width:100%;height:100%;border:0;background:#fff}
    .camera-badge{position:absolute;top:10px;left:10px;padding:5px 8px;border-radius:5px;background:rgba(8,12,14,.76);font-size:11px;color:#8be4d6;backdrop-filter:blur(5px)}
    .recognition{display:grid;grid-template-columns:auto minmax(0,1fr) auto;align-items:center;gap:10px;margin:10px 0 14px;padding:10px 12px;background:var(--panel);border:1px solid var(--line);border-radius:7px}
    .recognition .label{font-size:12px;color:var(--muted)}
    #resultText{font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
    #confidence{color:var(--accent2);font-variant-numeric:tabular-nums;font-weight:700;font-size:13px}
    .control-grid{display:grid;grid-template-columns:1fr 1.18fr;gap:12px}
    .section{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px}
    .section-title{font-size:12px;color:var(--muted);margin:0 0 10px}
    .dpad{width:min(100%,226px);aspect-ratio:1;display:grid;grid-template-columns:repeat(3,1fr);grid-template-rows:repeat(3,1fr);gap:7px;margin:auto}
    .control-btn,.action-btn{border:1px solid #3a474e;background:var(--panel2);color:var(--text);border-radius:7px;box-shadow:0 3px 7px rgba(0,0,0,.2);touch-action:none;user-select:none;cursor:pointer;transition:transform .08s,background .08s,border-color .08s}
    .control-btn{display:grid;place-items:center;font-size:18px;font-weight:700;min-width:0}
    .control-btn:active,.control-btn.pressed,.action-btn:active{transform:translateY(1px) scale(.97);background:#26433f;border-color:var(--accent)}
    .up{grid-column:2}.left{grid-column:1;grid-row:2}.center{grid-column:2;grid-row:2;background:#151a1d;color:var(--accent);font-size:13px}.right{grid-column:3;grid-row:2}.down{grid-column:2;grid-row:3}
    .actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;height:calc(100% - 24px)}
    .action-btn{min-height:54px;padding:8px 6px;font-size:14px;font-weight:650}
    .action-btn.stop{grid-column:1/-1;background:#48282b;border-color:#754044;color:#ffd9da}
    .action-btn.stop:active{background:var(--danger);color:#fff}
    @media(max-width:560px){body{padding-left:10px;padding-right:10px}.control-grid{grid-template-columns:1fr}.section{padding:10px}.dpad{width:min(100%,210px)}.actions{height:auto}.action-btn{min-height:50px}.recognition{grid-template-columns:auto minmax(0,1fr)}#confidence{grid-column:2}.camera{border-radius:7px}}
    @media(orientation:landscape) and (max-height:540px){.app{width:min(100%,920px)}header{margin-bottom:8px}.workspace{display:grid;grid-template-columns:minmax(310px,1.25fr) 1fr;gap:12px}.recognition{margin-bottom:0}.control-grid{grid-template-columns:1fr}.dpad{width:min(100%,175px)}.section{padding:9px}.camera-column{min-width:0}}
  </style>
</head>
<body>
  <main class="app">
    <header>
      <div class="brand"><div class="logo">蠖</div><div><h1>小蠖机器人控制台</h1><div class="subtitle">ESP32-S3 AP + STA 控制</div></div></div>
      <div class="link-state"><span class="dot" id="controlDot"></span><span id="controlState">CONTROL CONNECTING</span></div>
    </header>
    <div class="workspace">
      <div class="camera-column">
        <div class="camera" id="cameraBox">
          <iframe id="cameraFrame" src="http://192.168.4.3/" title="Camera"></iframe>
          <div class="camera-badge" id="cameraState">CAMERA · 192.168.4.3</div>
        </div>
        <div class="recognition"><span class="label">AI 识别</span><span id="resultText">等待识别结果</span><span id="confidence">--</span></div>
      </div>
      <div>
        <div class="control-grid">
          <section class="section"><p class="section-title">蠕动控制</p><div class="dpad">
            <button class="control-btn up" data-move="1" aria-label="向前蠕动">向前</button>
            <button class="control-btn center" type="button" aria-label="中立归位">归位</button>
            <button class="control-btn down" data-move="2" aria-label="向后蠕动">向后</button>
          </div></section>
          <section class="section"><p class="section-title">舵机动作</p><div class="actions">
            <button class="action-btn" data-action="3">向前翻滚</button><button class="action-btn" data-action="4">向后翻滚</button>
            <button class="action-btn" data-action="5">开心</button><button class="action-btn" data-action="6">安慰</button>
            <button class="action-btn stop" data-action="0">中立归位</button>
          </div></section>
        </div>
      </div>
    </div>
  </main>
  <script>
    var resultUrl="ws://192.168.4.3/Result";
    var wsData=null,wsResult=null,resultRetry=null,controlRetry=null;
    function setControlState(online){document.getElementById("controlDot").classList.toggle("online",online);document.getElementById("controlState").textContent=online?"CONTROL READY":"CONTROL CONNECTING"}
    function connectControl(){clearTimeout(controlRetry);wsData=new WebSocket("ws://"+location.hostname+"/CarInput");wsData.onopen=function(){setControlState(true)};wsData.onclose=function(){setControlState(false);controlRetry=setTimeout(connectControl,1500)};wsData.onerror=function(){try{wsData.close()}catch(e){}}}
    function connectResult(){clearTimeout(resultRetry);wsResult=new WebSocket(resultUrl);wsResult.onmessage=function(e){try{var d=JSON.parse(e.data);document.getElementById("resultText").textContent=d.label||"none";var c=Number(d.confidence);document.getElementById("confidence").textContent=isFinite(c)?(c*100).toFixed(1)+"%":"--"}catch(err){}};wsResult.onclose=function(){resultRetry=setTimeout(connectResult,2000)};wsResult.onerror=function(){try{wsResult.close()}catch(e){}}}
    function sendCmd(key,value){if(wsData&&wsData.readyState===WebSocket.OPEN)wsData.send(key+","+value)}
    Array.prototype.forEach.call(document.querySelectorAll("[data-move]"),function(btn){btn.addEventListener("pointerdown",function(e){e.preventDefault();sendCmd("Move",btn.dataset.move)})});
    document.querySelector(".center").addEventListener("pointerdown",function(e){e.preventDefault();sendCmd("Move","0")});
    Array.prototype.forEach.call(document.querySelectorAll("[data-action]"),function(btn){btn.addEventListener("pointerdown",function(e){e.preventDefault();sendCmd("Move",btn.dataset.action)})});
    document.addEventListener("contextmenu",function(e){e.preventDefault()});
    connectControl();connectResult();
  </script>
</body>
</html>
)HTML";
} // namespace

RobotControlServer::RobotControlServer() {
    uart_mutex_ = xSemaphoreCreateMutex();
}

RobotControlServer::~RobotControlServer() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    if (uart_initialized_) {
        uart_driver_delete(kBearPiUart);
        uart_initialized_ = false;
    }
    if (uart_mutex_ != nullptr) {
        vSemaphoreDelete(uart_mutex_);
        uart_mutex_ = nullptr;
    }
}

void RobotControlServer::StartAsync() {
    if (started_ || start_requested_) {
        return;
    }
    start_requested_ = true;
    BaseType_t ok = xTaskCreate(StartTask, "robot_ctrl", 6144, this, 4, &start_task_);
    if (ok != pdPASS) {
        start_requested_ = false;
        start_task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create robot control start task");
    }
}

void RobotControlServer::StartTask(void* arg) {
    auto* self = static_cast<RobotControlServer*>(arg);
    esp_err_t err = self->Start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Robot control server start failed: %s", esp_err_to_name(err));
        self->start_requested_ = false;
    }
    self->start_task_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t RobotControlServer::Start() {
    if (started_) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(InitializeUart(), TAG, "initialize UART failed");
    ESP_RETURN_ON_ERROR(StartAccessPoint(), TAG, "start AP failed");
    ESP_RETURN_ON_ERROR(StartWebServer(), TAG, "start HTTP server failed");

    started_ = true;
    ESP_LOGI(TAG, "Robot control page is ready: http://192.168.4.1");
    return ESP_OK;
}

esp_err_t RobotControlServer::InitializeUart() {
    if (uart_initialized_) {
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = kBearPiBaudRate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
            .backup_before_sleep = 0,
        },
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(kBearPiUart, 1024, 0, 0, nullptr, 0), TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(kBearPiUart, &uart_config), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(kBearPiUart, kBearPiTxPin, kBearPiRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin");

    uart_initialized_ = true;
    ESP_LOGW(TAG, "BearPi UART uses TXD0/GPIO43 only at %d baud with framed protocol XH,N. Connect ESP32 TXD0 to BearPi RX and share GND.",
             kBearPiBaudRate);
    return ESP_OK;
}

esp_err_t RobotControlServer::StartAccessPoint() {
    if (ap_netif_ == nullptr) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(ap_netif_ != nullptr, ESP_FAIL, TAG, "create AP netif failed");

        esp_netif_ip_info_t ip_info = {};
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap_netif_));
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif_, &ip_info), TAG, "set AP IP failed");

        dhcps_lease_t lease = {};
        lease.enable = true;
        IP4_ADDR(&lease.start_ip, 192, 168, 4, 10);
        IP4_ADDR(&lease.end_ip, 192, 168, 4, 50);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(ap_netif_, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease)));
        ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif_), TAG, "start AP DHCP server failed");
    }

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), kApSsid, sizeof(wifi_config.ap.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.password), kApPassword, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = std::strlen(kApSsid);
    wifi_config.ap.channel = kApChannel;
    wifi_config.ap.max_connection = kApMaxConnections;
    wifi_config.ap.authmode = std::strlen(kApPassword) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set WIFI_MODE_APSTA failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "set AP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set WiFi power save failed");

    ESP_LOGI(TAG, "AP started: SSID=%s password=%s IP=192.168.4.1, DHCP=192.168.4.10-50, camera static IP=192.168.4.3",
             kApSsid, kApPassword);
    return ESP_OK;
}

esp_err_t RobotControlServer::StartWebServer() {
    if (server_ != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 4;
    config.stack_size = 6144;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;

    ESP_RETURN_ON_ERROR(httpd_start(&server_, &config), TAG, "httpd_start failed");

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = RootHandler,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &root_uri), TAG, "register root");

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = StatusHandler,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &status_uri), TAG, "register status");

    httpd_uri_t car_input_uri = {
        .uri = "/CarInput",
        .method = HTTP_GET,
        .handler = CarInputHandler,
        .user_ctx = this,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &car_input_uri), TAG, "register websocket");

    return ESP_OK;
}

esp_err_t RobotControlServer::RootHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kHtmlHomePage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RobotControlServer::StatusHandler(httpd_req_t* req) {
    auto* self = static_cast<RobotControlServer*>(req->user_ctx);
    char json[384];
    self->BuildStatusJson(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RobotControlServer::CarInputHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }

    auto* self = static_cast<RobotControlServer*>(req->user_ctx);
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.len == 0 || frame.len > 63) {
        return ESP_FAIL;
    }

    char payload[64] = {};
    frame.payload = reinterpret_cast<uint8_t*>(payload);
    err = httpd_ws_recv_frame(req, &frame, sizeof(payload) - 1);
    if (err != ESP_OK) {
        return err;
    }
    payload[frame.len] = '\0';

    err = self->HandleCommand(payload);
    const char* response = err == ESP_OK ? "OK" : "ERR";
    httpd_ws_frame_t reply = {};
    reply.type = HTTPD_WS_TYPE_TEXT;
    reply.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(response));
    reply.len = std::strlen(response);
    httpd_ws_send_frame(req, &reply);
    return err;
}

esp_err_t RobotControlServer::HandleCommand(const char* command) {
    char key[16] = {};
    char value[16] = {};
    if (std::sscanf(command, "%15[^,],%15s", key, value) != 2) {
        ESP_LOGW(TAG, "Invalid command: %s", command);
        return ESP_ERR_INVALID_ARG;
    }

    int parsed_value = std::atoi(value);
    if (std::strcmp(key, "Move") == 0) {
        return SendMotionCommand(parsed_value);
    }

    if (std::strcmp(key, "Light") == 0) {
        ESP_LOGW(TAG, "Light command ignored: GPIO47 is used by DISPLAY_DC_PIN on this board");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Unknown command key: %s", key);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t RobotControlServer::SendMoveCommand(int command) {
    return SendMotionCommand(command);
}

esp_err_t RobotControlServer::SendMotionCommand(int command) {
    if (command < 0 || command > 6) {
        ESP_LOGW(TAG, "Servo command out of range: %d", command);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(InitializeUart(), TAG, "initialize UART before motion command failed");

    char payload[6] = {};
    int payload_len = std::snprintf(payload, sizeof(payload), "XH,%d\n", command);
    if (payload_len <= 0 || payload_len >= static_cast<int>(sizeof(payload))) {
        ESP_LOGE(TAG, "Failed to build UART payload for command=%d", command);
        return ESP_FAIL;
    }
    if (uart_mutex_ != nullptr) {
        xSemaphoreTake(uart_mutex_, portMAX_DELAY);
    }
    int written = uart_write_bytes(kBearPiUart, payload, payload_len);
    if (uart_mutex_ != nullptr) {
        xSemaphoreGive(uart_mutex_);
    }
    if (written != payload_len) {
        ESP_LOGE(TAG, "UART write failed, written=%d expected=%d", written, payload_len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Send UART to BearPi servo controller: %.*s", payload_len - 1, payload);
    return ESP_OK;
}

void RobotControlServer::BuildStatusJson(char* buffer, size_t buffer_size) const {
    auto& wifi = WifiManager::GetInstance();
    wifi_sta_list_t sta_list = {};
    esp_err_t sta_err = esp_wifi_ap_get_sta_list(&sta_list);
    std::snprintf(buffer, buffer_size,
                  "{\"ap\":{\"ssid\":\"%s\",\"ip\":\"192.168.4.1\",\"clients\":%d},"
                  "\"sta\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},"
                  "\"camera\":{\"url\":\"http://192.168.4.3/\",\"result_ws\":\"ws://192.168.4.3/Result\"},"
                  "\"bearpi\":{\"uart\":%d,\"tx\":%d,\"rx\":%d,\"baud\":%d,\"protocol\":\"XH,N\\\\n\"}}",
                  kApSsid,
                  sta_err == ESP_OK ? sta_list.num : 0,
                  wifi.IsConnected() ? "true" : "false",
                  wifi.GetSsid().c_str(),
                  wifi.GetIpAddress().c_str(),
                  static_cast<int>(kBearPiUart),
                  static_cast<int>(kBearPiTxPin),
                  static_cast<int>(kBearPiRxPin),
                  kBearPiBaudRate);
}
