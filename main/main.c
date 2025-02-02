#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Function declarations
void send_to_server(const char* endpoint, const char* data);
static float get_distance(void);
static bool check_water_level(int cups_requested);
void coffee_brewing_task(void *pvParameters);


// Function declarations
void send_to_server(const char* endpoint, const char* data);
static float get_distance(void);
static bool check_water_level(int cups_requested);
void coffee_brewing_task(void *pvParameters);

// GPIO Pin definitions
#define LED_GPIO 12        
#define SERVO_GPIO 13      
#define TRIGGER_GPIO 5     
#define ECHO_GPIO 18       

// Servo configuration
#define SERVO_MIN_PULSEWIDTH 500   
#define SERVO_MAX_PULSEWIDTH 2500  
#define SERVO_MAX_DEGREE 180       

// Global variables
static const char *TAG = "coffee-maker";
static int daily_uses = 0;
static int total_cups = 0;
static const char* SERVER_URL = "http://192.168.1.23:8087";

// Convert angle to duty cycle for servo
static uint32_t degree_to_duty(int degree) {
    uint32_t pulse_width = SERVO_MIN_PULSEWIDTH + (((SERVO_MAX_PULSEWIDTH - SERVO_MIN_PULSEWIDTH) * degree) / SERVO_MAX_DEGREE);
    uint32_t duty = (pulse_width * ((1 << 13) - 1)) / 20000;
    return duty;
}

// Measure distance with ultrasonic sensor
static float get_distance(void) {
    gpio_set_level(TRIGGER_GPIO, 0);
    esp_rom_delay_us(2);
    
    gpio_set_level(TRIGGER_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TRIGGER_GPIO, 0);
    
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 0) {
        if (esp_timer_get_time() - start > 30000) {
            return -1;
        }
    }
    
    start = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 1) {
        if (esp_timer_get_time() - start > 30000) {
            return -1;
        }
    }
    int64_t end = esp_timer_get_time();
    
    float distance = ((end - start) * 0.0343) / 2;
    return distance;
}

// Check water level
static bool check_water_level(int cups_requested) {
    float distance = get_distance();
    if (distance < 0) return false;
    
    float water_level = 10 - distance;
    float water_needed = cups_requested * 0.5;
    
    // Si el nivel de agua es crítico, enviar alerta
    if (water_level < 2.0) {
        char alert_data[100];
        snprintf(alert_data, sizeof(alert_data), 
                "{\"status\":\"low\",\"waterLevel\":%.1f}", 
                water_level);
        send_to_server("/water_alert", alert_data);
    }
    
    return water_level >= water_needed;
}

// Coffee brewing task
void coffee_brewing_task(void *pvParameters) {
    int cups = *(int*)pvParameters;
    free(pvParameters);  // Free the allocated memory
    
    int brewing_time = (cups == 2) ? 300 : (cups == 4) ? 600 : 900;
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, degree_to_duty(180));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    
    vTaskDelay(brewing_time * 1000 / portTICK_PERIOD_MS);
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, degree_to_duty(0));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    
    gpio_set_level(LED_GPIO, 0);
    
    char data[100];
    snprintf(data, sizeof(data), "{\"uses\":1,\"cups\":%d,\"waterLevel\":%.1f}", 
             cups, 10 - get_distance());
    send_to_server("/update_stats", data);
    
    vTaskDelete(NULL);
}

// Send data to server with retry mechanism
void send_to_server(const char* endpoint, const char* data) {
    char url[100];
    snprintf(url, sizeof(url), "%s%s", SERVER_URL, endpoint);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .keep_alive_enable = true
    };
    
    int retry_count = 0;
    while (retry_count < 3) {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            retry_count++;
            continue;
        }
        
        esp_http_client_set_post_field(client, data, strlen(data));
        esp_http_client_set_header(client, "Content-Type", "application/json");
        
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            if (status_code == 200) {
                ESP_LOGI(TAG, "Data sent successfully to %s", endpoint);
                esp_http_client_cleanup(client);
                return;
            }
        }
        
        ESP_LOGW(TAG, "Failed to send data (attempt %d/3): %s", 
                 retry_count + 1, esp_err_to_name(err));
        
        esp_http_client_cleanup(client);
        vTaskDelay((1000 * (retry_count + 1)) / portTICK_PERIOD_MS);
        retry_count++;
    }
    
    ESP_LOGE(TAG, "Failed to send data after 3 attempts");
}


