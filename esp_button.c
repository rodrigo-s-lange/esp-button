#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_at.h"
#include "esp_button.h"
#include "esp_gpio.h"

struct esp_button {
    bool used;
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

    bool raw_pressed;
    bool stable_pressed;
    bool long_detected;
    bool pressed_latched;
    uint8_t click_count;
    uint8_t long_click_count;
    uint32_t last_change_ms;
    uint32_t press_start_ms;
    uint32_t last_press_duration_ms;
    uint32_t click_deadline_ms;
    uint32_t last_long_report_ms;
    esp_button_event_t last_event;
};

static const char *TAG = "esp_button";

static esp_button_t s_buttons[ESP_BUTTON_MAX_BUTTONS];
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static bool s_initialized = false;
static bool s_at_registered = false;
static bool s_log_enabled = false;
static bool s_at_enabled = false;
static int s_next_id = 1;
static esp_button_event_cb_t s_default_callback = NULL;
static void *s_default_user_ctx = NULL;

#define BUTTON_LOGI(...) do { if (s_log_enabled) ESP_LOGI(TAG, __VA_ARGS__); } while (0)
#define BUTTON_LOGW(...) do { if (s_log_enabled) ESP_LOGW(TAG, __VA_ARGS__); } while (0)

typedef struct {
    esp_button_t *button;
    esp_button_event_t event;
    esp_button_event_cb_t callback;
    void *user_ctx;
} button_emit_ctx_t;

static esp_button_t *_find_button_by_id_locked(int id);
static esp_button_t *_find_button_by_pin_locked(uint8_t pin);

static uint32_t _now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t _check_ready(void)
{
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static char *_trim_ws(char *s)
{
    if (s == NULL) return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n > 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t' || s[n - 1U] == '\r' || s[n - 1U] == '\n')) {
        s[n - 1U] = '\0';
        n--;
    }
    return s;
}

static bool _eq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return false;
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - ('a' - 'A'));
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - ('a' - 'A'));
        if (ca != cb) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void _emit_event(button_emit_ctx_t *emit)
{
    if (emit == NULL || emit->button == NULL || emit->event == ESP_BUTTON_EVENT_NONE) return;
    BUTTON_LOGI("callback id=%d pin=%u event=%s",
                emit->button->id,
                (unsigned)emit->button->pin,
                esp_button_event_to_string(emit->event));
    if (emit->callback != NULL) {
        emit->callback(emit->button, emit->event, emit->user_ctx);
    }
}

