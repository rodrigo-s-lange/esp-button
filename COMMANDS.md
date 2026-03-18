# esp_button - Commands

When `esp_button_init(log_enabled, true)` is used, the component registers these AT commands.

## Register or simulate

`AT+BTN=<pin>,<id>`
- registers a GPIO button using default settings:
  - input pull-up
  - active-low
  - default timings

`AT+BTN=<id>,<event>`
- simulates an event on an existing button

Supported simulated events:
- `PRESSED`
- `RELEASED`
- `TAP`
- `CLICK`
- `DOUBLE`
- `TRIPLE`
- `LONG`
- `LONG_DETECTED`

## Query

`AT+BTN?`
- lists active buttons, current pressed state and last event

## Examples

```text
AT+BTN=23,1
AT+BTN?
AT+BTN=1,TAP
AT+BTN=1,DOUBLE
AT+BTN=1,LONG_DETECTED
```
