#include "servo_motion_controller.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_check.h>
#include <esp_log.h>

#include <cstdint>

#define TAG "ServoMotionController"

namespace {
constexpr uart_port_t kServoUart = UART_NUM_2;
constexpr int kServoTxPin = GPIO_NUM_48;
constexpr int kServoRxPin = UART_PIN_NO_CHANGE;
constexpr int kServoBaudRate = 9600;
constexpr int kCommandQueueLength = 8;
constexpr int kMotionTaskStackSize = 6144;
constexpr int kMotionTaskPriority = 4;
constexpr int kOverlapTimeMs = 30;

constexpr int kOffset8 = 47;
constexpr int kOffset9 = 18;
constexpr int kOffset10 = 24;
constexpr int kOffset11 = -41;
constexpr int kOffset12 = 18;
} // namespace

struct ServoMotionController::ActionGroup {
    uint16_t time_ms;
    uint8_t count;
    struct {
        uint8_t id;
        uint16_t position;
    } servos[5];
};

namespace {
constexpr ServoMotionController::ActionGroup kNeutral = {
    300, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}
};
} // namespace

ServoMotionController::ServoMotionController() {
    init_mutex_ = xSemaphoreCreateMutex();
}

ServoMotionController::~ServoMotionController() {
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    if (command_queue_ != nullptr) {
        vQueueDelete(command_queue_);
        command_queue_ = nullptr;
    }
    if (uart_initialized_) {
        uart_driver_delete(kServoUart);
        uart_initialized_ = false;
    }
    if (init_mutex_ != nullptr) {
        vSemaphoreDelete(init_mutex_);
        init_mutex_ = nullptr;
    }
}

