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
    int attempts;
    char choice[GEMMI_CHOICE_LEN];
} gemmi_device_msg_t;

static QueueHandle_t device_msg_queue;
static int last_shown_id;

typedef struct { uint8_t *buf; size_t len; } gemmi_image_t;

/* Static image buffer — a malloc here can fail under heap fragmentation and
 * silently starve the fetch. Persistent storage also survives for `view`. */
static uint8_t image_raw[14000];
static gemmi_image_t img = {.buf = image_raw};
static bool collecting_image;

static esp_err_t collect_response(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && collecting_image) {
        size_t avail = sizeof(image_raw) - img.len;
        size_t copy = event->data_len < avail ? event->data_len : avail;
        memcpy(image_raw + img.len, event->data, copy);
        img.len += copy;
        return ESP_OK;
    }
    gemmi_http_response_t *response = event->user_data;
    if (response == NULL || event->event_id != HTTP_EVENT_ON_DATA ||
        response->length >= sizeof(response->body) - 1) {
        return ESP_OK;
    }

    size_t available = sizeof(response->body) - 1 - response->length;
    size_t copy_len = event->data_len < available ? event->data_len : available;
    memcpy(response->body + response->length, event->data, copy_len);
    response->length += copy_len;
    response->body[response->length] = '\0';
    return ESP_OK;
}

/* One persistent client per session: a fresh TLS handshake to the Funnel on
 * every poll burns a full CPU core for seconds and trips the task watchdog. */
static esp_http_client_handle_t client;

