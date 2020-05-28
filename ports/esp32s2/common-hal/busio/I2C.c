/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Scott Shawcroft
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "shared-bindings/busio/I2C.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#include "driver/i2c.h"

#include "esp_log.h"

#include "shared-bindings/microcontroller/__init__.h"
#include "supervisor/shared/translate.h"

// Number of times to try to send packet if failed.
#define ATTEMPTS 2

static const char* TAG = "CircuitPython I2C";

typedef enum {
    STATUS_FREE = 0,
    STATUS_IN_USE,
    STATUS_NEVER_RESET
} i2c_status_t;

static i2c_status_t i2c_status[I2C_NUM_MAX];

void never_reset_i2c(i2c_port_t num) {
    i2c_status[num] = STATUS_NEVER_RESET;
}

void i2c_reset(void) {
    for (i2c_port_t num = 0; num < I2C_NUM_MAX; num++) {
        if (i2c_status[num] == STATUS_IN_USE) {
            i2c_driver_delete(num);
            i2c_status[num] = STATUS_FREE;
        }
    }
}

void common_hal_busio_i2c_construct(busio_i2c_obj_t *self,
        const mcu_pin_obj_t* scl, const mcu_pin_obj_t* sda, uint32_t frequency, uint32_t timeout) {

    // Make sure scl and sda aren't input only

#if CIRCUITPY_REQUIRE_I2C_PULLUPS
    // // Test that the pins are in a high state. (Hopefully indicating they are pulled up.)
    // gpio_set_pin_function(sda->number, GPIO_PIN_FUNCTION_OFF);
    // gpio_set_pin_function(scl->number, GPIO_PIN_FUNCTION_OFF);
    // gpio_set_pin_direction(sda->number, GPIO_DIRECTION_IN);
    // gpio_set_pin_direction(scl->number, GPIO_DIRECTION_IN);

    // gpio_set_pin_pull_mode(sda->number, GPIO_PULL_DOWN);
    // gpio_set_pin_pull_mode(scl->number, GPIO_PULL_DOWN);

    // common_hal_mcu_delay_us(10);

    // gpio_set_pin_pull_mode(sda->number, GPIO_PULL_OFF);
    // gpio_set_pin_pull_mode(scl->number, GPIO_PULL_OFF);

    // // We must pull up within 3us to achieve 400khz.
    // common_hal_mcu_delay_us(3);

    // if (!gpio_get_pin_level(sda->number) || !gpio_get_pin_level(scl->number)) {
    //     reset_pin_number(sda->number);
    //     reset_pin_number(scl->number);
    //     mp_raise_RuntimeError(translate("SDA or SCL needs a pull up"));
    // }
#endif
    ESP_EARLY_LOGW(TAG, "create I2C");


    self->semaphore_handle = xSemaphoreCreateBinaryStatic(&self->semaphore);
    xSemaphoreGive(self->semaphore_handle);
    ESP_EARLY_LOGW(TAG, "new I2C lock %x", self->semaphore_handle);
    self->sda_pin = sda;
    self->scl_pin = scl;
    ESP_EARLY_LOGW(TAG, "scl %d sda %d", self->sda_pin->number, self->scl_pin->number);
    self->i2c_num = I2C_NUM_MAX;
    for (i2c_port_t num = 0; num < I2C_NUM_MAX; num++) {
        if (i2c_status[num] == STATUS_FREE) {
            self->i2c_num = num;
        }
    }
    if (self->i2c_num == I2C_NUM_MAX) {
        mp_raise_ValueError(translate("All I2C peripherals are in use"));
    }
    i2c_status[self->i2c_num] = STATUS_IN_USE;
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = self->sda_pin->number,
        .scl_io_num = self->scl_pin->number,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,  /*!< Internal GPIO pull mode for I2C sda signal*/
        .scl_pullup_en = GPIO_PULLUP_DISABLE,  /*!< Internal GPIO pull mode for I2C scl signal*/

        .master = {
            .clk_speed = frequency,
        }
    };
    // ESP_EARLY_LOGW(TAG, "param config %p %p %d %d", &i2c_conf, &(i2c_conf.mode), i2c_conf.mode, I2C_MODE_MAX);
    ESP_EARLY_LOGW(TAG, "param config %p %d %d", &i2c_conf, i2c_conf.mode, I2C_MODE_MAX);
    esp_err_t result = i2c_param_config(self->i2c_num, &i2c_conf);
    if (result != ESP_OK) {
        ESP_EARLY_LOGW(TAG, "error %d %p %d %d", result, &i2c_conf, (&i2c_conf)->mode, I2C_MODE_MAX);
        vTaskDelay(0);
        mp_raise_ValueError(translate("Invalid pins"));
    }
    ESP_EARLY_LOGW(TAG, "param config I2C done");
    result = i2c_driver_install(self->i2c_num,
                                I2C_MODE_MASTER,
                                0,
                                0,
                                0);
    if (result != ESP_OK) {
        mp_raise_OSError(MP_EIO);
    }

    claim_pin(sda);
    claim_pin(scl);
    ESP_EARLY_LOGW(TAG, "create I2C done");
}

