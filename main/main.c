/* Persistent Sockets Example with LED Control */

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "esp_http_server.h"
#include "driver/gpio.h" // Se agregó la librería para manejar GPIO

#define LED_GPIO 12  // Define el pin del LED
static const char *TAG = "example";
static int led_state = 0; // Estado del LED (0 = apagado, 1 = encendido)

/* Página web HTML */
const char html_page[] = "\
<!DOCTYPE html>\
<html>\
<head>\
    <title>Control LED</title>\
    <style>\
        body { font-family: Arial, sans-serif; text-align: center; background-color: #f3e5f5; color: #4a148c; }\
        h1 { color: #6a1b9a; }\
        button { background-color: #8e24aa; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; }\
        button:hover { background-color: #7b1fa2; }\
    </style>\
    <script>\
        function toggleLED() {\
            fetch('/toggle_led', { method: 'POST' })\
                .then(response => response.text())\
                .then(state => {\
                    document.getElementById('led_status').innerText = state == '1' ? 'Encendido' : 'Apagado';\
                });\
        }\
    </script>\
</head>\
<body>\
    <h1>Control de LED</h1>\
    <p>Estado del LED: <strong id=\"led_status\">Apagado</strong></p>\
    <button onclick=\"toggleLED()\">Encender/Apagar LED</button>\
</body>\
</html>";

/* Manejador para servir la página web */
static esp_err_t page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Manejador para encender y apagar el LED */
static esp_err_t toggle_led_handler(httpd_req_t *req) {
    led_state = !led_state; // Alternar estado del LED
    gpio_set_level(LED_GPIO, led_state);
    char resp[4]; // Se aumentó el tamaño del buffer
    snprintf(resp, sizeof(resp), "%d", led_state);
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

static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_page);
        httpd_register_uri_handler(server, &uri_toggle_led);
        return server;
    }

    return NULL;
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    // Configurar el pin del LED
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, led_state);

    // Iniciar el servidor web
    start_webserver();
}