# 小蠖电脑端服务 MVP

这个目录实现第一版“电脑端服务”：

```text
电脑网页输入文字
  ↓
电脑端服务生成回复
  ↓
通过 WebSocket 推送给 ESP32
  ↓
ESP32 屏幕和串口显示用户输入与助手回复
```

当前版本暂时不做真实语音播放。LLM 已经预留 OpenAI-compatible API 框架；如果没有配置 API Key，会使用本地占位回复。

## 1. 启动服务

在工程根目录执行：

```bat
python -m local_control_server.server --host 0.0.0.0 --port 8000
```

或者双击：

```text
local_control_server\start_local_server.bat
```

启动后会打印类似：

```text
Browser UI: http://192.168.35.100:8000/
ESP32 OTA URL: http://192.168.35.100:8000/xiaozhi/ota/
ESP32 WebSocket: ws://192.168.35.100:8000/xiaozhi/ws
```

其中 `192.168.35.100` 要以你电脑实际打印出来的 IP 为准。

## 2. 配置 ESP32 固件

ESP32 需要把 OTA 地址改为服务打印出来的 OTA URL：

```text
http://电脑IP:8000/xiaozhi/ota/
```

不要使用：

```text
localhost
127.0.0.1
```

因为 ESP32 眼里的 `localhost` 是 ESP32 自己，不是电脑。

建议在：

```text
sdkconfig
```

中找到：

```text
CONFIG_OTA_URL="https://api.tenclass.net/xiaozhi/ota/"
```

改成：

```text
CONFIG_OTA_URL="http://电脑IP:8000/xiaozhi/ota/"
```

也可以使用脚本自动修改：

```bat
python -m local_control_server.configure_ota_url http://电脑IP:8000/xiaozhi/ota/
```

然后重新编译烧录：

```bat
idf.py build
idf.py -p COM口 flash monitor
```

如果你希望这个默认值长期保留，也可以同步修改：

```text
main\Kconfig.projbuild
```

里的 `CONFIG_OTA_URL` 默认值。

## 3. 使用电脑输入

1. 启动电脑端服务。
2. 烧录并启动 ESP32。
3. ESP32 进入待机后，说“你好小智”或按键唤醒。
4. ESP32 会连接本地 WebSocket 服务。
5. 打开浏览器：

```text
http://电脑IP:8000/
```

6. 在输入框里打字并发送。

串口预期能看到类似：

```text
WS: Connecting to websocket server: ws://电脑IP:8000/xiaozhi/ws
Application: >> 你好，我是电脑输入的
Application: << 我已经收到你的电脑端输入：你好，我是电脑输入的...
```

## 4. 配置 LLM API

当前服务支持 OpenAI-compatible Chat Completions 接口。

PowerShell 示例：

```powershell
$env:LOCAL_XIAOZHI_LLM_API_KEY="你的API Key"
$env:LOCAL_XIAOZHI_LLM_BASE_URL="https://api.openai.com/v1/chat/completions"
$env:LOCAL_XIAOZHI_LLM_MODEL="gpt-4o-mini"
python -m local_control_server.server --host 0.0.0.0 --port 8000
```

如果使用其他 OpenAI-compatible 服务，例如部分国产模型网关，需要把：

```text
LOCAL_XIAOZHI_LLM_BASE_URL
LOCAL_XIAOZHI_LLM_MODEL
```

改成对应服务的地址和模型名。

如果没有配置 API Key，服务会使用占位回复，方便先验证 ESP32 通信链路。

## 5. 如果自动显示的 IP 不对

如果启动服务时打印了类似 `172.x.x.x` 的虚拟网卡地址，而 ESP32 无法连接，请手动指定电脑 Wi-Fi 网卡 IP：

```bat
python -m local_control_server.server --host 0.0.0.0 --port 8000 --public-host 192.168.35.100
```

然后把 OTA URL 配成：

```bat
python -m local_control_server.configure_ota_url http://192.168.35.100:8000/xiaozhi/ota/
```

## 6. 当前限制

当前 MVP 已实现：

```text
OTA 配置接口
ESP32 WebSocket 接入
浏览器输入
占位 LLM 回复
把用户输入和助手回复推送到 ESP32
```

当前未实现：

```text
真实 ASR
真实 TTS 音频推送
Opus 音频编码
MCP 工具闭环调用
SD 卡记忆库联动
```

后续推荐顺序：

```text
1. 接入真实 LLM API
2. 增加 MCP initialize/tools/list/tools/call
3. 接入 SD 卡记忆检索
4. 增加 TTS 并转换为 Opus 发给 ESP32
```
