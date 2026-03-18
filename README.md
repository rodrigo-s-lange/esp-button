# esp_button

Lightweight, non-blocking button handling for ESP-IDF.

## Features

- GPIO and virtual buttons
- Polling task with fixed slot table
- Debounce, click, double, triple, long and long-detected events
- Optional AT registration
- Optional internal logs for lifecycle, events and callback dispatch

## Init policy

`esp_button_init(log_enabled, at_enabled)` controls optional behaviors.

- `log_enabled`
  - enables component logs
- `at_enabled`
  - registers `AT+BTN` commands

## Basic example

```c
#include "esp_button.h"

static void on_button(esp_button_t *button, esp_button_event_t event, void *ctx)
{
    (void)button;
    (void)ctx;

    if (event == ESP_BUTTON_EVENT_CLICK) {
        // single click
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_button_init(false, false));

    esp_button_config_t cfg = {
        .pin = 0,
        .mode = ESP_BUTTON_MODE_GPIO,
        .input_mode = ESP_BUTTON_INPUT_PULLUP,
        .active_low = true,
        .callback = on_button,
    };

    esp_button_t *button = NULL;
    ESP_ERROR_CHECK(esp_button_create(&cfg, &button));
}
```

## Recommended flow

1. call `esp_button_init(log_enabled, at_enabled)`
2. optionally set a default callback with `esp_button_set_default_callback()`
3. register buttons through code or AT

## API

- `esp_button_init(log_enabled, at_enabled)`
- `esp_button_deinit()`
- `esp_button_create()`
- `esp_button_delete()`
- `esp_button_register_gpio()`
- `esp_button_set_callback()`
- `esp_button_set_default_callback()`
- `esp_button_set_debounce_time()`
- `esp_button_set_long_click_time()`
- `esp_button_set_double_click_time()`
- `esp_button_set_read_callback()`
- `esp_button_set_long_detect_retrigger()`
- `esp_button_process()`
- `esp_button_process_all()`
- `esp_button_trigger_event()`
- `esp_button_find_by_id()`
- `esp_button_find_by_pin()`

## AT commands

When `at_enabled=true`:
- `AT+BTN=<pin>,<id>`
- `AT+BTN?`
- `AT+BTN=<id>,TAP`

## Notes

- Public API is non-blocking.
- Keep callbacks short.
- For custom sources such as touch or I2C expanders, use `ESP_BUTTON_MODE_VIRTUAL`.