static void _emit_event_locked(esp_button_t *button, esp_button_event_t event)
{
    button_emit_ctx_t emit = {0};

    if (button == NULL || event == ESP_BUTTON_EVENT_NONE) return;

    button->last_event = event;
    emit.button = button;
    emit.event = event;
    if (button->callback != NULL) {
        emit.callback = button->callback;
        emit.user_ctx = button->user_ctx;
    } else {
        emit.callback = s_default_callback;
        emit.user_ctx = s_default_user_ctx;
    }

    xSemaphoreGive(s_mutex);
    _emit_event(&emit);
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

const char *esp_button_event_to_string(esp_button_event_t event)
{
    switch (event) {
        case ESP_BUTTON_EVENT_CHANGED: return "CHANGED";
        case ESP_BUTTON_EVENT_PRESSED: return "PRESSED";
        case ESP_BUTTON_EVENT_RELEASED: return "RELEASED";
        case ESP_BUTTON_EVENT_TAP: return "TAP";
        case ESP_BUTTON_EVENT_CLICK: return "CLICK";
        case ESP_BUTTON_EVENT_DOUBLE_CLICK: return "DOUBLE";
        case ESP_BUTTON_EVENT_TRIPLE_CLICK: return "TRIPLE";
        case ESP_BUTTON_EVENT_LONG_DETECTED: return "LONG_DETECTED";
        case ESP_BUTTON_EVENT_LONG_CLICK: return "LONG_CLICK";
        default: return "NONE";
    }
}

static esp_err_t _validate_gpio_pin(uint8_t pin)
{
    if (pin >= GPIO_NUM_MAX) return ESP_ERR_INVALID_ARG;
    if (((SOC_GPIO_VALID_GPIO_MASK >> pin) & 0x1ULL) == 0ULL) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t _release_button_owner_by_pin(uint8_t pin)
{
    if (!s_initialized || s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_button_t *button = _find_button_by_pin_locked(pin);
    if (button != NULL) {
        memset(button, 0, sizeof(*button));
    }
    xSemaphoreGive(s_mutex);

    return esp_gpio_release_owner(pin, ESP_GPIO_OWNER_BUTTON);
}

static void _release_button_pin(const esp_button_t *button)
{
    if (button == NULL || !button->used || button->mode != ESP_BUTTON_MODE_GPIO) return;
    esp_err_t err = esp_gpio_release_owner(button->pin, ESP_GPIO_OWNER_BUTTON);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        BUTTON_LOGW("gpio %u release falhou: %s", (unsigned)button->pin, esp_err_to_name(err));
    }
}

static esp_err_t _configure_gpio_input(const esp_button_config_t *config)
{
    esp_err_t err = _validate_gpio_pin(config->pin);
    if (err != ESP_OK) return err;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << config->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    switch (config->input_mode) {
        case ESP_BUTTON_INPUT_PULLUP:
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            break;
        case ESP_BUTTON_INPUT_PULLDOWN:
            cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
            break;
        case ESP_BUTTON_INPUT:
        case ESP_BUTTON_INPUT_FLOATING:
        default:
            break;
    }

    return gpio_config(&cfg);
}

static esp_button_t *_find_button_by_id_locked(int id)
{
    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        if (s_buttons[i].used && s_buttons[i].id == id) {
            return &s_buttons[i];
        }
    }
    return NULL;
}

static esp_button_t *_find_button_by_pin_locked(uint8_t pin)
{
    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        if (s_buttons[i].used && s_buttons[i].pin == pin) {
            return &s_buttons[i];
        }
    }
    return NULL;
}

static esp_err_t _read_pressed(esp_button_t *button, bool *out_pressed)
{
    if (button == NULL || out_pressed == NULL) return ESP_ERR_INVALID_ARG;

    bool pressed = false;
    if (button->mode == ESP_BUTTON_MODE_GPIO) {
        int level = gpio_get_level((gpio_num_t)button->pin);
        pressed = button->active_low ? (level == 0) : (level != 0);
    } else {
        if (button->read_cb == NULL) return ESP_ERR_INVALID_STATE;
        esp_err_t err = button->read_cb(button->read_ctx, &pressed);
        if (err != ESP_OK) return err;
    }

    *out_pressed = pressed;
    return ESP_OK;
}

static void _finalize_click_sequence(esp_button_t *button)
{
    if (button == NULL || button->click_count == 0U) return;

    switch (button->click_count) {
        case 1:
            _emit_event_locked(button, ESP_BUTTON_EVENT_CLICK);
            break;
        case 2:
            _emit_event_locked(button, ESP_BUTTON_EVENT_DOUBLE_CLICK);
            break;
        default:
            _emit_event_locked(button, ESP_BUTTON_EVENT_TRIPLE_CLICK);
            break;
    }

    button->click_count = 0U;
    button->click_deadline_ms = 0U;
}

