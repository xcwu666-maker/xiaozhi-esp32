#ifndef ROBOT_CONTROL_SERVER_H
#define ROBOT_CONTROL_SERVER_H

#include "servo_motion_controller.h"

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class RobotControlServer {
public:
    RobotControlServer();
    ~RobotControlServer();

    void StartAsync();
    esp_err_t SendMotionCommand(int command);

private:
    esp_err_t Start();
    esp_err_t StartAccessPoint();
    esp_err_t StartWebServer();
    esp_err_t InitializeUart();
    esp_err_t HandleCommand(const char* command);
    esp_err_t SendMoveCommand(int command);
    void BuildStatusJson(char* buffer, size_t buffer_size) const;

    static void StartTask(void* arg);
    static esp_err_t RootHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t CarInputHandler(httpd_req_t* req);

    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    TaskHandle_t start_task_ = nullptr;
    bool start_requested_ = false;
    bool started_ = false;
    bool uart_initialized_ = false;
    SemaphoreHandle_t uart_mutex_ = nullptr;
    ServoMotionController servo_motion_controller_;
};

#endif // ROBOT_CONTROL_SERVER_H
