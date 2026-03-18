# Examples

## GPIO button with pull-up

```c
#include "esp_button.h"

static void on_button(esp_button_t *button, esp_button_event_t event, void *ctx)
{
    (void)button;
    (void)ctx;

    if (event == ESP_BUTTON_EVENT_CLICK) {
        // single click
    } else if (event == ESP_BUTTON_EVENT_DOUBLE_CLICK) {
        // double click
    } else if (event == ESP_BUTTON_EVENT_LONG_CLICK) {
        // released after long press
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_button_init(true, false));
    ESP_ERROR_CHECK(esp_button_set_default_callback(on_button, NULL));

    esp_button_t *button = NULL;
    ESP_ERROR_CHECK(esp_button_register_gpio(0, 1, &button));
}
```

## Virtual button

```c
static bool s_cached_pressed = false;

static esp_err_t read_virtual_button(void *ctx, bool *out_pressed)
{
    (void)ctx;
    *out_pressed = s_cached_pressed;
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_button_init(false, false));

    esp_button_config_t cfg = {
        .mode = ESP_BUTTON_MODE_VIRTUAL,
        .read_cb = read_virtual_button,
    };

    esp_button_t *button = NULL;
    ESP_ERROR_CHECK(esp_button_create(&cfg, &button));
}
```

## AT-driven setup

```c
ESP_ERROR_CHECK(esp_button_init(false, true));
```

```text
AT+BTN=23,1
AT+BTN?
AT+BTN=1,TAP
```
