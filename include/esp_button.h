#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of buttons that can be registered simultaneously. */
#define ESP_BUTTON_MAX_BUTTONS            16U
/** Polling interval in milliseconds used by the internal button task. */
#define ESP_BUTTON_DEFAULT_POLL_MS        10U
/** Default debounce window in milliseconds. */
#define ESP_BUTTON_DEFAULT_DEBOUNCE_MS    50U
/** Default duration in milliseconds before a long-click is detected. */
#define ESP_BUTTON_DEFAULT_LONG_MS        400U
/** Default window in milliseconds for detecting a double-click sequence. */
#define ESP_BUTTON_DEFAULT_DOUBLE_MS      300U

/** Opaque button handle returned by esp_button_create() and esp_button_register_gpio(). */
typedef struct esp_button esp_button_t;

/**
 * @brief Button event types emitted to the registered callback.
 *
 * CHANGED fires on every stable level transition (press or release).
 * TAP fires on a short press-and-release before the double-click window opens.
 * CLICK fires after the double-click window expires with exactly one tap.
 * LONG_DETECTED fires once the long-click threshold is crossed while held.
 * LONG_CLICK fires when the button is released after a long hold.
 */
typedef enum {
    ESP_BUTTON_EVENT_NONE = 0,       /**< No event. */
    ESP_BUTTON_EVENT_CHANGED,        /**< Stable level changed (press or release). */
    ESP_BUTTON_EVENT_PRESSED,        /**< Button transitioned to pressed state. */
    ESP_BUTTON_EVENT_RELEASED,       /**< Button transitioned to released state. */
    ESP_BUTTON_EVENT_TAP,            /**< Short press-and-release detected. */
    ESP_BUTTON_EVENT_CLICK,          /**< Single click confirmed after double-click window. */
    ESP_BUTTON_EVENT_DOUBLE_CLICK,   /**< Two taps within the double-click window. */
    ESP_BUTTON_EVENT_TRIPLE_CLICK,   /**< Three taps within the double-click window. */
    ESP_BUTTON_EVENT_LONG_DETECTED,  /**< Long-click threshold crossed while button is still held. */
    ESP_BUTTON_EVENT_LONG_CLICK,     /**< Button released after a long hold. */
} esp_button_event_t;

/**
 * @brief Button input source.
 */
typedef enum {
    ESP_BUTTON_MODE_GPIO = 0,  /**< Physical GPIO pin sampled by the internal task. */
    ESP_BUTTON_MODE_VIRTUAL,   /**< No GPIO — state driven via esp_button_trigger_event(). */
} esp_button_mode_t;

/**
 * @brief GPIO electrical configuration for the button pin.
 */
typedef enum {
    ESP_BUTTON_INPUT = 0,         /**< Input without internal pull resistor. */
    ESP_BUTTON_INPUT_PULLUP,      /**< Input with internal pull-up resistor. */
    ESP_BUTTON_INPUT_PULLDOWN,    /**< Input with internal pull-down resistor. */
    ESP_BUTTON_INPUT_FLOATING,    /**< Floating input (alias for INPUT). */
} esp_button_input_mode_t;

/**
 * @brief Custom GPIO read callback type.
 *
 * When set, the polling task calls this instead of reading GPIO directly.
 * Useful for buttons connected via I2C expanders or other peripherals.
 *
 * @param ctx         User context pointer provided at registration.
 * @param out_pressed Set to true if the button is currently pressed.
 * @return ESP_OK on success.
 */
typedef esp_err_t (*esp_button_read_cb_t)(void *ctx, bool *out_pressed);

/**
 * @brief Button event callback type.
 *
 * Invoked from the internal polling task. The internal mutex is NOT held
 * during the callback — it is safe to call esp_button_* getters from here.
 *
 * @param button    Handle of the button that generated the event.
 * @param event     Event type.
 * @param user_ctx  User context pointer registered with the callback.
 */
typedef void (*esp_button_event_cb_t)(esp_button_t *button, esp_button_event_t event, void *user_ctx);

/**
 * @brief Full configuration for a button instance.
 *
 * Zero-initialize and set only the fields you need; defaults apply for
 * timing fields left at zero.
 */