static void _process_button(esp_button_t *button, uint32_t now_ms)
{
    bool pressed = false;
    if (button == NULL || !button->used) return;
    if (_read_pressed(button, &pressed) != ESP_OK) return;

    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->last_change_ms = now_ms;
    }

    if ((now_ms - button->last_change_ms) < button->debounce_ms) {
        if (button->click_deadline_ms != 0U && !button->stable_pressed && now_ms >= button->click_deadline_ms) {
            _finalize_click_sequence(button);
        }
        return;
    }

    if (button->stable_pressed != button->raw_pressed) {
        button->stable_pressed = button->raw_pressed;
        button->pressed_latched = true;
        _emit_event_locked(button, ESP_BUTTON_EVENT_CHANGED);

        if (button->stable_pressed) {
            button->press_start_ms = now_ms;
            button->long_detected = false;
            button->last_long_report_ms = now_ms;
            _emit_event_locked(button, ESP_BUTTON_EVENT_PRESSED);
        } else {
            button->last_press_duration_ms = now_ms - button->press_start_ms;
            _emit_event_locked(button, ESP_BUTTON_EVENT_RELEASED);
            _emit_event_locked(button, ESP_BUTTON_EVENT_TAP);

            if (button->long_detected || button->last_press_duration_ms >= button->long_click_ms) {
                button->long_click_count++;
                _emit_event_locked(button, ESP_BUTTON_EVENT_LONG_CLICK);
                button->click_count = 0U;
                button->click_deadline_ms = 0U;
            } else {
                if (button->click_count < 3U) {
                    button->click_count++;
                }
                button->click_deadline_ms = now_ms + button->double_click_ms;
            }
        }
    }

    if (button->stable_pressed) {
        uint32_t held_ms = now_ms - button->press_start_ms;
        if (!button->long_detected && held_ms >= button->long_click_ms) {
            button->long_detected = true;
            button->long_click_count = 1U;
            button->last_long_report_ms = now_ms;
            _emit_event_locked(button, ESP_BUTTON_EVENT_LONG_DETECTED);
        } else if (button->long_detected && button->long_detect_retrigger &&
                   (now_ms - button->last_long_report_ms) >= button->long_click_ms) {
            button->long_click_count++;
            button->last_long_report_ms = now_ms;
            _emit_event_locked(button, ESP_BUTTON_EVENT_LONG_DETECTED);
        }
    } else if (button->click_deadline_ms != 0U && now_ms >= button->click_deadline_ms) {
        _finalize_click_sequence(button);
    }
}

static void _button_task(void *arg)
{
    (void)arg;

    while (1) {
        if (!s_initialized) {
            break;
        }

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            uint32_t now_ms = _now_ms();
            for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
                _process_button(&s_buttons[i], now_ms);
            }
            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(ESP_BUTTON_DEFAULT_POLL_MS));
    }

    if (s_task == xTaskGetCurrentTaskHandle()) {
        s_task = NULL;
    }
    vTaskDelete(NULL);
}

static esp_err_t _parse_btn_event(const char *text, esp_button_event_t *out_event)
{
    if (text == NULL || out_event == NULL) return ESP_ERR_INVALID_ARG;
    if (_eq_ci(text, "PRESSED")) { *out_event = ESP_BUTTON_EVENT_PRESSED; return ESP_OK; }
    if (_eq_ci(text, "RELEASED")) { *out_event = ESP_BUTTON_EVENT_RELEASED; return ESP_OK; }
    if (_eq_ci(text, "TAP")) { *out_event = ESP_BUTTON_EVENT_TAP; return ESP_OK; }
    if (_eq_ci(text, "CLICK")) { *out_event = ESP_BUTTON_EVENT_CLICK; return ESP_OK; }
    if (_eq_ci(text, "DOUBLE")) { *out_event = ESP_BUTTON_EVENT_DOUBLE_CLICK; return ESP_OK; }
    if (_eq_ci(text, "TRIPLE")) { *out_event = ESP_BUTTON_EVENT_TRIPLE_CLICK; return ESP_OK; }
    if (_eq_ci(text, "LONG") || _eq_ci(text, "LONG_CLICK")) { *out_event = ESP_BUTTON_EVENT_LONG_CLICK; return ESP_OK; }
    if (_eq_ci(text, "LONG_DETECTED")) { *out_event = ESP_BUTTON_EVENT_LONG_DETECTED; return ESP_OK; }
    return ESP_ERR_INVALID_ARG;
}

