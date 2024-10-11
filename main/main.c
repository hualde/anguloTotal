#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <inttypes.h>

#define TX_GPIO_NUM 18
#define RX_GPIO_NUM 19

#define WIFI_SSID "ESP32_AP"
#define WIFI_PASS ""

#define MAX_STORED_MESSAGES 20

#define TWAI_TX_TASK_PRIO 9
#define TWAI_RX_TASK_PRIO 8
#define EXAMPLE_DELAY_BETWEEN_MSGS_MS 100
#define MAX_RETRIES 3

static const char *TAG = "TWAI_APP";

QueueHandle_t message_queue;

typedef struct {
    twai_message_t message;
    const char* status;
} message_with_status_t;

message_with_status_t stored_messages[MAX_STORED_MESSAGES];
int stored_message_count = 0;

twai_message_t angle_config_messages[] = {
    {.identifier = 0x742, .data_length_code = 8, .data = {0x03, 0x14, 0xFF, 0x00, 0xB6, 0x01, 0xFF, 0xFF}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x03, 0x31, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x03, 0x31, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00}}
};

twai_message_t status_check_messages[] = {
    {.identifier = 0x742, .data_length_code = 8, .data = {0x02, 0x10, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x02, 0x21, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x02, 0x21, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x02, 0x21, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x02, 0x21, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {.identifier = 0x742, .data_length_code = 8, .data = {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}
};

static esp_timer_handle_t countdown_timer;
static int countdown_value = 30;
static bool countdown_active = false;
static bool timer_created = false;
static int current_status = 3; // 3: Status 3, 4: Status 4

void send_twai_messages(twai_message_t* messages, int count) {
    for (int i = 0; i < count; i++) {
        int retries = 0;
        esp_err_t result;
        do {
            result = twai_transmit(&messages[i], pdMS_TO_TICKS(1000));
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "Message %d successfully sent: ID=0x%03" PRIx32 ", DLC=%d", i + 1, messages[i].identifier, messages[i].data_length_code);
                break;
            } else {
                ESP_LOGE(TAG, "Failed to send message %d, error: %s. Retry %d", i + 1, esp_err_to_name(result), retries + 1);
                retries++;
                
                twai_status_info_t status;
                twai_get_status_info(&status);
                ESP_LOGI(TAG, "TWAI status: state=%lu, msgs_to_tx=%lu, msgs_to_rx=%lu, tx_error_counter=%lu, rx_error_counter=%lu",
                         (unsigned long)status.state, (unsigned long)status.msgs_to_tx, (unsigned long)status.msgs_to_rx,
                         (unsigned long)status.tx_error_counter, (unsigned long)status.rx_error_counter);

                if (status.state == TWAI_STATE_BUS_OFF) {
                    ESP_LOGI(TAG, "Bus off, initiating recovery");
                    twai_initiate_recovery();
                    vTaskDelay(pdMS_TO_TICKS(5000));  // Wait for recovery
                } else if (status.state == TWAI_STATE_STOPPED) {
                    ESP_LOGI(TAG, "TWAI stopped, restarting");
                    twai_start();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                } else {
                    // Try to recover from error state
                    twai_stop();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    twai_start();
                    ESP_LOGI(TAG, "TWAI restarted after error");
                }
            }
        } while (retries < MAX_RETRIES);

        if (retries == MAX_RETRIES) {
            ESP_LOGE(TAG, "Failed to send message %d after %d retries", i + 1, MAX_RETRIES);
        }

        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_DELAY_BETWEEN_MSGS_MS));
    }
}

