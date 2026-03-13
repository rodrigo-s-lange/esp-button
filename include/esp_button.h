#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_BUTTON_MAX_BUTTONS            16U
#define ESP_BUTTON_DEFAULT_POLL_MS        10U
#define ESP_BUTTON_DEFAULT_DEBOUNCE_MS    50U
#define ESP_BUTTON_DEFAULT_LONG_MS        400U
#define ESP_BUTTON_DEFAULT_DOUBLE_MS      300U

typedef struct esp_button esp_button_t;

typedef enum {
    ESP_BUTTON_EVENT_NONE = 0,
    ESP_BUTTON_EVENT_CHANGED,
    ESP_BUTTON_EVENT_PRESSED,
    ESP_BUTTON_EVENT_RELEASED,
    ESP_BUTTON_EVENT_TAP,
    ESP_BUTTON_EVENT_CLICK,
    ESP_BUTTON_EVENT_DOUBLE_CLICK,
    ESP_BUTTON_EVENT_TRIPLE_CLICK,
    ESP_BUTTON_EVENT_LONG_DETECTED,
    ESP_BUTTON_EVENT_LONG_CLICK,
} esp_button_event_t;

typedef enum {
    ESP_BUTTON_MODE_GPIO = 0,
    ESP_BUTTON_MODE_VIRTUAL,
} esp_button_mode_t;

typedef enum {
    ESP_BUTTON_INPUT = 0,
    ESP_BUTTON_INPUT_PULLUP,
    ESP_BUTTON_INPUT_PULLDOWN,
    ESP_BUTTON_INPUT_FLOATING,
} esp_button_input_mode_t;

typedef esp_err_t (*esp_button_read_cb_t)(void *ctx, bool *out_pressed);
typedef void (*esp_button_event_cb_t)(esp_button_t *button, esp_button_event_t event, void *user_ctx);

typedef struct {
    uint8_t pin;
    esp_button_mode_t mode;
    esp_button_input_mode_t input_mode;
    bool active_low;
    uint16_t debounce_ms;
    uint16_t long_click_ms;
    uint16_t double_click_ms;
    bool long_detect_retrigger;
    esp_button_read_cb_t read_cb;
    void *read_ctx;
    esp_button_event_cb_t callback;
    void *user_ctx;
    int id;
} esp_button_config_t;

esp_err_t esp_button_init(void);
esp_err_t esp_button_deinit(void);
bool esp_button_is_initialized(void);

esp_err_t esp_button_create(const esp_button_config_t *config, esp_button_t **out_button);
esp_err_t esp_button_delete(esp_button_t *button);
esp_err_t esp_button_register_gpio(uint8_t pin, int id, esp_button_t **out_button);

esp_err_t esp_button_set_callback(esp_button_t *button, esp_button_event_cb_t callback, void *user_ctx);
esp_err_t esp_button_set_default_callback(esp_button_event_cb_t callback, void *user_ctx);
esp_err_t esp_button_set_debounce_time(esp_button_t *button, uint16_t ms);
esp_err_t esp_button_set_long_click_time(esp_button_t *button, uint16_t ms);
esp_err_t esp_button_set_double_click_time(esp_button_t *button, uint16_t ms);
esp_err_t esp_button_set_read_callback(esp_button_t *button, esp_button_read_cb_t read_cb, void *read_ctx);
esp_err_t esp_button_set_long_detect_retrigger(esp_button_t *button, bool enable);

esp_err_t esp_button_process(esp_button_t *button);
esp_err_t esp_button_process_all(void);
esp_err_t esp_button_trigger_event(esp_button_t *button, esp_button_event_t event);

esp_button_t *esp_button_find_by_id(int id);
esp_button_t *esp_button_find_by_pin(uint8_t pin);
size_t esp_button_count_active(void);
bool esp_button_is_pressed(const esp_button_t *button);
esp_err_t esp_button_get_state(const esp_button_t *button, bool *out_pressed);
bool esp_button_was_pressed(const esp_button_t *button);
uint32_t esp_button_was_pressed_for(const esp_button_t *button);
uint8_t esp_button_get_click_count(const esp_button_t *button);
uint8_t esp_button_get_long_click_count(const esp_button_t *button);
esp_button_event_t esp_button_get_last_event(const esp_button_t *button);
uint8_t esp_button_get_pin(const esp_button_t *button);
int esp_button_get_id(const esp_button_t *button);
const char *esp_button_event_to_string(esp_button_event_t event);

#ifdef __cplusplus
}
#endif
