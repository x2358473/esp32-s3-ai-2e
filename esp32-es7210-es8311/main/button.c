#include "button.h"
#include <string.h>

static void button_press_down_cb(void* handle, void* usr_data) {
    Button* button = (Button*)usr_data;
    (void)handle;
    if (button->on_press_down != NULL) {
        button->on_press_down(button->cb_ctx);
    }
}

static void button_press_up_cb(void* handle, void* usr_data) {
    Button* button = (Button*)usr_data;
    (void)handle;
    if (button->on_press_up != NULL) {
        button->on_press_up(button->cb_ctx);
    }
}

static void button_long_press_cb(void* handle, void* usr_data) {
    Button* button = (Button*)usr_data;
    (void)handle;
    if (button->on_long_press != NULL) {
        button->on_long_press(button->cb_ctx);
    }
}

static void button_click_cb(void* handle, void* usr_data) {
    Button* button = (Button*)usr_data;
    (void)handle;
    if (button->on_click != NULL) {
        button->on_click(button->cb_ctx);
    }
}

static void button_double_click_cb(void* handle, void* usr_data) {
    Button* button = (Button*)usr_data;
    (void)handle;
    if (button->on_double_click != NULL) {
        button->on_double_click(button->cb_ctx);
    }
}

static void button_multiple_click_cb(void* handle, void* usr_data) {
    Button* button = (Button*)usr_data;
    (void)handle;
    if (button->on_multiple_click != NULL) {
        button->on_multiple_click(button->cb_ctx);
    }
}

void Button_InitFromHandle(Button* button, button_handle_t button_handle) {
    memset(button, 0, sizeof(Button));
    button->button_handle = button_handle;
    button->gpio_num = GPIO_NUM_NC;
}

bool Button_InitGpio(Button* button, gpio_num_t gpio_num, bool active_high, uint16_t long_press_time, uint16_t short_press_time, bool enable_power_save) {
    button_config_t button_config = {0};
    button_gpio_config_t gpio_config = {0};
    if (button == NULL) {
        return false;
    }
    memset(button, 0, sizeof(Button));
    button->gpio_num = gpio_num;
    if (gpio_num == GPIO_NUM_NC) {
        return false;
    }
    button_config.long_press_time = long_press_time;
    button_config.short_press_time = short_press_time;
    gpio_config.gpio_num = gpio_num;
    gpio_config.active_level = active_high ? 1 : 0;
    gpio_config.enable_power_save = enable_power_save;
    gpio_config.disable_pull = false;
    return iot_button_new_gpio_device(&button_config, &gpio_config, &button->button_handle) == ESP_OK;
}

void Button_Deinit(Button* button) {
    if (button != NULL && button->button_handle != NULL) {
        iot_button_delete(button->button_handle);
        button->button_handle = NULL;
    }
}

void Button_SetContext(Button* button, void* user_ctx) {
    if (button != NULL) {
        button->cb_ctx = user_ctx;
    }
}

void Button_OnPressDown(Button* button, button_callback_t callback) {
    if (button == NULL || button->button_handle == NULL) {
        return;
    }
    button->on_press_down = callback;
    iot_button_register_cb(button->button_handle, BUTTON_PRESS_DOWN, NULL, button_press_down_cb, button);
}

void Button_OnPressUp(Button* button, button_callback_t callback) {
    if (button == NULL || button->button_handle == NULL) {
        return;
    }
    button->on_press_up = callback;
    iot_button_register_cb(button->button_handle, BUTTON_PRESS_UP, NULL, button_press_up_cb, button);
}

void Button_OnLongPress(Button* button, button_callback_t callback) {
    if (button == NULL || button->button_handle == NULL) {
        return;
    }
    button->on_long_press = callback;
    iot_button_register_cb(button->button_handle, BUTTON_LONG_PRESS_START, NULL, button_long_press_cb, button);
}

void Button_OnClick(Button* button, button_callback_t callback) {
    if (button == NULL || button->button_handle == NULL) {
        return;
    }
    button->on_click = callback;
    iot_button_register_cb(button->button_handle, BUTTON_SINGLE_CLICK, NULL, button_click_cb, button);
}

void Button_OnDoubleClick(Button* button, button_callback_t callback) {
    if (button == NULL || button->button_handle == NULL) {
        return;
    }
    button->on_double_click = callback;
    iot_button_register_cb(button->button_handle, BUTTON_DOUBLE_CLICK, NULL, button_double_click_cb, button);
}

void Button_OnMultipleClick(Button* button, button_callback_t callback, uint8_t click_count) {
    button_event_args_t event_args = {0};
    if (button == NULL || button->button_handle == NULL) {
        return;
    }
    button->on_multiple_click = callback;
    event_args.multiple_clicks.clicks = click_count;
    iot_button_register_cb(button->button_handle, BUTTON_MULTIPLE_CLICK, &event_args, button_multiple_click_cb, button);
}
