#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "driver/gpio.h"

#include "eeprom.h"
#include "auth.h"
#include "rtc.h"
#include "html_page.h"

/* =========================================================
 * SETTINGS
 * ========================================================= */
#define TAG                 "SCHOOL_BELL"
#define MAX_BELLS           20
#define BUZZER_PIN          GPIO_NUM_25
#define WIFI_SSID           "ESP32_SCHOOL"
#define WIFI_PASSWORD       "12345678"
#define MAX_STA_CONN        4
#define SESSION_COOKIE      "school_session"

/* =========================================================
 * BELL STRUCTURE
 * ========================================================= */
typedef struct
{
    uint8_t hour;
    uint8_t minute;
    char event[30];
    uint8_t ringtone;
    uint8_t repeat;       /* 1 to 10 */
    uint8_t duration;     /* 1 to 10 seconds */
} BellTime;

/* =========================================================
 * GLOBAL VARIABLES
 * ========================================================= */
static BellTime bell[MAX_BELLS];
static int total_bells = 0;
static httpd_handle_t server = NULL;

static int last_ring_hour = -1;
static int last_ring_minute = -1;

/* =========================================================
 * DAY NAMES
 * ========================================================= */
static const char *day_name[] =
{
    "",
    "Sunday",
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday"
};

/* =========================================================
 * URL DECODE
 * ========================================================= */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    size_t j = 0;
    while (src[i] != '\0' && j < dst_size - 1)
    {
        if (src[i] == '%')
        {
            if (src[i + 1] != '\0' && src[i + 2] != '\0')
            {
                char hex[3];
                hex[0] = src[i + 1];
                hex[1] = src[i + 2];
                hex[2] = '\0';
                char *endptr;
                long value = strtol(hex, &endptr, 16);
                if (*endptr == '\0')
                {
                    dst[j++] = (char)value;
                    i += 3;
                    continue;
                }
            }
        }
        if (src[i] == '+')
        {
            dst[j++] = ' ';
        }
        else
        {
            dst[j++] = src[i];
        }
        i++;
    }
    dst[j] = '\0';
}

/* =========================================================
 * EEPROM / NVS STORAGE HANDLERS
 * ========================================================= */
static esp_err_t save_bells_to_eeprom(void)
{
    if (total_bells < 0) total_bells = 0;
    if (total_bells > MAX_BELLS) total_bells = MAX_BELLS;

    EEPROM_BellTime stored[MAX_BELLS];
    memset(stored, 0, sizeof(stored));

    for (int i = 0; i < total_bells; i++)
    {
        stored[i].hour = bell[i].hour;
        stored[i].minute = bell[i].minute;
        strncpy(stored[i].event, bell[i].event, sizeof(stored[i].event) - 1);
        stored[i].ringtone = bell[i].ringtone;
        stored[i].repeat = bell[i].repeat;
        stored[i].duration = bell[i].duration;
    }

    esp_err_t ret = eeprom_save_bells(stored, (uint8_t)total_bells);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Bell schedule saved to NVS. Total bells = %d", total_bells);
    else
        ESP_LOGE(TAG, "Failed to save bells: %s", esp_err_to_name(ret));
    return ret;
}

static void load_bells_from_eeprom(void)
{
    EEPROM_BellTime stored[MAX_BELLS];
    memset(stored, 0, sizeof(stored));
    uint8_t count = 0;

    esp_err_t ret = eeprom_load_bells(stored, &count);

    if (ret == ESP_OK)
    {
        total_bells = (count <= MAX_BELLS) ? count : MAX_BELLS;
        for (int i = 0; i < total_bells; i++)
        {
            bell[i].hour = stored[i].hour;
            bell[i].minute = stored[i].minute;
            strncpy(bell[i].event, stored[i].event, sizeof(bell[i].event) - 1);
            bell[i].event[sizeof(bell[i].event) - 1] = '\0';
            bell[i].ringtone = (stored[i].ringtone >= 1 && stored[i].ringtone <= 4) ? stored[i].ringtone : 1;
            bell[i].repeat = (stored[i].repeat >= 1 && stored[i].repeat <= 10) ? stored[i].repeat : 1;
            bell[i].duration = (stored[i].duration >= 1 && stored[i].duration <= 10) ? stored[i].duration : 1;
            ESP_LOGI(TAG, "Bell %d: %02d:%02d | %s | ringtone=%d | repeat=%d | duration=%d",
                     i, bell[i].hour, bell[i].minute, bell[i].event, bell[i].ringtone, bell[i].repeat, bell[i].duration);
        }
        ESP_LOGI(TAG, "Loaded %d bells from NVS", total_bells);
    }
    else if (ret == ESP_ERR_NVS_INVALID_LENGTH)
    {
        total_bells = 0;
        ESP_LOGW(TAG, "Old bell schedule format found. Clearing old schedule.");
        if (eeprom_clear_bells() == ESP_OK)
            ESP_LOGI(TAG, "Old schedule cleared. Add bells again from the web page.");
    }
    else
    {
        total_bells = 0;
        ESP_LOGW(TAG, "No bell schedule loaded: %s", esp_err_to_name(ret));
    }
}