bool common_hal_busio_i2c_deinited(busio_i2c_obj_t *self) {
    return self->sda_pin == NULL;
}

void common_hal_busio_i2c_deinit(busio_i2c_obj_t *self) {
    if (common_hal_busio_i2c_deinited(self)) {
        return;
    }

    i2c_driver_delete(self->i2c_num);
    i2c_status[self->i2c_num] = STATUS_FREE;

    reset_pin(self->sda_pin);
    reset_pin(self->scl_pin);
    self->sda_pin = NULL;
    self->scl_pin = NULL;
}

bool common_hal_busio_i2c_probe(busio_i2c_obj_t *self, uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, addr << 1, true);
    i2c_master_stop(cmd);
    esp_err_t result = i2c_master_cmd_begin(self->i2c_num, cmd, 100);
    i2c_cmd_link_delete(cmd);
    return result == ESP_OK;
}

bool common_hal_busio_i2c_try_lock(busio_i2c_obj_t *self) {
    ESP_EARLY_LOGW(TAG, "locking I2C %x", self->semaphore_handle);
    self->has_lock = xSemaphoreTake(self->semaphore_handle, 0) == pdTRUE;
    if (self->has_lock) {
        ESP_EARLY_LOGW(TAG, "lock grabbed");
    } else {
        ESP_EARLY_LOGW(TAG, "unable to grab lock");
    }
    return self->has_lock;
}

bool common_hal_busio_i2c_has_lock(busio_i2c_obj_t *self) {
    return self->has_lock;
}

void common_hal_busio_i2c_unlock(busio_i2c_obj_t *self) {
    ESP_EARLY_LOGW(TAG, "unlocking I2C");
    xSemaphoreGive(self->semaphore_handle);
    self->has_lock = false;
}

uint8_t common_hal_busio_i2c_write(busio_i2c_obj_t *self, uint16_t addr,
                                   const uint8_t *data, size_t len, bool transmit_stop_bit) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, addr << 1, true);
    i2c_master_write(cmd, (uint8_t*) data, len, true);
    if (transmit_stop_bit) {
        i2c_master_stop(cmd);
    }
    esp_err_t result = i2c_master_cmd_begin(self->i2c_num, cmd, 100 /* wait in ticks */);
    i2c_cmd_link_delete(cmd);

    if (result == ESP_OK) {
        return 0;
    } else if (result == ESP_FAIL) {
        return MP_ENODEV;
    }
    return MP_EIO;
}

uint8_t common_hal_busio_i2c_read(busio_i2c_obj_t *self, uint16_t addr,
        uint8_t *data, size_t len) {

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, addr << 1 | 1, true); // | 1 to indicate read
    i2c_master_read(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t result = i2c_master_cmd_begin(self->i2c_num, cmd, 100 /* wait in ticks */);
    i2c_cmd_link_delete(cmd);

    if (result == ESP_OK) {
        return 0;
    } else if (result == ESP_FAIL) {
        return MP_ENODEV;
    }
    return MP_EIO;
}

void common_hal_busio_i2c_never_reset(busio_i2c_obj_t *self) {
    never_reset_i2c(self->i2c_num);

    never_reset_pin(self->scl_pin);
    never_reset_pin(self->sda_pin);
}
