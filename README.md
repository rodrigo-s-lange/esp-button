# esp_button

Lightweight, non-blocking button handling for ESP-IDF.

Inspired by `Button2`, but adapted to:

- native ESP-IDF GPIO driver
- fixed slots without `malloc`
- internal low-overhead polling task
- callback-driven event model
- optional virtual/custom button readers
- optional AT commands:
  - `AT+BTN=<pin>,<id>`
  - `AT+BTN?`
  - `AT+BTN=<id>,TAP`

## Features

- debouncing
- `pressed` / `released` / `changed`
- `tap`
- `click`, `double click`, `triple click`
- `long detected`
- `long click`
- retriggerable long-detect mode
- GPIO buttons
- virtual buttons via custom read callback

## Design

This component uses:

- fixed slot table: `ESP_BUTTON_MAX_BUTTONS`
- one internal task polling buttons every `ESP_BUTTON_DEFAULT_POLL_MS`
- direct `gpio_get_level()` for GPIO-backed buttons

No blocking waits are used in the public API.

## Basic example

```c
#include "esp_button.h"

static void on_button(esp_button_t *button, esp_button_event_t event, void *ctx)
{
    (void)ctx;
    if (event == ESP_BUTTON_EVENT_CLICK) {
        // single click
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_button_init());

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

## Project integration style

The component does not bind a physical button automatically.

Recommended flow:

1. call `esp_button_init()`
2. set a default callback with `esp_button_set_default_callback()`
3. register buttons through:
   - code: `esp_button_register_gpio(pin, id, &button)`
   - AT: `AT+BTN=<pin>,<id>`

This keeps the firmware generic and avoids hardcoding board-specific pins.

## API

- `esp_button_init()`
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

## Defaults

- poll: `10 ms`
- debounce: `50 ms`
- long click: `400 ms`
- double click window: `300 ms`

## Notes

- For minimal latency, keep callbacks short.
- For custom sources such as touch or I2C expanders, use `ESP_BUTTON_MODE_VIRTUAL` with `read_cb`.
- If you need many I2C-backed buttons, cache the port read externally and let each virtual button read from the cached state.

## AT commands

- `AT+BTN=23,1`
  - register GPIO 23 as button ID 1
- `AT+BTN?`
  - list active buttons and current state
- `AT+BTN=1,TAP`
  - simulate event on button ID 1

Supported simulated events:

- `PRESSED`
- `RELEASED`
- `TAP`
- `CLICK`
- `DOUBLE`
- `TRIPLE`
- `LONG`
- `LONG_DETECTED`

## Repository

https://github.com/rodrigo-s-lange/esp-button