void twai_receive_task(void *pvParameters) {
    twai_message_t rx_message;
    message_with_status_t message_with_status;
    while (1) {
        esp_err_t result = twai_receive(&rx_message, pdMS_TO_TICKS(10000));
        if (result == ESP_OK) {
            if (rx_message.identifier == 0x762 && rx_message.data[0] == 0x23 && rx_message.data[1] == 0x00) {
                ESP_LOGI(TAG, "Received 0x762 frame: %02X %02X %02X %02X %02X %02X %02X %02X",
                         rx_message.data[0], rx_message.data[1], rx_message.data[2], rx_message.data[3],
                         rx_message.data[4], rx_message.data[5], rx_message.data[6], rx_message.data[7]);
    
                if ((rx_message.data[3] & 0x0F) == 0x0C) {
                    current_status = 4;
                    message_with_status.status = "Status 4";
                    ESP_LOGI(TAG, "Status 4 detected");
                } else {
                    current_status = 3;
                    message_with_status.status = "Status 3";
                    ESP_LOGI(TAG, "Status 3 detected");
                }
    
                message_with_status.message = rx_message;
    
                if (stored_message_count < MAX_STORED_MESSAGES) {
                    stored_messages[stored_message_count++] = message_with_status;
                } else {
                    for (int i = 0; i < MAX_STORED_MESSAGES - 1; i++) {
                        stored_messages[i] = stored_messages[i + 1];
                    }
                    stored_messages[MAX_STORED_MESSAGES - 1] = message_with_status;
                }
            }
        } else if (result != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Failed to receive message, error: %s", esp_err_to_name(result));
            
            twai_status_info_t status;
            twai_get_status_info(&status);
            ESP_LOGI(TAG, "TWAI status: state=%lu, msgs_to_tx=%lu, msgs_to_rx=%lu, tx_error_counter=%lu, rx_error_counter=%lu",
                     (unsigned long)status.state, (unsigned long)status.msgs_to_tx, (unsigned long)status.msgs_to_rx,
                     (unsigned long)status.tx_error_counter, (unsigned long)status.rx_error_counter);

            if (status.state == TWAI_STATE_BUS_OFF) {
                ESP_LOGI(TAG, "Bus off, initiating recovery");
                twai_initiate_recovery();
                vTaskDelay(pdMS_TO_TICKS(5000));  // Wait for recovery
            } else if (status.state == TWAI_STATE_STOPPED) {
                ESP_LOGI(TAG, "TWAI stopped, restarting");
                twai_start();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void wifi_init_softap() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started. SSID:%s (Open Network)", WIFI_SSID);
}

static void countdown_timer_callback(void* arg) {
    countdown_value--;
    ESP_LOGI(TAG, "Countdown: %d", countdown_value);
    if (countdown_value <= 0) {
        esp_timer_stop(countdown_timer);
        countdown_active = false;
        send_twai_messages(angle_config_messages, sizeof(angle_config_messages) / sizeof(angle_config_messages[0]));
    }
}

esp_err_t get_handler(httpd_req_t *req) {
    char *response = malloc(8192);
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for response");
        return ESP_FAIL;
    }

    char *p = response;
    
    // Check if it's an AJAX request
    char *header = NULL;
    size_t header_len = httpd_req_get_hdr_value_len(req, "X-Requested-With");
    if (header_len > 0) {
        header = malloc(header_len + 1);
        if (httpd_req_get_hdr_value_str(req, "X-Requested-With", header, header_len + 1) == ESP_OK) {
            if (strcmp(header, "XMLHttpRequest") == 0) {
                // It's an AJAX request, only send the message list
                p += sprintf(p, "<ul id='messageList'>");
                for (int i = 0; i < stored_message_count; i++) {
                    p += sprintf(p, "<li>ID: 0x762, Data: ");
                    for (int j = 0; j < stored_messages[i].message.data_length_code; j++) {
                        p += sprintf(p, "%02X ", stored_messages[i].message.data[j]);
                    }
                    p += sprintf(p, ", Status: %s</li>", stored_messages[i].status);
                }
                p += sprintf(p, "</ul>");
                free(header);
                httpd_resp_send(req, response, strlen(response));
                free(response);
                return ESP_OK;
            }
        }
        free(header);
    }

    // If it's not an AJAX request, send the full HTML page
    p += sprintf(p, "<!DOCTYPE html>");
    p += sprintf(p, "<html lang='es'><head><meta charset='UTF-8'><style>");
    p += sprintf(p, "body { font-family: Arial, sans-serif; margin: 0; padding: 20px; }");
    p += sprintf(p, ".button { background-color: #E4007B; border: none; color: white; padding: 15px 32px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; }");
    p += sprintf(p, ".status { margin-left: 20px; display: inline-block; }");
    p += sprintf(p, "#countdownBtn { display: block; margin: 20px 0; }");
    p += sprintf(p, "#instructions { margin-top: 10px; }");
    p += sprintf(p, ".status-box { background-color: #f0f0f0; padding: 10px; border-radius: 5px; display: inline-block; margin-left: 20px; }");
    p += sprintf(p, ".calibration-complete { background-color: #4CAF50; padding: 10px; border-radius: 5px; display: inline-block; margin-left: 20px; color: white; }");
    p += sprintf(p, "</style></head><body>");
    p += sprintf(p, "<h1>ESP32-C3 CAN Control Panel</h1>");
    p += sprintf(p, "<div>");
    p += sprintf(p, "<button class='button' onclick='sendStatusCheck()'>Comprobar estado de la configuración de ángulo de volante</button>");
    p += sprintf(p, "<span id='statusBox' class='status-box' style='display: none;'></span>");
    p  += sprintf(p, "</div>");
    p += sprintf(p, "<br><br>");
    
    p += sprintf(p, "<div style=\"display: flex; align-items: center;\">");
    p += sprintf(p, "<button id='countdownBtn' class='button' onclick='startCountdown()'>Calibrar ángulo de volante</button>"); 
    p += sprintf(p, "<span id='calibrationStatus' class='calibration-complete' style='display: none;'>Calibración completa</span>");
    p += sprintf(p, "</div>");
    p += sprintf(p, "<div id='instructions' style='display:none;'>");
    p += sprintf(p, "1. Con el motor encendido, ponga el volante/ruedas en el centro.<br>");
    p += sprintf(p, "2. Gire el volante a la izquierda hasta el tope.<br>");
    p += sprintf(p, "3. Gire el volante a la derecha hasta el tope.<br>");
    p += sprintf(p, "4. Vuelva a centrar el volante/ruedas y espere a que finalice la cuenta atrás.<br>");
    p += sprintf(p, "5. Una vez finalizada este proceso, apague el coche y vuelva a encenderlo<br>");
    p += sprintf(p, "</div>");
    p += sprintf(p, "<div id='messageListContainer'><ul id='messageList'>");

    for (int i = 0; i < stored_message_count; i++) {
        p += sprintf(p, "<li>ID: 0x762, Data: ");
        for (int j = 0; j < stored_messages[i].message.data_length_code; j++) {
            p += sprintf(p, "%02X ", stored_messages[i].message.data[j]);
        }
        p += sprintf(p, ", Status: %s</li>", stored_messages[i].status);
    }

    p += sprintf(p, "</ul></div>");
    p += sprintf(p, "<script>");
    p += sprintf(p, "var countdown = 30;");
    p += sprintf(p, "var countdownActive = false;");
    p += sprintf(p, "var countdownInterval;");
    p += sprintf(p, "function startCountdown() {");
    p += sprintf(p, "  if (!countdownActive) {");
    p += sprintf(p, "    countdownActive = true;");
    p += sprintf(p, "    countdown = 30;");
    p += sprintf(p, "    document.getElementById('instructions').style.display = 'block';");
    p += sprintf(p, "    updateButton();");
    p += sprintf(p, "    fetch('/start_countdown', { method: 'POST' })");
    p += sprintf(p, "      .then(response => response.text())");
    p += sprintf(p, "      .then(data => {");
    p += sprintf(p, "        console.log(data);");
    p += sprintf(p, "        countdownInterval = setInterval(function() {");
    p += sprintf(p, "          countdown--;");
    p += sprintf(p, "          updateButton();");
    p += sprintf(p, "          if (countdown <= 0) {");
    p += sprintf(p, "            clearInterval(countdownInterval);");
    p += sprintf(p, "            countdownActive = false;");
    p += sprintf(p, "            fetch('/calibrate', { method: 'POST' })");
    p += sprintf(p, "              .then(response => response.text())");
    p += sprintf(p, "              .then(data => {");
    p += sprintf(p, "                console.log(data);");
    p += sprintf(p, "                document.getElementById('calibrationStatus').innerHTML = 'Calibración completa';");
    p += sprintf(p, "                document.getElementById('calibrationStatus').style.display = 'inline-block';");
    p += sprintf(p, "              });");
    p += sprintf(p, "          }");
    p += sprintf(p, "        }, 1000);");
    p += sprintf(p, "      });");
    p += sprintf(p, "  }");
    p += sprintf(p, "}");
    p += sprintf(p, "function updateButton() {");
    p += sprintf(p, "  var btn = document.getElementById('countdownBtn');");
    p += sprintf(p, "  if (countdownActive) {");
    p += sprintf(p, "    btn.innerHTML = 'Calibrando: ' + countdown + 's';"); 
    p += sprintf(p, "    btn.disabled = true;");
    p += sprintf(p, "  } else {");
    p += sprintf(p, "    btn.innerHTML = 'Calibrar ángulo de volante';"); 
    p += sprintf(p, "    btn.disabled = false;");
    p += sprintf(p, "  }");
    p += sprintf(p, "}");
    p += sprintf(p, "function sendStatusCheck() {");
    p += sprintf(p, "  fetch('/status_check', { method: 'POST' })");
    p += sprintf(p, "    .then(response => response.json())");
    p += sprintf(p, "    .then(data => {");
    p += sprintf(p, "      console.log(data);");
    p += sprintf(p, "      var statusBox = document.getElementById('statusBox');");
    p += sprintf(p, "      statusBox.innerHTML = 'Status ' + data.status;");
    p += sprintf(p, "      statusBox.style.display = 'inline-block';");
    p += sprintf(p, "    });");
    p += sprintf(p, "}");
    p += sprintf(p, "function updateMessages() {");
    p += sprintf(p, "  fetch('/', { headers: { 'X-Requested-With': 'XMLHttpRequest' } })");
    p += sprintf(p, "    .then(response => response.text())");
    p += sprintf(p, "    .then(html => {");
    p += sprintf(p, "      document.getElementById('messageListContainer').innerHTML = html;");
    p += sprintf(p, "    });");
    p += sprintf(p, "}");
    p += sprintf(p, "setInterval(updateMessages, 5000);");
    p += sprintf(p, "</script>");
    p += sprintf(p, "</body></html>");

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    return ESP_OK;
}

esp_err_t start_countdown_handler(httpd_req_t *req) {
    if (countdown_active) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Countdown already active");
        return ESP_FAIL;
    }
    
    countdown_value = 30;
    countdown_active = true;
    esp_err_t err = esp_timer_start_periodic(countdown_timer, 1000000); // 1 second interval
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(err));
        countdown_active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start timer");
        return ESP_FAIL;
    }
    
    httpd_resp_sendstr(req, "Countdown started");
    return ESP_OK;
}