typedef struct {
    uint8_t pin;                        /**< GPIO pin number (ignored for VIRTUAL mode). */
    esp_button_mode_t mode;             /**< Input source: GPIO or virtual. */
    esp_button_input_mode_t input_mode; /**< Pull resistor configuration. */
    bool active_low;                    /**< True if pressed corresponds to logic LOW. */
    uint16_t debounce_ms;               /**< Debounce window in ms (0 = use default). */
    uint16_t long_click_ms;             /**< Long-click threshold in ms (0 = use default). */
    uint16_t double_click_ms;           /**< Double-click detection window in ms (0 = use default). */
    bool long_detect_retrigger;         /**< If true, LONG_DETECTED fires repeatedly while held. */
    esp_button_read_cb_t read_cb;       /**< Custom read callback (NULL = use GPIO). */
    void *read_ctx;                     /**< Context passed to read_cb. */
    esp_button_event_cb_t callback;     /**< Per-button event callback (NULL = use default). */
    void *user_ctx;                     /**< Context passed to callback. */
    int id;                             /**< Application-defined identifier for lookup by ID. */
} esp_button_config_t;

/**
 * @brief Initialize the button component.
 *
 * Creates the internal polling task and mutex. Must be called once before
 * any other esp_button_* function.
 *
 * @param log_enabled Enable internal component logs.
 * @param at_enabled  Register AT commands (AT+BTN, AT+BTN?).
 * @return ESP_OK, ESP_ERR_INVALID_STATE if already initialized.
 */
esp_err_t esp_button_init(bool log_enabled, bool at_enabled);

/**
 * @brief Deinitialize the component and release all resources.
 *
 * Stops the polling task, deletes all registered buttons, and frees the mutex.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t esp_button_deinit(void);

/**
 * @brief Report whether the component is initialized.
 */
bool esp_button_is_initialized(void);

/**
 * @brief Create a button from a full configuration struct.
 *
 * @param config     Button configuration. Must not be NULL.
 * @param out_button Receives the new handle on success. Must not be NULL.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if config is invalid,
 *         ESP_ERR_NO_MEM if the maximum button count is reached.
 */
esp_err_t esp_button_create(const esp_button_config_t *config, esp_button_t **out_button);

/**
 * @brief Delete a button and release its slot.
 *
 * @param button Handle returned by esp_button_create() or esp_button_register_gpio().
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL or not active.
 */
esp_err_t esp_button_delete(esp_button_t *button);

/**
 * @brief Convenience helper to register a GPIO button with sensible defaults.
 *
 * Equivalent to esp_button_create() with PULLUP, active_low=true, and
 * default timing values.
 *
 * @param pin        GPIO pin number.
 * @param id         Application-defined identifier.
 * @param out_button Receives the new handle on success. Must not be NULL.
 * @return ESP_OK on success.
 */
esp_err_t esp_button_register_gpio(uint8_t pin, int id, esp_button_t **out_button);

/**
 * @brief Set or replace the per-button event callback.
 *
 * @param button    Target button handle.
 * @param callback  Callback function (NULL to clear).
 * @param user_ctx  Context pointer passed to the callback.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_set_callback(esp_button_t *button, esp_button_event_cb_t callback, void *user_ctx);

/**
 * @brief Set the global fallback callback used by buttons with no per-button callback.
 *
 * @param callback  Callback function (NULL to clear).
 * @param user_ctx  Context pointer passed to the callback.
 * @return ESP_OK on success.
 */
esp_err_t esp_button_set_default_callback(esp_button_event_cb_t callback, void *user_ctx);