static esp_err_t reset_client(void)
{
    if (client) esp_http_client_cleanup(client);
    esp_http_client_config_t config = {
        .url = CONFIG_GEMMI_BRIDGE_URL,
        .timeout_ms = 3000,
        /* A restarted bridge leaves a half-open keep-alive socket that makes
         * perform hang instead of erroring — always take a fresh socket. */
        .keep_alive_enable = false,
        .event_handler = collect_response,
#if CONFIG_GEMMI_BRIDGE_REQUIRE_TLS
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    client = esp_http_client_init(&config);
    if (client == NULL) return ESP_FAIL;
    if (strlen(CONFIG_GEMMI_BRIDGE_TOKEN) > 0) {
        char authorization[128];
        snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_GEMMI_BRIDGE_TOKEN);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    return ESP_OK;
}

/* POST a small JSON body. Choices are agent-provided text, so quotes and
 * backslashes are escaped before they are framed. */
static esp_err_t bridge_post(const char *path, int event_id, const char *choice)
{
    if (client == NULL && reset_client() != ESP_OK) return ESP_FAIL;

    char url[192];
    snprintf(url, sizeof(url), "%s%s", CONFIG_GEMMI_BRIDGE_URL, path);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_user_data(client, NULL);

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
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (error != ESP_OK) {
        /* A dead keep-alive connection is normal after network hiccups;
         * drop it and retry once on a fresh socket before failing. */
        if (reset_client() == ESP_OK) {
            esp_http_client_set_url(client, url);
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_user_data(client, NULL);
            esp_http_client_set_post_field(client, body, (int)strlen(body));
            error = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
        }
    }
    if (error != ESP_OK || status >= 300) {
        ESP_LOGW(TAG, "POST %s failed: %s (%d)", path, esp_err_to_name(error), status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Fetch a stored RGB565 artifact on the shared client (a second TLS socket
 * through the funnel fails to connect). The collecting_image flag makes
 * collect_response divert binary data into the static buffer — callers must
 * not free it. */
static gemmi_image_t fetch_image(int event_id)
{
    img.len = 0;
    if (client == NULL && reset_client() != ESP_OK) return img;
    char url[192];
    snprintf(url, sizeof(url), "%s/v1/events/%d/image", CONFIG_GEMMI_BRIDGE_URL, event_id);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_post_field(client, NULL, 0);
    collecting_image = true;
    esp_err_t error = esp_http_client_perform(client);
    collecting_image = false;
    int status = esp_http_client_get_status_code(client);
    esp_http_client_set_header(client, "Accept", "text/plain");
    ESP_LOGI(TAG, "image %d: err=%s status=%d len=%u head=%02x%02x%02x%02x",
             event_id, esp_err_to_name(error), status, (unsigned)img.len,
             img.len > 0 ? img.buf[0] : 0, img.len > 1 ? img.buf[1] : 0,
             img.len > 2 ? img.buf[2] : 0, img.len > 3 ? img.buf[3] : 0);
    if (error != ESP_OK || status != 200 || img.len == 0) {
        return (gemmi_image_t){0};
    }
    return img;
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
    bool just_shown = false;
    int poll_failures = 0;
    while (true) {
        gemmi_device_msg_t outgoing;
        /* After displaying an event, poll again quickly so a burst of queued
         * events drains fast; idle periods keep the relaxed cadence. */
        TickType_t wait = pdMS_TO_TICKS(just_shown ? 50 : GEMMI_POLL_INTERVAL_MS);
        if (xQueueReceive(device_msg_queue, &outgoing, wait) == pdTRUE) {
            esp_err_t sent;
            if (outgoing.choice[0] != '\0') {
                sent = bridge_post("/v1/device/respond", outgoing.event_id, outgoing.choice);
            } else {
                sent = bridge_post("/v1/device/ack", outgoing.event_id, NULL);
            }
            /* A dropped answer leaves the agent waiting forever, so retry a
             * few times before giving up. The slot we just consumed keeps
             * room in the queue for the requeue. */
            if (sent != ESP_OK && ++outgoing.attempts < 6 &&
                xQueueSendToBack(device_msg_queue, &outgoing, 0) == pdTRUE) {
                ESP_LOGI(TAG, "queued retry %d for event %d", outgoing.attempts, outgoing.event_id);
                continue;
            }
            /* A response that never landed means the ask is still unconsumed
             * on the bridge; rewind past it so it resurfaces for a retry
             * instead of vanishing while showing "sent". */
            if (sent != ESP_OK && outgoing.choice[0] != '\0' &&
                outgoing.event_id <= last_shown_id) {
                ESP_LOGW(TAG, "answer for %d never reached the bridge; "
                         "resurfacing the ask", outgoing.event_id);
                last_shown_id = outgoing.event_id - 1;
            }
            continue;
        }

        /* An open ask or image is a blocking item: hold off consuming the
         * next event until the human clears this one — a real queue. */
        if (gemmi_ui_is_blocked()) {
            just_shown = false;
            continue;
        }

        if (client == NULL && reset_client() != ESP_OK) continue;
        snprintf(url, sizeof(url), "%s/v1/device/next?device_id=%s&after=%d",
                 CONFIG_GEMMI_BRIDGE_URL, CONFIG_GEMMI_DEVICE_ID, last_shown_id);
        esp_http_client_set_url(client, url);
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        esp_http_client_set_header(client, "Accept", "text/plain");
        esp_http_client_set_post_field(client, NULL, 0);
        gemmi_http_response_t response = {0};
        esp_http_client_set_user_data(client, &response);
        esp_err_t error = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (error != ESP_OK) {
            ESP_LOGD(TAG, "bridge unavailable: %s", esp_err_to_name(error));
            reset_client();
            if (++poll_failures == 3) {
                gemmi_display_lock();
                gemmi_ui_offline(true);
                gemmi_display_unlock();
            }
            continue;
        }
        if (poll_failures >= 3) {
            gemmi_display_lock();
            gemmi_ui_offline(false);
            gemmi_display_unlock();
        }
        poll_failures = 0;
        if (status != 200 || response.length == 0) {
            just_shown = false;
            continue;
        }

        gemmi_event_t event;
        if (!parse_event(response.body, &event) || event.id <= last_shown_id) {
            just_shown = false;
            continue;
        }
        last_shown_id = event.id;
        just_shown = true;
        gemmi_display_lock();
        gemmi_ui_show_event(&event);
        gemmi_display_unlock();
        bool image_shown = false;
        /* Doodles are the device's own drawings — don't bounce them back as
         * a blocking image view, just ack and keep moving. */
        if (strcmp(event.kind, "artifact") == 0 &&
            strcmp(event.artifact, "doodle") != 0) {
            gemmi_image_t img = fetch_image(event.id);
            if (img.len > 0) {
                gemmi_display_lock();
                gemmi_ui_show_image(img.buf, img.len);
                gemmi_display_unlock();
                image_shown = true;
            }
        }
        ESP_LOGI(TAG, "showing %s event %d from %s", event.kind, event.id, event.agent);
        /* Asks and images stay unacked until the human clears them so a
         * reboot can resurface the item. Everything else is acked once it
         * is on screen. */
        if (strcmp(event.kind, "ask") != 0 && !image_shown) {
            bridge_post("/v1/device/ack", event.id, NULL);
        }
    }
}

/* POST a fully-assembled JSON body — used by the doodle sender. */
esp_err_t gemmi_bridge_post_raw(const char *path, const char *body)
{
    if (client == NULL && reset_client() != ESP_OK) return ESP_FAIL;
    char url[192];
    snprintf(url, sizeof(url), "%s%s", CONFIG_GEMMI_BRIDGE_URL, path);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_user_data(client, NULL);
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (error != ESP_OK && reset_client() == ESP_OK) {
        /* dead keep-alive — retry once on a fresh socket */
        esp_http_client_set_url(client, url);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_user_data(client, NULL);
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        error = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "post_raw %s failed: %s (%d)", path, esp_err_to_name(error), status);
    }
    return (error == ESP_OK && (status == 200 || status == 201)) ? ESP_OK : ESP_FAIL;
}

void gemmi_bridge_start(void)
{
    device_msg_queue = xQueueCreate(GEMMI_ANSWER_QUEUE_DEPTH, sizeof(gemmi_device_msg_t));
    xTaskCreate(gemmi_bridge_task, "gemmi_bridge", 8192, NULL, 3, NULL);
}
