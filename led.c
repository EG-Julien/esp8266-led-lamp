#include <stdio.h>
#include <stdlib.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <math.h>
#include <multipwm.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include "wifi.h"

#define H_CUTOFF 1.047196667
#define H_CUTOFF2 H_CUTOFF*2
#define H_CUTOFF4 H_CUTOFF2*2
#define FADE_SPEED 15
#define INITIAL_BRIGHTNESS 100
#define INITIAL_ON true

struct Color {
    float r;
    float g;
    float b;
};

const char* animateLightPCName = "animate_task";

// Struct used to control the pwm
pwm_info_t pwm_info;
// Task handle used to suspend and resume the animate task (doesn't need to be always running!)
TaskHandle_t animateTH;
// Boolean flag that can be used to gracefully stop the animate task

bool shouldQuitAnimationTask = false;
float led_brightness = 0;
float target_brightness = INITIAL_BRIGHTNESS;
bool led_on = INITIAL_ON;

static void hsi2rgb(float h, float s, float i, struct Color* rgb) {
    // Make sure h is between 0 and 360
    while (h < 0) { h += 360.0f; };
    while (h >= 360) { h -= 360.0f; };

    // Convert h to radians and others from percentage to ratio
    h = M_PI * h / 180.0f;
    s /= 100.0f;
    i /= 100.0f;

    // Clmap s and i
    s = s > 0 ? (s < 1 ? s : 1) : 0;
    i = i > 0 ? (i < 1 ? i : 1) : 0;

    // Shape intensity for higher resolution near 0
    i = i * sqrt(i);

    // Transform depending on h
    if (h < H_CUTOFF2) {
        rgb->r = i / 3 * (1 + s * cos(h) / cos(H_CUTOFF - h));
        rgb->g = i / 3 * (1 + s * (1 - cos(h) / cos(H_CUTOFF - h)));
        rgb->b = i / 3 * (1 - s);
    } else if (h < H_CUTOFF4) {
        h = h - H_CUTOFF2;
        rgb->r = i / 3 * (1 - s);
        rgb->g = i / 3 * (1 + s * cos(h) / cos(H_CUTOFF - h));
        rgb->b = i / 3 * (1 + s * (1 - cos(h) / cos(H_CUTOFF - h)));
    } else {
        h = h - H_CUTOFF4;
        rgb->r = i / 3 * (1 + s * (1 - cos(h) / cos(H_CUTOFF - h)));
        rgb->g = i / 3 * (1 - s);
        rgb->b = i / 3 * (1 + s * cos(h) / cos(H_CUTOFF - h));
    }
}

static void wifi_init() {
    struct sdk_station_config wifi_config = {
        .ssid = "Bénéhouse",
        .password = "Cafsouris220",
    };

    sdk_wifi_set_opmode(STATION_MODE);
    sdk_wifi_station_set_config(&wifi_config);
    sdk_wifi_station_connect();
}

const int led_gpio = 14;

void led_write(struct Color rgb) {
    uint32_t p;
    p = rgb.r * UINT16_MAX;
    printf("duty : %u\n", p);
    multipwm_stop(&pwm_info);
    multipwm_set_duty(&pwm_info, 0, p);
    multipwm_start(&pwm_info);
}


float step(float num, float target, float stepWidth) {
    if (num < target) {
        if (num < target - stepWidth) return num + stepWidth;
	return target;
    } else if (num > target) {
        if (num > target + stepWidth) return num - stepWidth;
	return target;
    } 

    return target;
}

void animate_light_transition_task(void* pvParameters) {
    float t_b;
    struct Color rgb;

    while (!shouldQuitAnimationTask) {
        // Compute brightness target value
        if (led_on) 
            t_b = target_brightness;
        else 
            t_b = 0;

        // Do the transition
        while (!(t_b == led_brightness)) {

            // Update led values according to target
            led_brightness = step(led_brightness, t_b, 1.0);

            hsi2rgb(0.00, 100.00, led_brightness, &rgb);
            led_write(rgb);

            // Only do this at most 60 times per second
            vTaskDelay(FADE_SPEED / portTICK_PERIOD_MS);	
        }
        
        vTaskSuspend(animateTH);
    }


    vTaskDelete(NULL);
}

void led_init() {
    pwm_info.channels = 3;
    multipwm_init(&pwm_info);
    multipwm_set_pin(&pwm_info, 0, led_gpio);
}

void led_identify_task(void *_args) {
    vTaskDelete(NULL);
}

void led_identify(homekit_value_t _value) {
    printf("LED identify\n");
    xTaskCreate(led_identify_task, "LED identify", 128, NULL, 2, NULL);
}

homekit_value_t led_on_get() {
    return HOMEKIT_BOOL(led_on);
}

void led_on_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        printf("Invalid value format: %d\n", value.format);
        return;
    }

    led_on = value.bool_value;
    vTaskResume(animateTH);
}

homekit_value_t led_brightness_get() {
    return HOMEKIT_INT(target_brightness);
}

void led_brightness_set(homekit_value_t value) {
    if (value.format != homekit_format_int) {
        return;
    }

    target_brightness = value.int_value;
    if (target_brightness > 0) {
	// Turn LED on if value update is received
	    led_on = true;
    }
    vTaskResume(animateTH);
}


const homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_lightbulb, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sample LED"),
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "Kariboo"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A2BABF19D"),
            HOMEKIT_CHARACTERISTIC(MODEL, "MyLED"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sample LED"),
            HOMEKIT_CHARACTERISTIC(
                ON, false,
                .getter=led_on_get,
                .setter=led_on_set
            ),
            HOMEKIT_CHARACTERISTIC(BRIGHTNESS, 100,
            .getter = led_brightness_get,
            .setter = led_brightness_set
            ),
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = (homekit_accessory_t**)accessories,
    .password = "111-11-111"
};

void user_init(void) {
    uart_set_baud(0, 115200);

    wifi_init();
    led_init();
    homekit_server_init(&config);

    xTaskCreate(animate_light_transition_task, animateLightPCName, 1024, NULL, 1, &animateTH);
}
