#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>
#include <stdbool.h>
#include <stdint.h>

typedef void (*button_callback_t)(void* user_ctx);

typedef struct {
    gpio_num_t gpio_num;
    button_handle_t button_handle;
    button_callback_t on_press_down;
    button_callback_t on_press_up;
    button_callback_t on_long_press;
    button_callback_t on_click;
    button_callback_t on_double_click;
    button_callback_t on_multiple_click;
    void* cb_ctx;
} Button;

bool Button_InitGpio(Button* button, gpio_num_t gpio_num, bool active_high, uint16_t long_press_time, uint16_t short_press_time, bool enable_power_save);
void Button_InitFromHandle(Button* button, button_handle_t button_handle);
void Button_Deinit(Button* button);
void Button_SetContext(Button* button, void* user_ctx);
void Button_OnPressDown(Button* button, button_callback_t callback);
void Button_OnPressUp(Button* button, button_callback_t callback);
void Button_OnLongPress(Button* button, button_callback_t callback);
void Button_OnClick(Button* button, button_callback_t callback);
void Button_OnDoubleClick(Button* button, button_callback_t callback);
void Button_OnMultipleClick(Button* button, button_callback_t callback, uint8_t click_count);

#endif
