#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gemmi_bridge.h"
#include "gemmi_display.h"
#include "gemmi_ui.h"

#define GEMMI_POLL_INTERVAL_MS 1500
#define GEMMI_HTTP_RESPONSE_BYTES 640
#define GEMMI_ANSWER_QUEUE_DEPTH 4

static const char *TAG = "gemmi_bridge";

typedef struct {
    char body[GEMMI_HTTP_RESPONSE_BYTES];
    size_t length;
} gemmi_http_response_t;

typedef struct {
    int event_id;
    char choice[GEMMI_CHOICE_LEN];
} gemmi_device_msg_t;

static QueueHandle_t device_msg_queue;
static int last_shown_id;

static esp_err_t collect_response(esp_http_client_event_t *event)
{
    gemmi_http_response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || response->length >= sizeof(response->body) - 1) {
        return ESP_OK;
    }

    size_t available = sizeof(response->body) - 1 - response->length;
    size_t copy_len = event->data_len < available ? event->data_len : available;
    memcpy(response->body + response->length, event->data, copy_len);
    response->length += copy_len;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static esp_http_client_handle_t open_client(const char *url, esp_http_client_method_t method,
                                            gemmi_http_response_t *response)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 3000,
        .event_handler = response ? collect_response : NULL,
        .user_data = response,
        .keep_alive_enable = true,
#if CONFIG_GEMMI_BRIDGE_REQUIRE_TLS
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return NULL;
    if (strlen(CONFIG_GEMMI_BRIDGE_TOKEN) > 0) {
        char authorization[128];
        snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_GEMMI_BRIDGE_TOKEN);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    return client;
}

/* POST a small JSON body. Choices are agent-provided text, so quotes and
 * backslashes are escaped before they are framed. */