/* =========================================================
 * BUZZER RINGTONE DRIVER
 * ========================================================= */
static void play_ringtone(uint8_t type, uint8_t repeat, uint8_t duration)
{
    if (repeat < 1) repeat = 1;
    if (repeat > 10) repeat = 10;
    if (duration < 1) duration = 1;
    if (duration > 10) duration = 10;

    ESP_LOGI(TAG, "Playing ringtone=%d repeat=%d duration=%d sec", type, repeat, duration);

    for (uint8_t r = 0; r < repeat; r++)
    {
        switch (type)
        {
            case 1: /* CLASSIC BELL */
                gpio_set_level(BUZZER_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(duration * 1000));
                gpio_set_level(BUZZER_PIN, 0);
                break;

            case 2: /* LONG BELL */
                gpio_set_level(BUZZER_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(duration * 1000));
                gpio_set_level(BUZZER_PIN, 0);
                break;

            case 3: /* DOUBLE BELL */
                gpio_set_level(BUZZER_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(duration * 500));
                gpio_set_level(BUZZER_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
                gpio_set_level(BUZZER_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(duration * 500));
                gpio_set_level(BUZZER_PIN, 0);
                break;

            case 4: /* EMERGENCY BELL */
                for (int j = 0; j < 3; j++)
                {
                    gpio_set_level(BUZZER_PIN, 1);
                    vTaskDelay(pdMS_TO_TICKS(duration * 100));
                    gpio_set_level(BUZZER_PIN, 0);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                break;

            default:
                gpio_set_level(BUZZER_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(duration * 1000));
                gpio_set_level(BUZZER_PIN, 0);
                break;
        }

        if (r < repeat - 1)
        {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    gpio_set_level(BUZZER_PIN, 0);
}

/* =========================================================
 * AUTHENTICATION CHECK
 * ========================================================= */
static bool request_is_authenticated(httpd_req_t *req)
{
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len == 0)
    {
        return false;
    }

    char *cookie = malloc(cookie_len + 1);
    if (!cookie)
    {
        return false;
    }

    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, cookie_len + 1) != ESP_OK)
    {
        free(cookie);
        return false;
    }

    char *session_start = strstr(cookie, SESSION_COOKIE "=");
    if (session_start == NULL)
    {
        free(cookie);
        return false;
    }

    session_start += strlen(SESSION_COOKIE "=");

    char session[128];
    size_t i = 0;
    while (session_start[i] != '\0' &&
           session_start[i] != ';' &&
           session_start[i] != ' ' &&
           session_start[i] != '\r' &&
           session_start[i] != '\n' &&
           i < sizeof(session) - 1)
    {
        session[i] = session_start[i];
        i++;
    }
    session[i] = '\0';

    free(cookie);

    return auth_check_session(session);
}

/* =========================================================
 * LOGIN PAGE & SET PASSWORD PAGES
 * ========================================================= */
static const char login_page[] =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>School Bell Login</title>"
"<style>"
"body{font-family:Arial;text-align:center;background:#f2f2f2;padding:30px;}"
".box{max-width:400px;margin:auto;background:white;padding:25px;border-radius:10px;}"
"input{width:90%;padding:12px;margin:10px 0;}"
"button{padding:12px 25px;}"
"</style></head><body><div class='box'>"
"<h2>Automatic School Bell</h2><h3>Login</h3>"
"<form action='/login_check'>"
"<input type='password' name='password' placeholder='Enter password' required><br>"
"<button type='submit'>LOGIN</button>"
"</form></div></body></html>";

static const char set_password_page[] =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Set Password</title>"
"<style>"
"body{font-family:Arial;text-align:center;background:#f2f2f2;padding:30px;}"
".box{max-width:400px;margin:auto;background:white;padding:25px;border-radius:10px;}"
"input{width:90%;padding:12px;margin:10px 0;}"
"button{padding:12px 25px;}"
"</style></head><body><div class='box'>"
"<h2>Automatic School Bell</h2><h3>Create Password</h3>"
"<form action='/set_password'>"
"<input type='password' name='password' placeholder='New password' required><br>"
"<input type='password' name='confirm' placeholder='Confirm password' required><br>"
"<button type='submit'>SET PASSWORD</button>"
"</form><p>Minimum 4 characters</p></div></body></html>";

/* =========================================================
 * HTTP HANDLERS
 * ========================================================= */
static esp_err_t root_handler(httpd_req_t *req)
{
    if (!auth_password_is_set())
    {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, set_password_page, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!request_is_authenticated(req))
    {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, login_page, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t login_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password missing");
        return ESP_FAIL;
    }

    char encoded_password[128];
    if (httpd_query_key_value(query, "password", encoded_password, sizeof(encoded_password)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password missing");
        return ESP_FAIL;
    }

    char password[128];
    url_decode(password, encoded_password, sizeof(password));

    if (!auth_check_password(password))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Incorrect password");
        return ESP_FAIL;
    }

    char session[128];
    if (!auth_create_session(session, sizeof(session)))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not create session");
        return ESP_FAIL;
    }

    char cookie[180];
    snprintf(cookie, sizeof(cookie), SESSION_COOKIE "=%s; Path=/; HttpOnly", session);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);

    ESP_LOGI(TAG, "User logged in successfully");
    return ESP_OK;
}

