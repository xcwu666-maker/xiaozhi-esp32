#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "esp32_camera.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#include <algorithm>
#include <cctype>
#include <string>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "CompactWifiBoardS3Cam"

class CompactWifiBoardS3Cam : public WifiBoard {
private:
    struct KnowledgeItem {
        const char* source;
        const char* keywords;
        const char* content;
    };

    static constexpr KnowledgeItem kKnowledgeItems[] = {
        {
            "robot_usage",
            "小蠖 小智 功能 能做什么 介绍",
            "小蠖可以进行语音对话、屏幕显示、摄像头拍照理解、音量调节、待机唤醒，并可通过后续 MCP 工具扩展运动控制、传感器事件和家庭陪护能力。"
        },
        {
            "robot_usage",
            "配网 WiFi 联网 网络 配置",
            "需要重新配网时，可以在设备刚启动时按下 BOOT 按键进入配网模式，然后用手机连接设备热点，按页面提示填写 2.4GHz Wi-Fi 信息。"
        },
        {
            "robot_usage",
            "摄像头 拍照 图传 远程 画面 看家",
            "当前固件支持摄像头拍照和图片理解。远程持续看画面需要后续增加图传服务或网页端预览能力，第一版先以拍照识别和问答为主。"
        },
        {
            "robot_usage",
            "音量 声音 太大 太小 调高 调低",
            "可以通过语音让小智调节音量，例如“把音量调到 30%”。系统会调用音量 MCP 工具修改扬声器输出音量。"
        },
        {
            "robot_usage",
            "待机 休眠 停止 对话 再见",
            "需要结束当前对话时，可以说“待机”“再见”或“停止对话”。设备会关闭当前语音会话，等待下一次唤醒。"
        },
        {
            "family_safety",
            "着火 火情 火灾 烟雾 起火",
            "发现火情时，先提醒人员远离火源和浓烟，不要乘坐电梯；在安全位置通知家人，必要时拨打 119。不要让儿童或老人自行处理火源。"
        },
        {
            "family_safety",
            "摔倒 跌倒 老人 倒地 受伤",
            "老人跌倒后，先确认是否清醒、是否明显疼痛或出血。不要立即强行扶起；先安抚并通知家人，严重时拨打 120。"
        },
        {
            "caregiving",
            "儿童 小朋友 陪伴 故事 安抚",
            "儿童陪伴场景下，小智适合进行简短故事、问答互动和情绪安抚。回答要温和、简短，并避免给出危险动作建议。"
        },
        {
            "caregiving",
            "服药 吃药 提醒 老人 药",
            "服药提醒应以家人或医生设定为准。小智可以提醒用户按时吃药，但不能自行判断药量或替代医生建议。"
        },
        {
            "robot_motion",
            "运动 前进 后退 左转 右转 停止 底盘",
            "机器人运动控制建议后续通过独立 MCP 工具接入，例如前进、后退、左转、右转和停止。涉及安全时，应先确认周围环境再执行动作。"
        },
    };
 
    Button boot_button_;
    LcdDisplay* display_;
    Esp32Camera* camera_;

    static std::string DefaultUserProfileJson() {
        return "{\"user_name\":\"\",\"speech_speed\":\"normal\",\"preferences\":[],\"care_notes\":[]}";
    }

    static cJSON* LoadUserProfile() {
        Settings settings("user_memory", false);
        auto profile_json = settings.GetString("profile", DefaultUserProfileJson());
        auto profile = cJSON_Parse(profile_json.c_str());
        if (!cJSON_IsObject(profile)) {
            if (profile) {
                cJSON_Delete(profile);
            }
            profile = cJSON_Parse(DefaultUserProfileJson().c_str());
        }
        return profile;
    }

    static void SaveUserProfile(cJSON* profile) {
        char* profile_json = cJSON_PrintUnformatted(profile);
        Settings settings("user_memory", true);
        settings.SetString("profile", profile_json);
        cJSON_free(profile_json);
    }

    static std::string BuildUserProfileResult(bool success, const char* message, cJSON* profile) {
        auto result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", success);
        cJSON_AddStringToObject(result, "message", message);
        cJSON_AddItemReferenceToObject(result, "profile", profile);

        char* result_json = cJSON_PrintUnformatted(result);
        std::string result_string(result_json);
        cJSON_free(result_json);
        cJSON_Delete(result);
        return result_string;
    }

    static void SetProfileString(cJSON* profile, const char* key, const std::string& value) {
        if (!value.empty()) {
            cJSON_ReplaceItemInObject(profile, key, cJSON_CreateString(value.c_str()));
        }
    }

    static void AppendProfileArrayItem(cJSON* profile, const char* key, const std::string& value) {
        if (value.empty()) {
            return;
        }

        auto array = cJSON_GetObjectItem(profile, key);
        if (!cJSON_IsArray(array)) {
            cJSON_DeleteItemFromObject(profile, key);
            array = cJSON_CreateArray();
            cJSON_AddItemToObject(profile, key, array);
        }

        cJSON_AddItemToArray(array, cJSON_CreateString(value.c_str()));
    }