static void _at_btn_query(const char *param)
{
    (void)param;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        AT(R "ERROR: button query timeout");
        return;
    }

    size_t active = 0U;
    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        if (s_buttons[i].used) active++;
    }

    AT(C "Buttons ativos: " W "%u", (unsigned)active);
    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        if (!s_buttons[i].used) continue;
        AT(C "  BTN[%d] pin=%u state=" W "%s" C " event=" W "%s",
           s_buttons[i].id,
           (unsigned)s_buttons[i].pin,
           s_buttons[i].stable_pressed ? "TRUE" : "FALSE",
           esp_button_event_to_string(s_buttons[i].last_event));
    }
    xSemaphoreGive(s_mutex);
}

static void _at_btn_define_or_simulate(const char *param)
{
    char work[96];
    strncpy(work, param != NULL ? param : "", sizeof(work) - 1U);
    work[sizeof(work) - 1U] = '\0';

    char *a = _trim_ws(work);
    char *comma = strchr(a, ',');
    if (a[0] == '\0' || comma == NULL) {
        AT(Y "Uso: AT+BTN=<pin>,<id> ou AT+BTN=<id>,TAP");
        return;
    }

    *comma = '\0';
    char *b = _trim_ws(comma + 1);

    char *end = NULL;
    long first = strtol(a, &end, 10);
    if (end == a || *end != '\0' || first < 0 || first > 255) {
        AT(R "ERROR: argumento invalido");
        return;
    }

    long second_num = strtol(b, &end, 10);
    if (!(end == b || *end != '\0')) {
        esp_button_t *button = NULL;
        esp_err_t err = esp_button_register_gpio((uint8_t)first, (int)second_num, &button);
        if (err != ESP_OK || button == NULL) {
            AT(R "ERROR: btn define (%s)", esp_err_to_name(err));
            return;
        }
        AT(G "OK");
        return;
    }

    esp_button_event_t event = ESP_BUTTON_EVENT_NONE;
    if (_parse_btn_event(b, &event) != ESP_OK) {
        AT(R "ERROR: evento invalido");
        return;
    }

    esp_button_t *button = esp_button_find_by_id((int)first);
    if (button == NULL) {
        AT(R "ERROR: button nao encontrado");
        return;
    }

    esp_err_t err = esp_button_trigger_event(button, event);
    if (err != ESP_OK) {
        AT(R "ERROR: btn simulate (%s)", esp_err_to_name(err));
        return;
    }
    AT(G "OK");
}

static esp_err_t _register_at_commands(void)
{
    if (s_at_registered) return ESP_OK;

    esp_err_t err = esp_at_register_cmd_example("AT+BTN", _at_btn_define_or_simulate, "AT+BTN=23,1 ou AT+BTN=1,TAP");
    if (err != ESP_OK) return err;

    err = esp_at_register_cmd_example("AT+BTN?", _at_btn_query, "AT+BTN?");
    if (err != ESP_OK) return err;

    s_at_registered = true;
    return ESP_OK;
}

static void _unregister_at_commands(void)
{
    if (!s_at_registered) return;

    (void)esp_at_unregister_cmd("AT+BTN");
    (void)esp_at_unregister_cmd("AT+BTN?");
    s_at_registered = false;
}