static esp_err_t bridge_post(const char *path, int event_id, const char *choice)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s", CONFIG_GEMMI_BRIDGE_URL, path);

    char body[160];
    size_t used = snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"event_id\":%d",
                           CONFIG_GEMMI_DEVICE_ID, event_id);
    if (choice != NULL && *choice != '\0') {
        used += snprintf(body + used, sizeof(body) - used, ",\"choice\":\"");
        for (const char *c = choice; *c && used < sizeof(body) - 3; c++) {
            if (*c == '"' || *c == '\\') body[used++] = '\\';
            body[used++] = *c;
        }
        used += snprintf(body + used, sizeof(body) - used, "\"");
    }
    snprintf(body + used, sizeof(body) - used, "}");

    esp_http_client_handle_t client = open_client(url, HTTP_METHOD_POST, NULL);
    if (client == NULL) return ESP_FAIL;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || status >= 300) {
        ESP_LOGW(TAG, "POST %s failed: %s (%d)", path, esp_err_to_name(error), status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static char *take_line(char **cursor)
{
    char *line = *cursor;
    char *newline = strchr(line, '\n');
    if (newline == NULL) return NULL;
    *newline = '\0';
    *cursor = newline + 1;
    return line;
}

static bool parse_event(const char *body, gemmi_event_t *event)
{
    char mutable_body[GEMMI_HTTP_RESPONSE_BYTES];
    strlcpy(mutable_body, body, sizeof(mutable_body));
    char *cursor = mutable_body;
    char *id = take_line(&cursor);
    char *kind = id ? take_line(&cursor) : NULL;
    char *agent = kind ? take_line(&cursor) : NULL;
    char *state = agent ? take_line(&cursor) : NULL;
    char *choices = state ? take_line(&cursor) : NULL;
    char *artifact = choices ? take_line(&cursor) : NULL;
    char *message = (artifact != NULL && *cursor != '\0') ? cursor : NULL;
    if (id == NULL || kind == NULL || agent == NULL || state == NULL ||
        choices == NULL || artifact == NULL || message == NULL) {
        ESP_LOGW(TAG, "ignored malformed bridge response");
        return false;
    }

    memset(event, 0, sizeof(*event));
    event->id = atoi(id);
    if (event->id <= 0) return false;
    strlcpy(event->kind, kind, sizeof(event->kind));
    strlcpy(event->agent, agent, sizeof(event->agent));
    strlcpy(event->state, state, sizeof(event->state));
    strlcpy(event->artifact, artifact, sizeof(event->artifact));
    strlcpy(event->message, message, sizeof(event->message));
    for (char *token = strtok(choices, "|");
         token != NULL && event->choice_count < GEMMI_MAX_CHOICES;
         token = strtok(NULL, "|")) {
        strlcpy(event->choices[event->choice_count++], token, GEMMI_CHOICE_LEN);
    }
    return true;
}

void gemmi_bridge_answer(int event_id, const char *choice)
{
    if (device_msg_queue == NULL) return;
    gemmi_device_msg_t message = {.event_id = event_id};
    if (choice != NULL) strlcpy(message.choice, choice, sizeof(message.choice));
    if (xQueueSend(device_msg_queue, &message, 0) != pdTRUE) {
        ESP_LOGW(TAG, "device reply queue is full; dropped reply for %d", event_id);
    }
}

static void gemmi_bridge_task(void *arg)
{
    char url[192];
#if CONFIG_GEMMI_BRIDGE_REQUIRE_TOKEN
    if (strlen(CONFIG_GEMMI_BRIDGE_TOKEN) == 0) {
        ESP_LOGE(TAG, "bridge token is required; polling is disabled");
        vTaskDelete(NULL);
    }
#endif
#if CONFIG_GEMMI_BRIDGE_REQUIRE_TLS
    if (strncmp(CONFIG_GEMMI_BRIDGE_URL, "https://", 8) != 0) {
        ESP_LOGE(TAG, "HTTPS bridge URL is required; polling is disabled");
        vTaskDelete(NULL);
    }
#endif
    while (true) {
        gemmi_device_msg_t outgoing;
        if (xQueueReceive(device_msg_queue, &outgoing,
                          pdMS_TO_TICKS(GEMMI_POLL_INTERVAL_MS)) == pdTRUE) {
            if (outgoing.choice[0] != '\0') {
                bridge_post("/v1/device/respond", outgoing.event_id, outgoing.choice);
            } else {
                bridge_post("/v1/device/ack", outgoing.event_id, NULL);
            }
            continue;
        }

        snprintf(url, sizeof(url), "%s/v1/device/next?device_id=%s&after=%d",
                 CONFIG_GEMMI_BRIDGE_URL, CONFIG_GEMMI_DEVICE_ID, last_shown_id);
        gemmi_http_response_t response = {0};
        esp_http_client_handle_t client = open_client(url, HTTP_METHOD_GET, &response);
        if (client == NULL) continue;
        esp_http_client_set_header(client, "Accept", "text/plain");
        esp_err_t error = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (error != ESP_OK) {
            ESP_LOGD(TAG, "bridge unavailable: %s", esp_err_to_name(error));
            continue;
        }
        if (status != 200 || response.length == 0) continue;

        gemmi_event_t event;
        if (!parse_event(response.body, &event) || event.id <= last_shown_id) continue;
        last_shown_id = event.id;
        gemmi_display_lock();
        gemmi_ui_show_event(&event);
        gemmi_display_unlock();
        ESP_LOGI(TAG, "showing %s event %d from %s", event.kind, event.id, event.agent);
        /* Asks stay unacked until the human answers or dismisses them so a
         * reboot can resurface the open question. Everything else is acked
         * once it is on screen. */
        if (strcmp(event.kind, "ask") != 0) {
            bridge_post("/v1/device/ack", event.id, NULL);
        }
    }
}

void gemmi_bridge_start(void)
{
    device_msg_queue = xQueueCreate(GEMMI_ANSWER_QUEUE_DEPTH, sizeof(gemmi_device_msg_t));
    xTaskCreate(gemmi_bridge_task, "gemmi_bridge", 8192, NULL, 3, NULL);
}