esp_err_t ServoMotionController::Initialize() {
    if (initialized_) {
        return ESP_OK;
    }

    if (init_mutex_ != nullptr) {
        xSemaphoreTake(init_mutex_, portMAX_DELAY);
    }

    if (initialized_) {
        if (init_mutex_ != nullptr) {
            xSemaphoreGive(init_mutex_);
        }
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = kServoBaudRate,
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

    esp_err_t err = uart_driver_install(kServoUart, 1024, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        goto done;
    }
    uart_initialized_ = true;

    err = uart_param_config(kServoUart, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        goto done;
    }

    err = uart_set_pin(kServoUart, kServoTxPin, kServoRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        goto done;
    }

    command_queue_ = xQueueCreate(kCommandQueueLength, sizeof(int));
    if (command_queue_ == nullptr) {
        ESP_LOGE(TAG, "create command queue failed");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    if (xTaskCreate(MotionTask, "servo_motion", kMotionTaskStackSize, this, kMotionTaskPriority, &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "create motion task failed");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Direct servo controller UART ready: UART%d TX=GPIO%d RX=none baud=%d",
             static_cast<int>(kServoUart), kServoTxPin, kServoBaudRate);

done:
    if (err != ESP_OK) {
        if (task_handle_ != nullptr) {
            vTaskDelete(task_handle_);
            task_handle_ = nullptr;
        }
        if (command_queue_ != nullptr) {
            vQueueDelete(command_queue_);
            command_queue_ = nullptr;
        }
        if (uart_initialized_) {
            uart_driver_delete(kServoUart);
            uart_initialized_ = false;
        }
    }
    if (init_mutex_ != nullptr) {
        xSemaphoreGive(init_mutex_);
    }
    return err;
}

esp_err_t ServoMotionController::ExecuteCommand(int command) {
    if (command < 0 || command > 6) {
        ESP_LOGW(TAG, "motion command out of range: %d", command);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(Initialize(), TAG, "initialize direct servo controller failed");
    if (xQueueSend(command_queue_, &command, pdMS_TO_TICKS(50)) != pdPASS) {
        ESP_LOGW(TAG, "motion command queue full: %d", command);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Queued direct servo motion command: %d", command);
    return ESP_OK;
}

void ServoMotionController::MotionTask(void* arg) {
    auto* self = static_cast<ServoMotionController*>(arg);

    int command = 0;
    while (true) {
        if (xQueueReceive(self->command_queue_, &command, portMAX_DELAY) == pdPASS) {
            esp_err_t err = self->ExecuteCommandNow(command);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "execute motion command %d failed: %s", command, esp_err_to_name(err));
            }
        }
    }
}

esp_err_t ServoMotionController::MoveMulti(const ActionGroup& action) {
    uint8_t buf[32] = {};
    uint8_t length = action.count * 3 + 5;

    buf[0] = 0x55;
    buf[1] = 0x55;
    buf[2] = length;
    buf[3] = 0x03;
    buf[4] = action.count;
    buf[5] = static_cast<uint8_t>(action.time_ms & 0xFF);
    buf[6] = static_cast<uint8_t>((action.time_ms >> 8) & 0xFF);

    for (uint8_t i = 0; i < action.count; ++i) {
        uint8_t offset = 7 + i * 3;
        buf[offset] = action.servos[i].id;
        buf[offset + 1] = static_cast<uint8_t>(action.servos[i].position & 0xFF);
        buf[offset + 2] = static_cast<uint8_t>((action.servos[i].position >> 8) & 0xFF);
    }

    uint8_t total_bytes = length + 2;
    int written = uart_write_bytes(kServoUart, buf, total_bytes);
    if (written != total_bytes) {
        ESP_LOGE(TAG, "servo UART write failed, written=%d expected=%u", written, total_bytes);
        return ESP_FAIL;
    }
    uart_wait_tx_done(kServoUart, pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t ServoMotionController::PlayAction(const ActionGroup& action) {
    ESP_RETURN_ON_ERROR(MoveMulti(action), TAG, "move multi servos failed");

    int delay_ms = static_cast<int>(action.time_ms) - kOverlapTimeMs;
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return ESP_OK;
}

esp_err_t ServoMotionController::ExecuteCommandNow(int command) {
    switch (command) {
        case 0:
            ESP_LOGI(TAG, "Action: neutral");
            return ActionNeutral();
        case 1:
            ESP_LOGI(TAG, "Action: crawl forward");
            return ActionCrawlForward();
        case 2:
            ESP_LOGI(TAG, "Action: crawl backward");
            return ActionCrawlBackward();
        case 3:
            ESP_LOGI(TAG, "Action: roll forward");
            return ActionRollForward();
        case 4:
            ESP_LOGI(TAG, "Action: roll backward");
            return ActionRollBackward();
        case 5:
            ESP_LOGI(TAG, "Action: happy");
            return ActionHappy();
        case 6:
            ESP_LOGI(TAG, "Action: comfort");
            return ActionComfort();
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ServoMotionController::ActionNeutral() {
    ESP_RETURN_ON_ERROR(PlayAction(kNeutral), TAG, "neutral failed");
    vTaskDelay(pdMS_TO_TICKS(kOverlapTimeMs));
    return ESP_OK;
}

esp_err_t ServoMotionController::ActionCrawlForward() {
    static constexpr ActionGroup s11 = {500, 5, {{8, 1050 + kOffset8}, {9, 1950 + kOffset9}, {10, 1500 + kOffset10}, {11, 1950 + kOffset11}, {12, 1050 + kOffset12}}};
    static constexpr ActionGroup s12 = {500, 5, {{8, 1050 + kOffset8}, {9, 1950 + kOffset9}, {10, 1500 + kOffset10}, {11, 1950 + kOffset11}, {12, 1500 + kOffset12}}};
    static constexpr ActionGroup s13 = {500, 5, {{8, 1500 + kOffset8}, {9, 1950 + kOffset9}, {10, 1500 + kOffset10}, {11, 1950 + kOffset11}, {12, 1050 + kOffset12}}};

    ESP_RETURN_ON_ERROR(PlayAction(s12), TAG, "crawl forward s12 failed");
    ESP_RETURN_ON_ERROR(PlayAction(s11), TAG, "crawl forward s11 failed");
    ESP_RETURN_ON_ERROR(PlayAction(s13), TAG, "crawl forward s13 failed");
    return ActionNeutral();
}

esp_err_t ServoMotionController::ActionCrawlBackward() {
    static constexpr ActionGroup s11 = {500, 5, {{8, 1050 + kOffset8}, {9, 1950 + kOffset9}, {10, 1500 + kOffset10}, {11, 1950 + kOffset11}, {12, 1050 + kOffset12}}};
    static constexpr ActionGroup s12 = {500, 5, {{8, 1050 + kOffset8}, {9, 1950 + kOffset9}, {10, 1500 + kOffset10}, {11, 1950 + kOffset11}, {12, 1500 + kOffset12}}};
    static constexpr ActionGroup s13 = {500, 5, {{8, 1500 + kOffset8}, {9, 1950 + kOffset9}, {10, 1500 + kOffset10}, {11, 1950 + kOffset11}, {12, 1050 + kOffset12}}};

    ESP_RETURN_ON_ERROR(PlayAction(s13), TAG, "crawl backward s13 failed");
    ESP_RETURN_ON_ERROR(PlayAction(s11), TAG, "crawl backward s11 failed");
    ESP_RETURN_ON_ERROR(PlayAction(s12), TAG, "crawl backward s12 failed");
    return ActionNeutral();
}

// esp_err_t ServoMotionController::ActionRollForward() {
//     static constexpr ActionGroup steps[] = {
//         {300, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 800 + kOffset12}}},
//         {300, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {300, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {350, 5, {{8, 1500 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {250, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {250, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {350, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 1150 + kOffset12}}},
//         {350, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 1150 + kOffset11}, {12, 1500 + kOffset12}}},
//         {350, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 1150 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
//         {350, 5, {{8, 800 + kOffset8}, {9, 1150 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
//         {350, 5, {{8, 1150 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
//     };
esp_err_t ServoMotionController::ActionRollForward() {
     static constexpr ActionGroup steps[] = {
         // 第1步 Index 1
         {500, 5, {{8, 1500 + kOffset8},  {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 840  + kOffset11}, {12, 960  + kOffset12}}},
         // 第2步 Index 2
         {500, 5, {{8, 1780 + kOffset8},  {9, 1500 + kOffset9}, {10, 850  + kOffset10}, {11, 960  + kOffset11}, {12, 820  + kOffset12}}},
         // 第3步 Index 3
         {500, 5, {{8, 1780 + kOffset8},  {9, 1320 + kOffset9}, {10, 670  + kOffset10}, {11, 900  + kOffset11}, {12, 1260 + kOffset12}}},
         // 第4步 Index 4
         {500, 5, {{8, 1940 + kOffset8},  {9, 845  + kOffset9}, {10, 590  + kOffset10}, {11, 1005 + kOffset11}, {12, 1500 + kOffset12}}},
         // 第5步 Index 5
         {500, 5, {{8, 1940 + kOffset8},  {9, 500  + kOffset9}, {10, 590  + kOffset10}, {11, 1445 + kOffset11}, {12, 1500 + kOffset12}}},
         // 第6步 Index 6
         {500, 5, {{8, 1200 + kOffset8},  {9, 500  + kOffset9}, {10, 1000 + kOffset10}, {11, 1485 + kOffset11}, {12, 965  + kOffset12}}},
         // 第7步 Index 7
         {500, 5, {{8, 560  + kOffset8},  {9, 840  + kOffset9}, {10, 1240 + kOffset10}, {11, 975  + kOffset11}, {12, 800  + kOffset12}}},
         // 第8步 Index 8
         {500, 5, {{8, 560  + kOffset8},  {9, 840  + kOffset9}, {10, 1240 + kOffset10}, {11, 875  + kOffset11}, {12, 500  + kOffset12}}},
         // 第9步 Index 9
         {500, 5, {{8, 1300 + kOffset8},  {9, 840  + kOffset9}, {10, 1240 + kOffset10}, {11, 700  + kOffset11}, {12, 500  + kOffset12}}},
         // 第10步 Index 10
         {500, 5, {{8, 1300 + kOffset8},  {9, 840  + kOffset9}, {10, 1240 + kOffset10}, {11, 700  + kOffset11}, {12, 1200 + kOffset12}}},
         // 第11步 Index 11
         {500, 5, {{8, 1300 + kOffset8},  {9, 840  + kOffset9}, {10, 1240 + kOffset10}, {11, 700  + kOffset11}, {12, 1700 + kOffset12}}},
         // 第12步 Index 12
         {500, 5, {{8, 1310 + kOffset8},  {9, 870  + kOffset9}, {10, 880  + kOffset10}, {11, 880  + kOffset11}, {12, 2000 + kOffset12}}},
         // 第13步 Index 13
         {500, 5, {{8, 1310 + kOffset8},  {9, 870  + kOffset9}, {10, 1000 + kOffset10}, {11, 880  + kOffset11}, {12, 2000 + kOffset12}}},
         // 第14步 Index 14
         {500, 5, {{8, 1310 + kOffset8},  {9, 870  + kOffset9}, {10, 1100 + kOffset10}, {11, 880  + kOffset11}, {12, 2000 + kOffset12}}},
         // 第15步 Index 15
         {500, 5, {{8, 1350 + kOffset8},  {9, 1170 + kOffset9}, {10, 1100 + kOffset10}, {11, 1580 + kOffset11}, {12, 2000 + kOffset12}}},
         // 第16步 Index 16 回中
         {500, 5, {{8, 1500 + kOffset8},  {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
     };
    for (const auto& step : steps) {
        ESP_RETURN_ON_ERROR(PlayAction(step), TAG, "roll forward step failed");
    }
    return ActionNeutral();
}

// esp_err_t ServoMotionController::ActionRollBackward() {
//     static constexpr ActionGroup steps[] = {
//         {300, 5, {{8, 800 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
//         {300, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
//         {300, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
//         {350, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 1500 + kOffset12}}},
//         {250, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {250, 5, {{8, 800 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {350, 5, {{8, 1150 + kOffset8}, {9, 800 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {350, 5, {{8, 1500 + kOffset8}, {9, 1150 + kOffset9}, {10, 800 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {350, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1150 + kOffset10}, {11, 800 + kOffset11}, {12, 800 + kOffset12}}},
//         {350, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1150 + kOffset11}, {12, 800 + kOffset12}}},
//         {350, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1150 + kOffset12}}},
//     };
esp_err_t ServoMotionController::ActionRollBackward() {
    static constexpr ActionGroup steps[] = {
        // 第1步 Index 1
        {1000, 5, {{8, 960  + kOffset8},  {9, 840  + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},

        // 第2步 Index 2
        {1000, 5, {{8, 820  + kOffset8},  {9, 960  + kOffset9}, {10, 850  + kOffset10}, {11, 1500 + kOffset11}, {12, 1780 + kOffset12}}},

        // 第3步 Index 3
        {1000, 5, {{8, 1260 + kOffset8},  {9, 900  + kOffset9}, {10, 670  + kOffset10}, {11, 1320 + kOffset11}, {12, 1780 + kOffset12}}},

        // 第4步 Index 4
        {1000, 5, {{8, 1500 + kOffset8},  {9, 1005 + kOffset9}, {10, 590  + kOffset10}, {11, 1320 + kOffset11}, {12, 1780 + kOffset12}}},

        // 第5步 Index 5
        {100, 5,  {{8, 1500 + kOffset8},  {9, 1005 + kOffset9}, {10, 590  + kOffset10}, {11, 845  + kOffset11}, {12, 1780 + kOffset12}}},

        // 第6步 Index 6
        {2000, 5, {{8, 1500 + kOffset8},  {9, 1005 + kOffset9}, {10, 590  + kOffset10}, {11, 845  + kOffset11}, {12, 1780 + kOffset12}}},

        // 第7步 Index 7
        {1000, 5, {{8, 1500 + kOffset8},  {9, 1005 + kOffset9}, {10, 590  + kOffset10}, {11, 845  + kOffset11}, {12, 1940 + kOffset12}}},

        // 第8步 Index 8
        {1000, 5, {{8, 1500 + kOffset8},  {9, 1445 + kOffset9}, {10, 590  + kOffset10}, {11, 500  + kOffset11}, {12, 1940 + kOffset12}}},

        // 第9步 Index 9
        {1000, 5, {{8, 965  + kOffset8},  {9, 1485 + kOffset9}, {10, 1000 + kOffset10}, {11, 500  + kOffset11}, {12, 1940 + kOffset12}}},

        // 第10步 Index 10
        {1000, 5, {{8, 965  + kOffset8},  {9, 1485 + kOffset9}, {10, 1000 + kOffset10}, {11, 500  + kOffset11}, {12, 1200 + kOffset12}}},

        // 第11步 Index 11
        {1000, 5, {{8, 800  + kOffset8},  {9, 975  + kOffset9}, {10, 1240 + kOffset10}, {11, 840  + kOffset11}, {12, 560  + kOffset12}}},

        // 第12步 Index 12
        {1000, 5, {{8, 500  + kOffset8},  {9, 875  + kOffset9}, {10, 1240 + kOffset10}, {11, 840  + kOffset11}, {12, 560  + kOffset12}}},

        // 第13步 Index 13
        {1000, 5, {{8, 500  + kOffset8},  {9, 700  + kOffset9}, {10, 1240 + kOffset10}, {11, 840  + kOffset11}, {12, 1300 + kOffset12}}},

        // 第14步 Index 14
        {1000, 5, {{8, 1200 + kOffset8},  {9, 700  + kOffset9}, {10, 1240 + kOffset10}, {11, 840  + kOffset11}, {12, 1300 + kOffset12}}},

        // 第15步 Index 15
        {1000, 5, {{8, 1700 + kOffset8},  {9, 700  + kOffset9}, {10, 1240 + kOffset10}, {11, 840  + kOffset11}, {12, 1300 + kOffset12}}},

        // 第16步 Index 16
        {1000, 5, {{8, 2000 + kOffset8},  {9, 880  + kOffset9}, {10, 880  + kOffset10}, {11, 870  + kOffset11}, {12, 1310 + kOffset12}}},

        // 第17步 Index 17
        {1000, 5, {{8, 2000 + kOffset8},  {9, 880  + kOffset9}, {10, 1000 + kOffset10}, {11, 870  + kOffset11}, {12, 1310 + kOffset12}}},

        // 第18步 Index 18
        {1000, 5, {{8, 1700 + kOffset8},  {9, 700  + kOffset9}, {10, 1240 + kOffset10}, {11, 840  + kOffset11}, {12, 1300 + kOffset12}}},

        // 第19步 Index 19
        {1000, 5, {{8, 1700 + kOffset8}, {9, 700 + kOffset9}, {10, 1360 + kOffset10}, {11, 1220 + kOffset11}, {12, 1600 + kOffset12}}},

        // 第20步 Index 20 回中
        {1000, 5, {{8, 1500 + kOffset8},  {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
    };
    for (const auto& step : steps) {
        ESP_RETURN_ON_ERROR(PlayAction(step), TAG, "roll backward step failed");
    }
    return ActionNeutral();
}

esp_err_t ServoMotionController::ActionHappy() {
    static constexpr ActionGroup steps[] = {
        {300, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1195 + kOffset12}}},
        {500, 5, {{8, 1240 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {750, 5, {{8, 1755 + kOffset8}, {9, 1080 + kOffset9}, {10, 1500 + kOffset10}, {11, 1050 + kOffset11}, {12, 1740 + kOffset12}}},
        {1000, 5, {{8, 1500 + kOffset8}, {9, 1755 + kOffset9}, {10, 1595 + kOffset10}, {11, 1850 + kOffset11}, {12, 1150 + kOffset12}}},
        {1000, 5, {{8, 1675 + kOffset8}, {9, 1130 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1475 + kOffset8}, {9, 1130 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1675 + kOffset8}, {9, 1130 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1475 + kOffset8}, {9, 1130 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1675 + kOffset8}, {9, 1130 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
    };

    for (const auto& step : steps) {
        ESP_RETURN_ON_ERROR(PlayAction(step), TAG, "happy step failed");
    }
    return ActionNeutral();
}

esp_err_t ServoMotionController::ActionComfort() {
    static constexpr ActionGroup steps[] = {
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1500 + kOffset10}, {11, 1500 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1635 + kOffset10}, {11, 1355 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1755 + kOffset8}, {9, 1070 + kOffset9}, {10, 1635 + kOffset10}, {11, 1355 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1580 + kOffset8}, {9, 1070 + kOffset9}, {10, 1635 + kOffset10}, {11, 1355 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1755 + kOffset8}, {9, 1070 + kOffset9}, {10, 1635 + kOffset10}, {11, 1355 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1635 + kOffset10}, {11, 1355 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1635 + kOffset10}, {11, 1355 + kOffset11}, {12, 1085 + kOffset12}}},
        {500, 5, {{8, 1070 + kOffset8}, {9, 1500 + kOffset9}, {10, 1635 + kOffset10}, {11, 1355 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1635 + kOffset10}, {11, 1210 + kOffset11}, {12, 1815 + kOffset12}}},
        {1500, 5, {{8, 1500 + kOffset8}, {9, 1660 + kOffset9}, {10, 1890 + kOffset10}, {11, 1805 + kOffset11}, {12, 1300 + kOffset12}}},
        {1000, 5, {{8, 1355 + kOffset8}, {9, 1725 + kOffset9}, {10, 1780 + kOffset10}, {11, 1450 + kOffset11}, {12, 1285 + kOffset12}}},
        {1000, 5, {{8, 1615 + kOffset8}, {9, 1180 + kOffset9}, {10, 1285 + kOffset10}, {11, 1165 + kOffset11}, {12, 1655 + kOffset12}}},
        {1000, 5, {{8, 1100 + kOffset8}, {9, 810 + kOffset9}, {10, 1175 + kOffset10}, {11, 1165 + kOffset11}, {12, 1500 + kOffset12}}},
        {1000, 5, {{8, 1815 + kOffset8}, {9, 1345 + kOffset9}, {10, 1260 + kOffset10}, {11, 1240 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1525 + kOffset8}, {9, 1345 + kOffset9}, {10, 1260 + kOffset10}, {11, 1240 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1815 + kOffset8}, {9, 1345 + kOffset9}, {10, 1260 + kOffset10}, {11, 1240 + kOffset11}, {12, 1500 + kOffset12}}},
        {500, 5, {{8, 1500 + kOffset8}, {9, 1500 + kOffset9}, {10, 1660 + kOffset10}, {11, 1390 + kOffset11}, {12, 1505 + kOffset12}}},
    };

    for (const auto& step : steps) {
        ESP_RETURN_ON_ERROR(PlayAction(step), TAG, "comfort step failed");
    }
    return ActionNeutral();
}
