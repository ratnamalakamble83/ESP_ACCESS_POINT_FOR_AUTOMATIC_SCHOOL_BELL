#include "rtc.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* =========================================================
 * DS3232 I2C SETTINGS
 * ========================================================= */

#define RTC_I2C_PORT       I2C_NUM_0

#define RTC_SDA_PIN        GPIO_NUM_21
#define RTC_SCL_PIN        GPIO_NUM_22

#define RTC_I2C_FREQ       100000

#define RTC_ADDRESS        0x68

static const char *TAG = "RTC";


/* =========================================================
 * BCD CONVERSION
 * ========================================================= */

static uint8_t dec_to_bcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}

static uint8_t bcd_to_dec(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}


/* =========================================================
 * RTC INITIALIZATION
 * ========================================================= */

esp_err_t rtc_initialize(void)
{
    i2c_config_t config =
    {
        .mode = I2C_MODE_MASTER,

        .sda_io_num = RTC_SDA_PIN,
        .scl_io_num = RTC_SCL_PIN,

        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,

        .master.clk_speed = RTC_I2C_FREQ,

        .clk_flags = 0
    };

    esp_err_t ret;

    /* Configure I2C */
    ret = i2c_param_config(RTC_I2C_PORT, &config);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "I2C parameter configuration failed: %s",
                 esp_err_to_name(ret));

        return ret;
    }

    /* Install I2C driver */
    ret = i2c_driver_install(
        RTC_I2C_PORT,
        I2C_MODE_MASTER,
        0,
        0,
        0
    );

    /*
     * Driver may already be installed.
     */
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "I2C driver already installed");
    }
    else if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "I2C driver installation failed: %s",
                 esp_err_to_name(ret));

        return ret;
    }

    ESP_LOGI(TAG, "DS3232 I2C initialized");
    ESP_LOGI(TAG, "SDA = GPIO %d", RTC_SDA_PIN);
    ESP_LOGI(TAG, "SCL = GPIO %d", RTC_SCL_PIN);
    ESP_LOGI(TAG, "Address = 0x%02X", RTC_ADDRESS);

    return ESP_OK;
}


/* =========================================================
 * SET DATE AND TIME
 *
 * DS3232 TIME REGISTERS
 *
 * 0x00 = Seconds
 * 0x01 = Minutes
 * 0x02 = Hours
 * 0x03 = Day
 * 0x04 = Date
 * 0x05 = Month
 * 0x06 = Year
 * ========================================================= */

esp_err_t rtc_set_datetime(const rtc_time_t *time)
{
    if (time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * data[0] = starting register
     * data[1] = seconds
     * data[2] = minutes
     * data[3] = hours
     * data[4] = day
     * data[5] = date
     * data[6] = month
     * data[7] = year
     */

    uint8_t data[8];

    data[0] = 0x00;

    /* Seconds */
    data[1] = dec_to_bcd(time->second) & 0x7F;

    /* Minutes */
    data[2] = dec_to_bcd(time->minute) & 0x7F;

    /*
     * Hours
     *
     * Bit 6 = 0
     * Therefore 24-hour mode.
     */
    data[3] = dec_to_bcd(time->hour) & 0x3F;

    /*
     * Day of week
     *
     * 1 = Sunday
     * 2 = Monday
     * ...
     * 7 = Saturday
     */
    data[4] = dec_to_bcd(time->day) & 0x07;

    /* Date */
    data[5] = dec_to_bcd(time->date) & 0x3F;

    /*
     * Month
     *
     * DS3232 bit 7 is century bit.
     * We use 0 because we are storing 20xx.
     */
    data[6] = dec_to_bcd(time->month) & 0x1F;

    /*
     * Year
     *
     * Example:
     * 2026 -> 26
     */
    data[7] = dec_to_bcd(time->year);

    esp_err_t ret = i2c_master_write_to_device(
        RTC_I2C_PORT,
        RTC_ADDRESS,
        data,
        sizeof(data),
        pdMS_TO_TICKS(1000)
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "DS3232 write failed: %s",
                 esp_err_to_name(ret));

        return ret;
    }

    ESP_LOGI(TAG,
             "DS3232 SET: %02d/%02d/20%02d %02d:%02d:%02d",
             time->date,
             time->month,
             time->year,
             time->hour,
             time->minute,
             time->second);

    return ESP_OK;
}


/* =========================================================
 * GET DATE AND TIME
 * ========================================================= */

esp_err_t rtc_get_datetime(rtc_time_t *time)
{
    if (time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg = 0x00;

    uint8_t data[7];

    /*
     * Write register address 0x00
     * Then read 7 bytes.
     */

    esp_err_t ret = i2c_master_write_read_device(
        RTC_I2C_PORT,
        RTC_ADDRESS,
        &reg,
        1,
        data,
        7,
        pdMS_TO_TICKS(1000)
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "DS3232 read failed: %s",
                 esp_err_to_name(ret));

        return ret;
    }

    /*
     * Convert BCD -> decimal
     */

    /* Seconds */
    time->second = bcd_to_dec(data[0] & 0x7F);

    /* Minutes */
    time->minute = bcd_to_dec(data[1] & 0x7F);

    /*
     * Hours
     *
     * We configured 24-hour mode.
     */
    if ((data[2] & 0x40) == 0)
    {
        time->hour = bcd_to_dec(data[2] & 0x3F);
    }
    else
    {
        /*
         * Safety handling if RTC is accidentally
         * in 12-hour mode.
         */
        uint8_t hour = bcd_to_dec(data[2] & 0x1F);

        bool pm = (data[2] & 0x20) != 0;

        if (pm && hour != 12)
        {
            hour += 12;
        }

        if (!pm && hour == 12)
        {
            hour = 0;
        }

        time->hour = hour;
    }

    /* Day */
    time->day = bcd_to_dec(data[3] & 0x07);

    /* Date */
    time->date = bcd_to_dec(data[4] & 0x3F);

    /* Month */
    time->month = bcd_to_dec(data[5] & 0x1F);

    /* Year */
    time->year = bcd_to_dec(data[6]);

    ESP_LOGI(TAG,
             "DS3232 GET: %02d/%02d/20%02d %02d:%02d:%02d",
             time->date,
             time->month,
             time->year,
             time->hour,
             time->minute,
             time->second);

    return ESP_OK;
}