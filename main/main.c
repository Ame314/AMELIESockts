#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
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

// GPIO Pin definitions
#define LED_GPIO 12        
#define TRIGGER_GPIO 5     
#define ECHO_GPIO 18    
#define RELAY_GPIO 27

// Global variables
static const char *TAG = "coffee-maker";
static int daily_uses = 0;
static int total_cups = 0;
static const char* SERVER_URL = "http://192.168.1.23:8087";

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
    free(pvParameters);

    int brewing_time = (cups == 2) ? 100 : (cups == 4) ? 200 : 400;

    // Turn on relay and LED
    gpio_set_level(RELAY_GPIO, 1);  // Turn on relay (active low) 0
    gpio_set_level(LED_GPIO, 1);    // Turn on LED

    // Wait for brewing time
    vTaskDelay(brewing_time * 1000 / portTICK_PERIOD_MS);
    
    // Turn off relay and LED
    gpio_set_level(RELAY_GPIO, 0);  // Turn off relay
    gpio_set_level(LED_GPIO, 0);    // Turn off LED

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
const char html_page[] = "<!DOCTYPE html>\n\
<html>\n\
<head>\n\
    <title>Control de Cafetera IoT</title>\n\
    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n\
    <style>\n\
        body { font-family: Arial, sans-serif; text-align: center; background:rgb(233, 208, 250); margin: 0; padding: 0; }\n\
        .container { max-width: 500px; margin: 50px auto; padding: 20px; background: white; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.2); }\n\
        h1 { color: #333; }\n\
        .control-panel, .status { padding: 20px; border-radius: 8px; margin: 20px 0; background:rgb(197, 198, 243); }\n\
        select, button { width: 100%; padding: 10px; margin-top: 10px; border: none; border-radius: 5px; font-size: 16px; }\n\
        button { background:rgb(159, 250, 85); color: white; cursor: pointer; transition: 0.3s; }\n\
        button:hover { background:rgb(118, 247, 86); }\n\
        .status p { font-size: 18px; }\n\
    </style>\n\
</head>\n\
<body>\n\
    <div class=\"container\">\n\
        <h1>Cafetera IoT</h1>\n\
        <div class=\"control-panel\">\n\
            <h2>Control de Preparacion</h2>\n\
            <select id=\"cups\">\n\
                <option value=\"2\">2 Tazas (1 min 40 seg)</option>\n\
                <option value=\"4\">4 Tazas (3 min 20 seg)</option>\n\
                <option value=\"8\">8 Tazas (6 min 40 seg)</option>\n\
            </select>\n\
            <button onclick=\"startCoffee()\">Preparar Cafe</button>\n\
        </div>\n\
        <div class=\"status\">\n\
            <h3>Estado</h3>\n\
            <p>Nivel de agua: <span id=\"waterLevel\">Midiendo...</span></p>\n\
            <p>Estado: <span id=\"status\">Listo</span></p>\n\
            <button onclick=\"checkWater()\">Verificar Agua</button>\n\
        </div>\n\
    </div>\n\
    <script>\n\
        async function startCoffee() {\n\
            const cups = document.getElementById('cups').value;\n\
            document.getElementById('status').innerText = 'Preparando...';\n\
            try {\n\
                const response = await fetch('/make_coffee', {\n\
                    method: 'POST',\n\
                    headers: { 'Content-Type': 'application/json' },\n\
                    body: JSON.stringify({ cups })\n\
                });\n\
                const result = await response.text();\n\
                document.getElementById('status').innerText = result;\n\
            } catch (error) {\n\
                document.getElementById('status').innerText = 'Error al preparar cafe';\n\
            }\n\
        }\n\
        async function checkWater() {\n\
            document.getElementById('waterLevel').innerText = 'Verificando...';\n\
            try {\n\
                const response = await fetch('/check_water', { method: 'POST' });\n\
                const level = await response.text();\n\
                document.getElementById('waterLevel').innerText = level;\n\
            } catch (error) {\n\
                document.getElementById('waterLevel').innerText = 'Error al verificar';\n\
            }\n\
        }\n\
    </script>\n\
</body>\n\
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
    snprintf(response, sizeof(response), "Preparando %d tazas de cafe", cups);
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
    gpio_reset_pin(RELAY_GPIO);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_GPIO, 1);  // Initialize relay as off

    gpio_reset_pin(TRIGGER_GPIO);
    gpio_reset_pin(ECHO_GPIO);
    gpio_set_direction(TRIGGER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_GPIO, GPIO_MODE_INPUT);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

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