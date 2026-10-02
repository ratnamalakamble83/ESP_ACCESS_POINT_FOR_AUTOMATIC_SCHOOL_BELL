#ifndef RTC_H
#define RTC_H

#include "esp_err.h"
#include <stdint.h>

typedef struct
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour;

    uint8_t day;       // 1 = Sunday ... 7 = Saturday
    uint8_t date;
    uint8_t month;
    uint8_t year;      // 26 = 2026

} rtc_time_t;

esp_err_t rtc_initialize(void);

esp_err_t rtc_set_datetime(const rtc_time_t *time);

esp_err_t rtc_get_datetime(rtc_time_t *time);

#endif