static esp_err_t set_password_handler(httpd_req_t *req)
{
    if (auth_password_is_set())
    {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Password already configured");
        return ESP_FAIL;
    }

    char query[300];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    char encoded_password[128], encoded_confirm[128];
    if (httpd_query_key_value(query, "password", encoded_password, sizeof(encoded_password)) != ESP_OK ||
        httpd_query_key_value(query, "confirm", encoded_confirm, sizeof(encoded_confirm)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password missing");
        return ESP_FAIL;
    }

    char password[128], confirm[128];
    url_decode(password, encoded_password, sizeof(password));
    url_decode(confirm, encoded_confirm, sizeof(confirm));

    if (strlen(password) < 4)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password must contain at least 4 characters");
        return ESP_FAIL;
    }

    if (strcmp(password, confirm) != 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Passwords do not match");
        return ESP_FAIL;
    }

    esp_err_t ret = auth_set_password(password);
    if (ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save password");
        return ret;
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    ESP_LOGI(TAG, "Password configured successfully");
    return ESP_OK;
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    auth_logout();
    httpd_resp_set_hdr(req, "Set-Cookie", SESSION_COOKIE "=; Path=/; Max-Age=0");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t time_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    rtc_time_t now;
    esp_err_t ret = rtc_get_datetime(&now);
    if (ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "RTC read failed");
        return ret;
    }

    char response[200];
    const char *day = "";
    if (now.day >= 1 && now.day <= 7)
    {
        day = day_name[now.day];
    }

    snprintf(response, sizeof(response),
             "{\"day\":\"%s\",\"date\":\"%02d/%02d/20%02d\",\"time\":\"%02d:%02d:%02d\",\"iso_date\":\"20%02d-%02d-%02d\"}",
             day, now.date, now.month, now.year, now.hour, now.minute, now.second, now.year, now.month, now.date);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t set_datetime_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char query[300];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    char encoded_date[32], encoded_time[32];
    if (httpd_query_key_value(query, "date", encoded_date, sizeof(encoded_date)) != ESP_OK ||
        httpd_query_key_value(query, "time", encoded_time, sizeof(encoded_time)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Date or time missing");
        return ESP_FAIL;
    }

    char decoded_date[32], decoded_time[32];
    url_decode(decoded_date, encoded_date, sizeof(decoded_date));
    url_decode(decoded_time, encoded_time, sizeof(decoded_time));

    int year = 0, month = 0, date = 0, hour = 0, minute = 0;

    // Try parsing YYYY-MM-DD or YYYY/MM/DD
    if (sscanf(decoded_date, "%d-%d-%d", &year, &month, &date) != 3 &&
        sscanf(decoded_date, "%d/%d/%d", &year, &month, &date) != 3)
    {
        // Fallback for DD/MM/YYYY or DD-MM-YYYY
        if (sscanf(decoded_date, "%d/%d/%d", &date, &month, &year) != 3 &&
            sscanf(decoded_date, "%d-%d-%d", &date, &month, &year) != 3)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid datetime format");
            return ESP_FAIL;
        }
    }

    if (year < 100) year += 2000;

    if (sscanf(decoded_time, "%d:%d", &hour, &minute) != 2)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time format");
        return ESP_FAIL;
    }

    if (year < 2000 || year > 2099 || month < 1 || month > 12 || date < 1 || date > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid datetime values");
        return ESP_FAIL;
    }

    /* Zeller's Congruence algorithm to derive day of the week */
    int y = year, m = month;
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    int h = (date + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    int day = ((h + 6) % 7) + 1;

    rtc_time_t set_time = {
        .year = (uint8_t)(year - 2000),
        .month = (uint8_t)month,
        .date = (uint8_t)date,
        .day = (uint8_t)day,
        .hour = (uint8_t)hour,
        .minute = (uint8_t)minute,
        .second = 0
    };

    esp_err_t ret = rtc_set_datetime(&set_time);
    if (ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "RTC write failed");
        return ret;
    }

    ESP_LOGI(TAG, "Date/time set: %04d-%02d-%02d %02d:%02d", year, month, date, hour, minute);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    if (total_bells >= MAX_BELLS)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Maximum 20 bells reached");
        return ESP_FAIL;
    }

    char query[500];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    char encoded_time[32], encoded_event[100];
    char ringtone_str[10] = "1", repeat_str[10] = "1", duration_str[10] = "1";

    if (httpd_query_key_value(query, "time", encoded_time, sizeof(encoded_time)) != ESP_OK ||
        httpd_query_key_value(query, "event", encoded_event, sizeof(encoded_event)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing arguments");
        return ESP_FAIL;
    }

    httpd_query_key_value(query, "ringtone", ringtone_str, sizeof(ringtone_str));
    httpd_query_key_value(query, "repeat", repeat_str, sizeof(repeat_str));
    httpd_query_key_value(query, "duration", duration_str, sizeof(duration_str));

    char decoded_time[32], decoded_event[100];
    url_decode(decoded_time, encoded_time, sizeof(decoded_time));
    url_decode(decoded_event, encoded_event, sizeof(decoded_event));

    int hour, minute;
    if (sscanf(decoded_time, "%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time format");
        return ESP_FAIL;
    }

    int ringtone = atoi(ringtone_str);
    int repeat = atoi(repeat_str);
    int duration = atoi(duration_str);

    if (ringtone < 1 || ringtone > 4) ringtone = 1;
    if (repeat < 1 || repeat > 10) repeat = 1;
    if (duration < 1 || duration > 10) duration = 1;

    bell[total_bells].hour = (uint8_t)hour;
    bell[total_bells].minute = (uint8_t)minute;
    strncpy(bell[total_bells].event, decoded_event, sizeof(bell[total_bells].event) - 1);
    bell[total_bells].event[sizeof(bell[total_bells].event) - 1] = '\0';
    bell[total_bells].ringtone = (uint8_t)ringtone;
    bell[total_bells].repeat = (uint8_t)repeat;
    bell[total_bells].duration = (uint8_t)duration;

    total_bells++;

    esp_err_t save_ret = save_bells_to_eeprom();
    if (save_ret != ESP_OK)
    {
        total_bells--;
        memset(&bell[total_bells], 0, sizeof(bell[total_bells]));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save schedule");
        return save_ret;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char query[100];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Index missing");
        return ESP_FAIL;
    }

    char index_str[10];
    if (httpd_query_key_value(query, "index", index_str, sizeof(index_str)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Index missing");
        return ESP_FAIL;
    }

    int index = atoi(index_str);
    if (index < 0 || index >= total_bells)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid bell index");
        return ESP_FAIL;
    }

    for (int i = index; i < total_bells - 1; i++)
    {
        bell[i] = bell[i + 1];
    }
    total_bells--;

    esp_err_t save_ret = save_bells_to_eeprom();
    if (save_ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save schedule");
        return save_ret;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t get_bells_handler(httpd_req_t *req)
{
    if (!request_is_authenticated(req))
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char response[6000];
    int pos = 0;

    int n = snprintf(response + pos, sizeof(response) - pos, "[");
    if (n < 0 || n >= (int)(sizeof(response) - pos)) return ESP_FAIL;
    pos += n;

    for (int i = 0; i < total_bells; i++)
    {
        char event_json[120];
        int ep = 0;

        for (int j = 0; bell[i].event[j] != '\0' && ep < (int)sizeof(event_json) - 2; j++)
        {
            unsigned char c = (unsigned char)bell[i].event[j];
            if (c == '"' || c == '\\')
            {
                event_json[ep++] = '\\';
                event_json[ep++] = c;
            }
            else
            {
                event_json[ep++] = c;
            }
        }
        event_json[ep] = '\0';

        n = snprintf(response + pos, sizeof(response) - pos,
                     "%s{\"hour\":%d,\"minute\":%d,\"event\":\"%s\",\"ringtone\":%d,\"repeat\":%d,\"duration\":%d}",
                     (i == 0) ? "" : ",",
                     bell[i].hour, bell[i].minute, event_json, bell[i].ringtone, bell[i].repeat, bell[i].duration);

        if (n < 0 || n >= (int)(sizeof(response) - pos)) return ESP_FAIL;
        pos += n;
    }

    n = snprintf(response + pos, sizeof(response) - pos, "]");
    if (n < 0 || n >= (int)(sizeof(response) - pos)) return ESP_FAIL;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* =========================================================
 * BACKGROUND SCHEDULER TASK
 * ========================================================= */
static void bell_task(void *pvParameters)
{
    rtc_time_t now;

    while (1)
    {
        if (rtc_get_datetime(&now) == ESP_OK)
        {
            if (now.minute != last_ring_minute || now.hour != last_ring_hour)
            {
                for (int i = 0; i < total_bells; i++)
                {
                    if (bell[i].hour == now.hour && bell[i].minute == now.minute)
                    {
                        ESP_LOGI(TAG, "Triggering bell: %s", bell[i].event);
                        last_ring_hour = now.hour;
                        last_ring_minute = now.minute;
                        play_ringtone(bell[i].ringtone, bell[i].repeat, bell[i].duration);
                        break;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* =========================================================
 * WIFI & WEBSERVER INITIALIZATION
 * ========================================================= */
static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .password = WIFI_PASSWORD,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen(WIFI_PASSWORD) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi SoftAP started. SSID: %s", WIFI_SSID);
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_login = { .uri = "/login_check", .method = HTTP_GET, .handler = login_handler };
        httpd_register_uri_handler(server, &uri_login);

        httpd_uri_t uri_set_pass = { .uri = "/set_password", .method = HTTP_GET, .handler = set_password_handler };
        httpd_register_uri_handler(server, &uri_set_pass);

        httpd_uri_t uri_logout = { .uri = "/logout", .method = HTTP_GET, .handler = logout_handler };
        httpd_register_uri_handler(server, &uri_logout);

        httpd_uri_t uri_get_time = { .uri = "/time", .method = HTTP_GET, .handler = time_handler };
        httpd_register_uri_handler(server, &uri_get_time);

        httpd_uri_t uri_set_time = { .uri = "/set_datetime", .method = HTTP_GET, .handler = set_datetime_handler };
        httpd_register_uri_handler(server, &uri_set_time);

        httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_GET, .handler = save_handler };
        httpd_register_uri_handler(server, &uri_save);

        httpd_uri_t uri_delete = { .uri = "/delete", .method = HTTP_GET, .handler = delete_handler };
        httpd_register_uri_handler(server, &uri_delete);

        httpd_uri_t uri_get_bells = { .uri = "/get_bells", .method = HTTP_GET, .handler = get_bells_handler };
        httpd_register_uri_handler(server, &uri_get_bells);

        ESP_LOGI(TAG, "Web Server active and listening");
    }
}

/* =========================================================
 * MAIN ENTRY
 * ========================================================= */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(BUZZER_PIN, 0);

    rtc_initialize();
    load_bells_from_eeprom();

    wifi_init_softap();
    start_webserver();

    xTaskCreate(&bell_task, "bell_task", 4096, NULL, 5, NULL);
}