esp_err_t esp_button_init(bool log_enabled, bool at_enabled)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    s_log_enabled = log_enabled;
    s_at_enabled = at_enabled;

    esp_err_t err = ESP_OK;
    if (s_at_enabled) {
        err = _register_at_commands();
        if (err != ESP_OK) {
            s_log_enabled = false;
            s_at_enabled = false;
            return err;
        }
    }

    err = esp_gpio_set_owner_release_handler(ESP_GPIO_OWNER_BUTTON, _release_button_owner_by_pin);
    if (err != ESP_OK) {
        s_log_enabled = false;
        s_at_enabled = false;
        return err;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        s_log_enabled = false;
        s_at_enabled = false;
        return ESP_ERR_NO_MEM;
    }

    memset(s_buttons, 0, sizeof(s_buttons));
    s_next_id = 1;

    s_initialized = true;

    BaseType_t ok = xTaskCreate(_button_task, "esp_button", 3072, NULL, 8, &s_task);
    if (ok != pdPASS) {
        s_initialized = false;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        (void)esp_gpio_set_owner_release_handler(ESP_GPIO_OWNER_BUTTON, NULL);
        s_log_enabled = false;
        s_at_enabled = false;
        return ESP_ERR_NO_MEM;
    }

    BUTTON_LOGI("initialized (AT=%s)", s_at_enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t esp_button_deinit(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    s_initialized = false;
    if (s_task != NULL) {
        TaskHandle_t task = s_task;
        for (int i = 0; i < 20 && s_task == task; i++) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (s_task == task) {
            s_task = NULL;
            vTaskDelete(task);
        }
    }
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    (void)esp_gpio_set_owner_release_handler(ESP_GPIO_OWNER_BUTTON, NULL);

    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        _release_button_pin(&s_buttons[i]);
    }

    BUTTON_LOGI("deinitialized");
    _unregister_at_commands();
    memset(s_buttons, 0, sizeof(s_buttons));
    s_default_callback = NULL;
    s_default_user_ctx = NULL;
    s_next_id = 1;
    s_log_enabled = false;
    s_at_enabled = false;
    return ESP_OK;
}

bool esp_button_is_initialized(void)
{
    return s_initialized;
}