// HTML page
const char html_page[] = "<!DOCTYPE html>\
<html>\
<head>\
    <title>Control de Cafetera IoT</title>\
    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\
    <style>\
        body { font-family: Arial; text-align: center; background: #f5f5f5; }\
        .container { max-width: 600px; margin: 0 auto; padding: 20px; }\
        .control-panel { background: white; padding: 20px; border-radius: 8px; margin: 20px 0; }\
        select, button { padding: 10px; margin: 10px; }\
        .status { margin: 20px 0; }\
    </style>\
</head>\
<body>\
    <div class=\"container\">\
        <h1>Cafetera IoT</h1>\
        <div class=\"control-panel\">\
            <h2>Control de Preparación</h2>\
            <select id=\"cups\">\
                <option value=\"2\">2 Tazas (5 min)</option>\
                <option value=\"4\">4 Tazas (10 min)</option>\
                <option value=\"8\">8 Tazas (15 min)</option>\
            </select>\
            <button onclick=\"startCoffee()\">Preparar Café</button>\
        </div>\
        <div class=\"status\">\
            <h3>Estado</h3>\
            <p>Nivel de agua: <span id=\"waterLevel\">Midiendo...</span></p>\
            <p>Estado: <span id=\"status\">Listo</span></p>\
            <button onclick=\"checkWater()\">Verificar Agua</button>\
        </div>\
    </div>\
    <script>\
        function startCoffee() {\
            const cups = document.getElementById('cups').value;\
            fetch('/make_coffee', {\
                method: 'POST',\
                body: JSON.stringify({ cups: cups })\
            })\
            .then(response => response.text())\
            .then(result => {\
                document.getElementById('status').innerText = result;\
            });\
        }\
        function checkWater() {\
            fetch('/check_water', { method: 'POST' })\
            .then(response => response.text())\
            .then(level => {\
                document.getElementById('waterLevel').innerText = level;\
            });\
        }\
    </script>\
</body>\
</html>";

// HTTP handlers
static esp_err_t page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

static esp_err_t check_water_handler(httpd_req_t *req) {
    float distance = get_distance();
    float water_level = 10 - distance;
    
    char resp[50];
    snprintf(resp, sizeof(resp), "%.1f cm", water_level);
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t make_coffee_handler(httpd_req_t *req) {
    char buf[100];
    int received = httpd_req_recv(req, buf, sizeof(buf));
    if (received <= 0) return ESP_FAIL;
    
    int cups = 2;
    if (strstr(buf, "\"cups\":\"4\"")) cups = 4;
    if (strstr(buf, "\"cups\":\"8\"")) cups = 8;
    
    if (!check_water_level(cups)) {
        httpd_resp_send(req, "Nivel de agua insuficiente", HTTPD_RESP_USE_STRLEN);
        send_to_server("/water_alert", "{\"status\":\"low\"}");
        return ESP_OK;
    }
    
    gpio_set_level(LED_GPIO, 1);
    
    daily_uses++;
    total_cups += cups;
    
    int *cups_param = malloc(sizeof(int));
    if (cups_param == NULL) {
        httpd_resp_send(req, "Error de memoria", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    *cups_param = cups;
    
    BaseType_t task_created = xTaskCreate(coffee_brewing_task, "coffee_brewing", 2048, cups_param, 5, NULL);
    if (task_created != pdPASS) {
        free(cups_param);
        httpd_resp_send(req, "Error al crear tarea", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    char response[100];
    snprintf(response, sizeof(response), "Preparando %d tazas de café", cups);
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    
    return ESP_OK;
}

// URI handlers configuration
static httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = page_handler,
    .user_ctx = NULL
};

static httpd_uri_t uri_check_water = {
    .uri = "/check_water",
    .method = HTTP_POST,
    .handler = check_water_handler,
    .user_ctx = NULL
};

static httpd_uri_t uri_make_coffee = {
    .uri = "/make_coffee",
    .method = HTTP_POST,
    .handler = make_coffee_handler,
    .user_ctx = NULL
};


// Start web server
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_check_water);
        httpd_register_uri_handler(server, &uri_make_coffee);
        return server;
    }

    return NULL;
}

// Main function
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    gpio_reset_pin(TRIGGER_GPIO);
    gpio_reset_pin(ECHO_GPIO);
    gpio_set_direction(TRIGGER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_GPIO, GPIO_MODE_INPUT);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Configuración WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Casa_GROB_ROSERO",
            .password = "laClave;-)",
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    start_webserver();
}