/**
 * @brief Override the debounce window for a specific button.
 *
 * @param button  Target button handle.
 * @param ms      Debounce duration in milliseconds.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_set_debounce_time(esp_button_t *button, uint16_t ms);

/**
 * @brief Override the long-click detection threshold for a specific button.
 *
 * @param button  Target button handle.
 * @param ms      Duration in milliseconds the button must be held.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_set_long_click_time(esp_button_t *button, uint16_t ms);

/**
 * @brief Override the double-click detection window for a specific button.
 *
 * @param button  Target button handle.
 * @param ms      Window in milliseconds within which taps must occur.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_set_double_click_time(esp_button_t *button, uint16_t ms);

/**
 * @brief Set a custom GPIO read callback for a button.
 *
 * Replaces direct GPIO sampling with a user-supplied function.
 * Pass NULL to revert to GPIO sampling.
 *
 * @param button    Target button handle.
 * @param read_cb   Read function.
 * @param read_ctx  Context pointer passed to read_cb.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_set_read_callback(esp_button_t *button, esp_button_read_cb_t read_cb, void *read_ctx);

/**
 * @brief Enable or disable LONG_DETECTED retrigger for a button.
 *
 * When enabled, ESP_BUTTON_EVENT_LONG_DETECTED fires repeatedly at each
 * long_click_ms interval while the button remains held.
 *
 * @param button  Target button handle.
 * @param enable  True to enable retrigger.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_set_long_detect_retrigger(esp_button_t *button, bool enable);

/**
 * @brief Manually advance the state machine for a single button (one polling step).
 *
 * Called automatically by the internal task. Useful for manual polling or tests.
 *
 * @param button  Target button handle.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_process(esp_button_t *button);

/**
 * @brief Manually advance the state machine for all registered buttons.
 *
 * @return ESP_OK on success.
 */
esp_err_t esp_button_process_all(void);

/**
 * @brief Inject a synthetic event into a button's state machine.
 *
 * Useful for virtual buttons and unit-testing event handlers.
 *
 * @param button  Target button handle.
 * @param event   Event to inject.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button is NULL.
 */
esp_err_t esp_button_trigger_event(esp_button_t *button, esp_button_event_t event);

/**
 * @brief Find a registered button by its application-defined ID.
 *
 * @param id  ID assigned in esp_button_config_t or esp_button_register_gpio().
 * @return Button handle, or NULL if not found.
 */
esp_button_t *esp_button_find_by_id(int id);

/**
 * @brief Find a registered button by its GPIO pin number.
 *
 * @param pin  GPIO pin number.
 * @return Button handle, or NULL if not found.
 */
esp_button_t *esp_button_find_by_pin(uint8_t pin);

/**
 * @brief Return the number of currently active (registered) buttons.
 */
size_t esp_button_count_active(void);

/**
 * @brief Return true if the button is currently in the stable pressed state.
 *
 * Non-blocking. Safe to call from any task.
 *
 * @param button  Target button handle.
 */
bool esp_button_is_pressed(const esp_button_t *button);

/**
 * @brief Read the stable pressed state with NULL-safety.
 *
 * @param button      Target button handle.
 * @param out_pressed Set to true if currently pressed.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if button or out_pressed is NULL.
 */
esp_err_t esp_button_get_state(const esp_button_t *button, bool *out_pressed);

/**
 * @brief Return true if the button was pressed at any point since the last poll.
 *
 * @param button  Target button handle.
 */
bool esp_button_was_pressed(const esp_button_t *button);

/**
 * @brief Return how long the button has been held in the current pressed state, in milliseconds.
 *
 * Returns 0 if the button is not currently pressed.
 *
 * @param button  Target button handle.
 */
uint32_t esp_button_was_pressed_for(const esp_button_t *button);

/**
 * @brief Return the accumulated click count since the last internal reset.
 *
 * @param button  Target button handle.
 */
uint8_t esp_button_get_click_count(const esp_button_t *button);

/**
 * @brief Return the accumulated long-click count since the last internal reset.
 *
 * @param button  Target button handle.
 */
uint8_t esp_button_get_long_click_count(const esp_button_t *button);

/**
 * @brief Return the most recent event emitted for this button.
 *
 * @param button  Target button handle.
 */
esp_button_event_t esp_button_get_last_event(const esp_button_t *button);

/**
 * @brief Return the GPIO pin number associated with this button.
 *
 * @param button  Target button handle.
 */
uint8_t esp_button_get_pin(const esp_button_t *button);

/**
 * @brief Return the application-defined ID of this button.
 *
 * @param button  Target button handle.
 */
int esp_button_get_id(const esp_button_t *button);

/**
 * @brief Convert an event enum value to a human-readable string.
 *
 * @param event  Event value.
 * @return Static string (e.g. "CLICK", "LONG_CLICK"). Never NULL.
 */
const char *esp_button_event_to_string(esp_button_event_t event);

#ifdef __cplusplus
}
#endif