esp_err_t esp_button_create(const esp_button_config_t *config, esp_button_t **out_button)
{
    if (config == NULL || out_button == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = _check_ready();
    if (err != ESP_OK) return err;

    if (config->mode == ESP_BUTTON_MODE_GPIO) {
        err = esp_gpio_claim_owner(config->pin, ESP_GPIO_OWNER_BUTTON);
        if (err != ESP_OK) return err;
        err = _configure_gpio_input(config);
        if (err != ESP_OK) {
            (void)esp_gpio_release_owner(config->pin, ESP_GPIO_OWNER_BUTTON);
            return err;
        }
    } else if (config->read_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        if (config->mode == ESP_BUTTON_MODE_GPIO) {
            (void)esp_gpio_release_owner(config->pin, ESP_GPIO_OWNER_BUTTON);
        }
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        if (s_buttons[i].used) continue;

        esp_button_t *button = &s_buttons[i];
        memset(button, 0, sizeof(*button));
        button->used = true;
        button->pin = config->pin;
        button->mode = config->mode;
        button->input_mode = config->input_mode;
        button->active_low = config->active_low;
        button->debounce_ms = (config->debounce_ms != 0U) ? config->debounce_ms : ESP_BUTTON_DEFAULT_DEBOUNCE_MS;
        button->long_click_ms = (config->long_click_ms != 0U) ? config->long_click_ms : ESP_BUTTON_DEFAULT_LONG_MS;
        button->double_click_ms = (config->double_click_ms != 0U) ? config->double_click_ms : ESP_BUTTON_DEFAULT_DOUBLE_MS;
        button->long_detect_retrigger = config->long_detect_retrigger;
        button->read_cb = config->read_cb;
        button->read_ctx = config->read_ctx;
        button->callback = config->callback;
        button->user_ctx = config->user_ctx;
        button->id = (config->id != 0) ? config->id : s_next_id++;
        button->last_event = ESP_BUTTON_EVENT_NONE;
        button->last_change_ms = _now_ms();
        _read_pressed(button, &button->raw_pressed);
        button->stable_pressed = button->raw_pressed;

        *out_button = button;
        BUTTON_LOGI("created id=%d pin=%u mode=%d", button->id, (unsigned)button->pin, (int)button->mode);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_button_register_gpio(uint8_t pin, int id, esp_button_t **out_button)
{
    esp_err_t err = _check_ready();
    if (err != ESP_OK) return err;
    if (out_button == NULL) return ESP_ERR_INVALID_ARG;

    esp_button_config_t cfg = {
        .pin = pin,
        .mode = ESP_BUTTON_MODE_GPIO,
        .input_mode = ESP_BUTTON_INPUT_PULLUP,
        .active_low = true,
        .id = id,
    };
    *out_button = NULL;

    err = esp_gpio_claim_owner(pin, ESP_GPIO_OWNER_BUTTON);
    if (err != ESP_OK) return err;

    err = _configure_gpio_input(&cfg);
    if (err != ESP_OK) {
        (void)esp_gpio_release_owner(pin, ESP_GPIO_OWNER_BUTTON);
        return err;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        (void)esp_gpio_release_owner(pin, ESP_GPIO_OWNER_BUTTON);
        return ESP_ERR_TIMEOUT;
    }

    esp_button_t *existing = _find_button_by_id_locked(id);
    if (existing != NULL) {
        if (existing->pin != pin) {
            _release_button_pin(existing);
        }
        memset(existing, 0, sizeof(*existing));
    }

    existing = _find_button_by_pin_locked(pin);
    if (existing != NULL) {
        memset(existing, 0, sizeof(*existing));
    }

    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        if (s_buttons[i].used) continue;

        esp_button_t *button = &s_buttons[i];
        memset(button, 0, sizeof(*button));
        button->used = true;
        button->pin = cfg.pin;
        button->mode = cfg.mode;
        button->input_mode = cfg.input_mode;
        button->active_low = cfg.active_low;
        button->debounce_ms = ESP_BUTTON_DEFAULT_DEBOUNCE_MS;
        button->long_click_ms = ESP_BUTTON_DEFAULT_LONG_MS;
        button->double_click_ms = ESP_BUTTON_DEFAULT_DOUBLE_MS;
        button->id = cfg.id;
        button->last_event = ESP_BUTTON_EVENT_NONE;
        button->last_change_ms = _now_ms();
        _read_pressed(button, &button->raw_pressed);
        button->stable_pressed = button->raw_pressed;
        *out_button = button;
        BUTTON_LOGI("registered gpio id=%d pin=%u", button->id, (unsigned)button->pin);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_button_delete(esp_button_t *button)
{
    esp_err_t err = _check_ready();
    if (err != ESP_OK) return err;
    if (button == NULL) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    BUTTON_LOGI("deleted id=%d pin=%u", button->id, (unsigned)button->pin);
    _release_button_pin(button);
    memset(button, 0, sizeof(*button));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_set_callback(esp_button_t *button, esp_button_event_cb_t callback, void *user_ctx)
{
    if (button == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    button->callback = callback;
    button->user_ctx = user_ctx;
    BUTTON_LOGI("set callback id=%d pin=%u", button->id, (unsigned)button->pin);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_set_default_callback(esp_button_event_cb_t callback, void *user_ctx)
{
    if (!s_initialized || s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_default_callback = callback;
    s_default_user_ctx = user_ctx;
    BUTTON_LOGI("set default callback");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_set_debounce_time(esp_button_t *button, uint16_t ms)
{
    if (button == NULL || ms == 0U) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    button->debounce_ms = ms;
    BUTTON_LOGI("set debounce id=%d pin=%u ms=%u", button->id, (unsigned)button->pin, (unsigned)ms);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_set_long_click_time(esp_button_t *button, uint16_t ms)
{
    if (button == NULL || ms == 0U) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    button->long_click_ms = ms;
    BUTTON_LOGI("set long_click id=%d pin=%u ms=%u", button->id, (unsigned)button->pin, (unsigned)ms);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_set_double_click_time(esp_button_t *button, uint16_t ms)
{
    if (button == NULL || ms == 0U) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    button->double_click_ms = ms;
    BUTTON_LOGI("set double_click id=%d pin=%u ms=%u", button->id, (unsigned)button->pin, (unsigned)ms);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_set_read_callback(esp_button_t *button, esp_button_read_cb_t read_cb, void *read_ctx)
{
    if (button == NULL || read_cb == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    button->mode = ESP_BUTTON_MODE_VIRTUAL;
    button->read_cb = read_cb;
    button->read_ctx = read_ctx;
    BUTTON_LOGI("set read callback id=%d pin=%u", button->id, (unsigned)button->pin);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_set_long_detect_retrigger(esp_button_t *button, bool enable)
{
    if (button == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    button->long_detect_retrigger = enable;
    BUTTON_LOGI("set long retrigger id=%d pin=%u enable=%s", button->id, (unsigned)button->pin, enable ? "true" : "false");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_process(esp_button_t *button)
{
    esp_err_t err = _check_ready();
    if (err != ESP_OK) return err;
    if (button == NULL) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    _process_button(button, _now_ms());
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_process_all(void)
{
    esp_err_t err = _check_ready();
    if (err != ESP_OK) return err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    uint32_t now_ms = _now_ms();
    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        _process_button(&s_buttons[i], now_ms);
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t esp_button_trigger_event(esp_button_t *button, esp_button_event_t event)
{
    esp_err_t err = _check_ready();
    if (err != ESP_OK) return err;
    if (button == NULL || event == ESP_BUTTON_EVENT_NONE) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    switch (event) {
        case ESP_BUTTON_EVENT_PRESSED:
            button->stable_pressed = true;
            button->raw_pressed = true;
            button->press_start_ms = _now_ms();
            break;
        case ESP_BUTTON_EVENT_RELEASED:
            button->stable_pressed = false;
            button->raw_pressed = false;
            break;
        default:
            break;
    }
    BUTTON_LOGI("trigger event id=%d pin=%u event=%s",
                button->id,
                (unsigned)button->pin,
                esp_button_event_to_string(event));
    _emit_event_locked(button, event);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_button_t *esp_button_find_by_id(int id)
{
    if (!s_initialized || s_mutex == NULL) return NULL;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return NULL;
    esp_button_t *button = _find_button_by_id_locked(id);
    xSemaphoreGive(s_mutex);
    return button;
}

esp_button_t *esp_button_find_by_pin(uint8_t pin)
{
    if (!s_initialized || s_mutex == NULL) return NULL;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return NULL;
    esp_button_t *button = _find_button_by_pin_locked(pin);
    xSemaphoreGive(s_mutex);
    return button;
}

size_t esp_button_count_active(void)
{
    size_t count = 0U;
    if (!s_initialized || s_mutex == NULL) return 0U;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return 0U;
    for (size_t i = 0; i < ESP_BUTTON_MAX_BUTTONS; i++) {
        if (s_buttons[i].used) count++;
    }
    xSemaphoreGive(s_mutex);
    return count;
}

bool esp_button_is_pressed(const esp_button_t *button)
{
    return (button != NULL) ? button->stable_pressed : false;
}

esp_err_t esp_button_get_state(const esp_button_t *button, bool *out_pressed)
{
    if (button == NULL || out_pressed == NULL) return ESP_ERR_INVALID_ARG;
    *out_pressed = button->stable_pressed;
    return ESP_OK;
}

bool esp_button_was_pressed(const esp_button_t *button)
{
    return (button != NULL) ? button->pressed_latched : false;
}

uint32_t esp_button_was_pressed_for(const esp_button_t *button)
{
    return (button != NULL) ? button->last_press_duration_ms : 0U;
}

uint8_t esp_button_get_click_count(const esp_button_t *button)
{
    return (button != NULL) ? button->click_count : 0U;
}

uint8_t esp_button_get_long_click_count(const esp_button_t *button)
{
    return (button != NULL) ? button->long_click_count : 0U;
}

esp_button_event_t esp_button_get_last_event(const esp_button_t *button)
{
    return (button != NULL) ? button->last_event : ESP_BUTTON_EVENT_NONE;
}

uint8_t esp_button_get_pin(const esp_button_t *button)
{
    return (button != NULL) ? button->pin : 0U;
}

int esp_button_get_id(const esp_button_t *button)
{
    return (button != NULL) ? button->id : 0;
}