    void InitializeMemoryTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.memory.get_profile",
            "读取保存在 ESP32 设备本地 NVS 中的用户长期记忆。"
            "当用户询问“你知道我是谁吗”“你还记得我是谁吗”“你记得我什么”“我的偏好是什么”"
            "“我希望你怎么称呼我”“我让你以后怎么说话”等问题时，回答前必须先调用此工具。"
            "不要只依赖当前对话上下文回答用户身份或偏好，必须以此工具返回的 profile 为准。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto profile = LoadUserProfile();
                auto result = BuildUserProfileResult(true, "user profile loaded", profile);
                cJSON_Delete(profile);
                return result;
            });

        mcp_server.AddTool(
            "self.memory.update_profile",
            "更新保存在 ESP32 设备本地 NVS 中的用户长期记忆。"
            "当用户表达稳定长期信息时必须调用此工具，例如："
            "“我叫...”“我是...”“以后叫我...”“请记住我...”“以后你说话慢一点/快一点”"
            "“执行动作前先确认”“家里有老人/儿童需要照顾”等。"
            "调用成功后再用简短自然语言确认已经记住。"
            "只保存长期有效的用户称呼、语速偏好、交互偏好、照护备注，不保存普通闲聊内容。",
            PropertyList({
                Property("user_name", kPropertyTypeString, std::string("")),
                Property("speech_speed", kPropertyTypeString, std::string("")),
                Property("preference", kPropertyTypeString, std::string("")),
                Property("care_note", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto profile = LoadUserProfile();

                SetProfileString(profile, "user_name", properties["user_name"].value<std::string>());
                SetProfileString(profile, "speech_speed", properties["speech_speed"].value<std::string>());
                AppendProfileArrayItem(profile, "preferences", properties["preference"].value<std::string>());
                AppendProfileArrayItem(profile, "care_notes", properties["care_note"].value<std::string>());

                SaveUserProfile(profile);
                auto result = BuildUserProfileResult(true, "user profile updated", profile);
                cJSON_Delete(profile);
                return result;
            });

        ESP_LOGI(TAG, "Memory MCP tools registered");
    }

    static std::string ToLowerAscii(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return text;
    }

    static int CountKeywordMatches(const std::string& query, const char* keywords) {
        auto lowered_query = ToLowerAscii(query);
        int score = 0;
        const char* start = keywords;

        for (const char* p = keywords; ; ++p) {
            if (*p == ' ' || *p == ',' || *p == ';' || *p == '|' || *p == '/' || *p == '\0') {
                if (p > start) {
                    std::string keyword(start, p - start);
                    if (lowered_query.find(ToLowerAscii(keyword)) != std::string::npos) {
                        ++score;
                    }
                }
                if (*p == '\0') {
                    break;
                }
                start = p + 1;
            }
        }

        return score;
    }

    static std::string SearchKnowledge(const std::string& query) {
        const KnowledgeItem* best_item = nullptr;
        int best_score = 0;

        for (const auto& item : kKnowledgeItems) {
            int score = CountKeywordMatches(query, item.keywords);
            if (score > best_score) {
                best_score = score;
                best_item = &item;
            }
        }

        auto result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON_AddStringToObject(result, "query", query.c_str());

        if (best_item != nullptr) {
            cJSON_AddBoolToObject(result, "matched", true);
            cJSON_AddNumberToObject(result, "score", best_score);
            cJSON_AddStringToObject(result, "source", best_item->source);
            cJSON_AddStringToObject(result, "content", best_item->content);
        } else {
            cJSON_AddBoolToObject(result, "matched", false);
            cJSON_AddNumberToObject(result, "score", 0);
            cJSON_AddStringToObject(result, "source", "none");
            cJSON_AddStringToObject(result, "content", "没有在本地知识库中找到可靠答案，请根据通用知识简短回答，并说明当前没有命中项目知识。");
        }

        char* result_json = cJSON_PrintUnformatted(result);
        std::string result_string(result_json);
        cJSON_free(result_json);
        cJSON_Delete(result);
        return result_string;
    }

    void InitializeRagTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.rag.search_knowledge",
            "检索 ESP32 固件内置的项目知识库。"
            "当用户询问小蠖/小智功能、配网、摄像头、图传、音量、待机、家庭安全、火情、老人跌倒、儿童陪伴、服药提醒、运动控制等问题时，回答前必须先调用此工具。"
            "调用后必须优先依据返回的 content 回答；如果 matched=false，需要说明当前没有命中本地项目知识。",
            PropertyList({
                Property("query", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto query = properties["query"].value<std::string>();
                return SearchKnowledge(query);
            });

        ESP_LOGI(TAG, "RAG-lite MCP tools registered");
    }
 
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };        
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif
        
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 0;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new Esp32Camera(config);
        camera_->SetHMirror(false);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    CompactWifiBoardS3Cam() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeCamera();
        InitializeMemoryTools();
        InitializeRagTools();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
        
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(CompactWifiBoardS3Cam);