esp_err_t calibrate_handler(httpd_req_t *req) {
    httpd_resp_sendstr(req, "Calibration complete");
    return ESP_OK;
}

esp_err_t status_check_handler(httpd_req_t *req) {
    send_twai_messages(status_check_messages, sizeof(status_check_messages) / sizeof(status_check_messages[0]));
    
    // Wait for a short period to allow for message processing
    vTaskDelay(pdMS_TO_TICKS(500));
    
    char response[32];
    snprintf(response, sizeof(response), "{\"status\": %d}", current_status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

httpd_handle_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_start_countdown = {
            .uri       = "/start_countdown",
            .method    = HTTP_POST,
            .handler   = start_countdown_handler,
            .user_ctx  = NULL
        };
        
        httpd_register_uri_handler(server, &uri_start_countdown);

        httpd_uri_t uri_calibrate = {
            .uri       = "/calibrate",
            .method    = HTTP_POST,
            .handler   = calibrate_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_calibrate);

        httpd_uri_t uri_status_check = {
            .uri       = "/status_check",
            .method    = HTTP_POST,
            .handler   = status_check_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_status_check);
    }
    return server;
}

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_softap();

    httpd_handle_t server = start_webserver();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start webserver");
    } else {
        ESP_LOGI(TAG, "Webserver started successfully");
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO_NUM, RX_GPIO_NUM, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());

    ESP_LOGI(TAG, "TWAI driver installed and started");

    xTaskCreate(twai_receive_task, "TWAI_receive_task", 4096, NULL, TWAI_RX_TASK_PRIO, NULL);

    if (!timer_created) {
        esp_timer_create_args_t timer_args = {
            .callback = &countdown_timer_callback,
            .name = "countdown_timer"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &countdown_timer));
        timer_created = true;
    }
}