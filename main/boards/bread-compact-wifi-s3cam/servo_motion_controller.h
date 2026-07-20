#ifndef SERVO_MOTION_CONTROLLER_H
#define SERVO_MOTION_CONTROLLER_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class ServoMotionController {
public:
    struct ActionGroup;

    ServoMotionController();
    ~ServoMotionController();

    esp_err_t Initialize();
    esp_err_t ExecuteCommand(int command);

private:
    esp_err_t MoveMulti(const ActionGroup& action);
    esp_err_t PlayAction(const ActionGroup& action);
    esp_err_t ExecuteCommandNow(int command);

    esp_err_t ActionNeutral();
    esp_err_t ActionCrawlForward();
    esp_err_t ActionCrawlBackward();
    esp_err_t ActionRollForward();
    esp_err_t ActionRollBackward();
    esp_err_t ActionHappy();
    esp_err_t ActionComfort();

    static void MotionTask(void* arg);

    QueueHandle_t command_queue_ = nullptr;
    SemaphoreHandle_t init_mutex_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    bool initialized_ = false;
    bool uart_initialized_ = false;
};

#endif // SERVO_MOTION_CONTROLLER_H
