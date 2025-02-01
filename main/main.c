#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#define LED_GPIO 12
#define SERVO_GPIO 13
#define SERVO_MIN_PULSEWIDTH 500  // Pulso mínimo para 0 grados (microsegundos)
#define SERVO_MAX_PULSEWIDTH 2500 // Pulso máximo para 180 grados (microsegundos)
#define SERVO_MAX_DEGREE 180      // Grados máximos de rotación

static const char *TAG = "example";
static int led_state = 0;
static int servo_angle = 0;
static bool servo_direction = true; // true = hacia 180, false = hacia 0

// Función para convertir ángulos a ciclo de trabajo
static uint32_t degree_to_duty(int degree) {
    // Convertir el ángulo a ancho de pulso
    uint32_t pulse_width = SERVO_MIN_PULSEWIDTH + (((SERVO_MAX_PULSEWIDTH - SERVO_MIN_PULSEWIDTH) * degree) / SERVO_MAX_DEGREE);
    // Convertir el ancho de pulso a ciclo de trabajo (basado en frecuencia de 50Hz y resolución de 13 bits)
    uint32_t duty = (pulse_width * ((1 << 13) - 1)) / 20000;
    return duty;
}

const char html_page[] = "\
<!DOCTYPE html>\
<html>\
<head>\
    <title>Control LED y Servo</title>\
    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\
    <style>\
        body { font-family: Arial, sans-serif; text-align: center; background-color: #f3e5f5; color: #4a148c; margin: 20px; }\
        h1 { color: #6a1b9a; }\
        button { background-color: #8e24aa; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; margin: 10px; }\
        button:hover { background-color: #7b1fa2; }\
        .status { margin: 20px 0; }\
    </style>\
</head>\
<body>\
    <h1>Control de LED y Servo</h1>\
    <div class=\"status\">\
        <p>Estado del LED: <strong id=\"led_status\">Apagado</strong></p>\
        <button onclick=\"toggleLED()\">Encender/Apagar LED</button>\
    </div>\
    <div class=\"status\">\
        <p>Estado del Servo: <strong id=\"servo_status\">Ángulo: 0°</strong></p>\
        <button onclick=\"moveServo()\">Mover Servo 0° - 180°</button>\
    </div>\
    <script>\
        function toggleLED() {\
            fetch('/toggle_led', { method: 'POST' })\
                .then(response => response.text())\
                .then(state => {\
                    document.getElementById('led_status').innerText = state === '1' ? 'Encendido' : 'Apagado';\
                })\
                .catch(error => console.error('Error:', error));\
        }\
        function moveServo() {\
            fetch('/move_servo', { method: 'POST' })\
                .then(response => response.text())\
                .then(angle => {\
                    document.getElementById('servo_status').innerText = 'Ángulo: ' + angle + '°';\
                })\
                .catch(error => console.error('Error:', error));\
        }\
    </script>\
</body>\
</html>";

static esp_err_t page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t toggle_led_handler(httpd_req_t *req) {
    led_state = !led_state;
    gpio_set_level(LED_GPIO, led_state);
    char resp[4];
    snprintf(resp, sizeof(resp), "%d", led_state);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t move_servo_handler(httpd_req_t *req) {
    // Determinar el siguiente ángulo basado en la dirección actual
    if (servo_direction) {
        servo_angle = 180;  // Ir a 180 grados
    } else {
        servo_angle = 0;    // Regresar a 0 grados
    }
    
    // Cambiar la dirección para el siguiente movimiento
    servo_direction = !servo_direction;
    
    // Aplicar el nuevo ángulo
    uint32_t duty = degree_to_duty(servo_angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    
    char resp[4];
    snprintf(resp, sizeof(resp), "%d", servo_angle);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t uri_page = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = page_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_toggle_led = {
    .uri      = "/toggle_led",
    .method   = HTTP_POST,
    .handler  = toggle_led_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_move_servo = {
    .uri      = "/move_servo",
    .method   = HTTP_POST,
    .handler  = move_servo_handler,
    .user_ctx = NULL
};

static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &uri_page);
        httpd_register_uri_handler(server, &uri_toggle_led);
        httpd_register_uri_handler(server, &uri_move_servo);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting application...");
    
    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicializar la red
    ESP_LOGI(TAG, "Initializing network...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    // Configurar el LED
    ESP_LOGI(TAG, "Configuring LED...");
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, led_state);

    // Configurar el servo
    ESP_LOGI(TAG, "Configuring servo...");
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = SERVO_GPIO,
        .duty = degree_to_duty(0), // Iniciar en 0 grados
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    // Iniciar el servidor web
    ESP_LOGI(TAG, "Starting web server...");
    start_webserver();
    ESP_LOGI(TAG, "Setup complete!");
}