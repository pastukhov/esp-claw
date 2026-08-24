/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_voice_whisper.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cap_voice_whisper";

/* Dynamic response buffer grown on each HTTP event */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} whisper_buf_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    whisper_buf_t *b = (whisper_buf_t *)evt->user_data;
    if (b->len + evt->data_len > b->cap) {
        size_t new_cap = b->cap + evt->data_len + 4096;
        uint8_t *tmp = heap_caps_realloc(b->data, new_cap,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) return ESP_ERR_NO_MEM;
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

/* Build a minimal WAV header + PCM payload in a single buffer (PSRAM). */
static uint8_t *build_wav(const int16_t *samples, size_t num_samples,
                            size_t *out_len)
{
    const uint32_t sample_rate = 16000;
    const uint16_t channels    = 1;
    const uint16_t bits        = 16;
    const uint32_t data_size   = num_samples * sizeof(int16_t);
    const uint32_t total       = 44 + data_size;

    uint8_t *buf = heap_caps_malloc(total,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;

    /* RIFF header */
    memcpy(buf,      "RIFF", 4);
    uint32_t chunk = total - 8;
    memcpy(buf + 4,  &chunk, 4);
    memcpy(buf + 8,  "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(buf + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1;  /* PCM */
    memcpy(buf + 20, &audio_fmt, 2);
    memcpy(buf + 22, &channels, 2);
    memcpy(buf + 24, &sample_rate, 4);
    uint32_t byte_rate = sample_rate * channels * bits / 8;
    memcpy(buf + 28, &byte_rate, 4);
    uint16_t block_align = channels * bits / 8;
    memcpy(buf + 32, &block_align, 2);
    memcpy(buf + 34, &bits, 2);
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &data_size, 4);
    memcpy(buf + 44, samples, data_size);

    *out_len = total;
    return buf;
}

esp_err_t cap_voice_whisper_transcribe(const cap_voice_whisper_config_t *cfg,
                                        const int16_t *samples, size_t num_samples,
                                        char **out_text)
{
    *out_text = NULL;
    size_t wav_len = 0;
    uint8_t *wav = build_wav(samples, num_samples, &wav_len);
    if (!wav) return ESP_ERR_NO_MEM;

    const char *boundary  = "----ESP32WavBoundary";
    const char *model     = cfg->model && cfg->model[0] ? cfg->model : "whisper-1";
    const char *lang      = cfg->language ? cfg->language : "ru";

    char part_hdr[256];
    int phlen = snprintf(part_hdr, sizeof(part_hdr),
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", boundary);
    char model_part[512];
    int mplen = snprintf(model_part, sizeof(model_part),
        "\r\n--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s"
        "\r\n--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n%s"
        "\r\n--%s--\r\n",
        boundary, model, boundary, lang, boundary);

    size_t body_len = phlen + wav_len + mplen;
    uint8_t *body = heap_caps_malloc(body_len,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { free(wav); return ESP_ERR_NO_MEM; }

    memcpy(body,                  part_hdr, phlen);
    memcpy(body + phlen,          wav,      wav_len);
    memcpy(body + phlen + wav_len, model_part, mplen);
    free(wav);

    /* base_url follows this project's convention (matches llm_base_url): it
     * already includes the "/v1" prefix, so only the endpoint path is
     * appended here. */
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/transcriptions",
             cfg->base_url && cfg->base_url[0] ? cfg->base_url : "https://api.openai.com/v1");

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key ? cfg->api_key : "");

    char ct[96];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", boundary);

    /* esp_http_client_perform() completes the whole request/response cycle
     * (including closing a non-keep-alive connection) before returning, so
     * esp_http_client_read_response() afterward has nothing left to read —
     * that's why this used to silently come back empty despite a real 200.
     * Capture the body via the event handler instead (same pattern as
     * cap_voice_tts.c), which works regardless of chunked encoding or
     * whether a Content-Length header is even present. */
    whisper_buf_t buf = {
        .data = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .len  = 0,
        .cap  = 4096,
    };
    if (!buf.data) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .event_handler = on_data,
        .user_data = &buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", ct);
    esp_http_client_set_post_field(client, (char *)body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Whisper HTTP status=%d body_len=%u", status, (unsigned)buf.len);

    if (err == ESP_OK && status == 200 && buf.len > 0) {
        cJSON *j = cJSON_ParseWithLength((const char *)buf.data, buf.len);
        cJSON *text_j = j ? cJSON_GetObjectItem(j, "text") : NULL;
        if (text_j && cJSON_IsString(text_j)) {
            *out_text = strdup(text_j->valuestring);
            ESP_LOGI(TAG, "Transcript: %s", *out_text);
        }
        cJSON_Delete(j);
    } else if (buf.len > 0) {
        ESP_LOGW(TAG, "Whisper error body: %.*s", (int)buf.len, (const char *)buf.data);
    }

    esp_http_client_cleanup(client);
    free(buf.data);
    free(body);
    return (*out_text) ? ESP_OK : ESP_FAIL;
}
