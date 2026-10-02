
#include "eeprom.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "EEPROM";

#define NVS_NAMESPACE  "school_bell"
#define KEY_BELLS      "bells"
#define KEY_COUNT      "count"


/* =========================================================
 * INITIALIZE NVS
 * ========================================================= */

esp_err_t eeprom_init(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase");

        ret = nvs_flash_erase();

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "NVS erase failed: %s",
                     esp_err_to_name(ret));

            return ret;
        }

        ret = nvs_flash_init();
    }

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "NVS initialized");
    }
    else
    {
        ESP_LOGE(TAG,
                 "NVS initialization failed: %s",
                 esp_err_to_name(ret));
    }

    return ret;
}


/* =========================================================
 * SAVE BELLS
 * ========================================================= */

esp_err_t eeprom_save_bells(const EEPROM_BellTime *bells,
                            uint8_t total_bells)
{
    nvs_handle_t handle;
    esp_err_t ret;

    /* Check bell count */
    if (total_bells > EEPROM_MAX_BELLS)
    {
        ESP_LOGW(TAG,
                 "Bell count %d exceeds maximum %d",
                 total_bells,
                 EEPROM_MAX_BELLS);

        total_bells = EEPROM_MAX_BELLS;
    }

    /* Open NVS */
    ret = nvs_open(NVS_NAMESPACE,
                   NVS_READWRITE,
                   &handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "NVS open failed: %s",
                 esp_err_to_name(ret));

        return ret;
    }


    /* =====================================================
     * CASE 1: No bells
     *
     * Delete old bell blob.
     * Store count = 0.
     * ===================================================== */

    if (total_bells == 0)
    {
        ret = nvs_erase_key(handle, KEY_BELLS);

        /*
         * If key does not exist, that is not an error.
         */
        if (ret != ESP_OK &&
            ret != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGE(TAG,
                     "Failed to erase bell data: %s",
                     esp_err_to_name(ret));

            nvs_close(handle);
            return ret;
        }

        /* Store bell count = 0 */
        ret = nvs_set_u8(handle,
                         KEY_COUNT,
                         0);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "Failed to save bell count: %s",
                     esp_err_to_name(ret));

            nvs_close(handle);
            return ret;
        }

        /* Commit */
        ret = nvs_commit(handle);

        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "Bell schedule cleared");
        }
        else
        {
            ESP_LOGE(TAG,
                     "NVS commit failed: %s",
                     esp_err_to_name(ret));
        }

        nvs_close(handle);

        return ret;
    }


    /* =====================================================
     * CASE 2: Bells available
     * ===================================================== */

    if (bells == NULL)
    {
        ESP_LOGE(TAG,
                 "bells pointer is NULL");

        nvs_close(handle);

        return ESP_ERR_INVALID_ARG;
    }


    /* Save bell array */
    ret = nvs_set_blob(handle,
                       KEY_BELLS,
                       bells,
                       sizeof(EEPROM_BellTime) * total_bells);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to save bells: %s",
                 esp_err_to_name(ret));

        nvs_close(handle);

        return ret;
    }


    /* Save number of bells */
    ret = nvs_set_u8(handle,
                     KEY_COUNT,
                     total_bells);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to save bell count: %s",
                 esp_err_to_name(ret));

        nvs_close(handle);

        return ret;
    }


    /* Commit data */
    ret = nvs_commit(handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "NVS commit failed: %s",
                 esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG,
                 "Saved %d bell(s) to NVS",
                 total_bells);
    }

    nvs_close(handle);

    return ret;
}


/* =========================================================
 * DELETE ONE BELL
 * ========================================================= */

esp_err_t eeprom_delete_bell(uint8_t index)
{
    EEPROM_BellTime bells[EEPROM_MAX_BELLS];

    uint8_t total_bells = 0;

    esp_err_t ret;


    /* Load existing bells */
    ret = eeprom_load_bells(bells,
                            &total_bells);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to load bells before delete");

        return ret;
    }


    /* Check index */
    if (index >= total_bells)
    {
        ESP_LOGE(TAG,
                 "Invalid bell index: %d",
                 index);

        return ESP_ERR_INVALID_ARG;
    }


    /* Shift remaining bells */
    for (uint8_t i = index;
         i < total_bells - 1;
         i++)
    {
        bells[i] = bells[i + 1];
    }


    /* Reduce bell count */
    total_bells--;


    /* Save updated list */
    ret = eeprom_save_bells(bells,
                            total_bells);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "Deleted bell at index %d. "
                 "Remaining bells: %d",
                 index,
                 total_bells);
    }
    else
    {
        ESP_LOGE(TAG,
                 "Failed to save after deleting bell");
    }

    return ret;
}


/* =========================================================
 * LOAD BELLS
 * ========================================================= */

esp_err_t eeprom_load_bells(EEPROM_BellTime *bells,
                            uint8_t *total_bells)
{
    nvs_handle_t handle;

    esp_err_t ret;

    size_t stored_size = 0;

    uint8_t count = 0;


    /* Check pointers */
    if (bells == NULL ||
        total_bells == NULL)
    {
        ESP_LOGE(TAG,
                 "Invalid NULL pointer");

        return ESP_ERR_INVALID_ARG;
    }


    *total_bells = 0;


    /* Open NVS */
    ret = nvs_open(NVS_NAMESPACE,
                   NVS_READONLY,
                   &handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG,
                 "No saved bell schedule");

        return ESP_OK;
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "NVS open failed: %s",
                 esp_err_to_name(ret));

        return ret;
    }


    /* =====================================================
     * Read bell count
     * ===================================================== */

    ret = nvs_get_u8(handle,
                     KEY_COUNT,
                     &count);

    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);

        ESP_LOGI(TAG,
                 "No saved bell count");

        return ESP_OK;
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to read bell count: %s",
                 esp_err_to_name(ret));

        nvs_close(handle);

        return ret;
    }


    /* Check maximum */
    if (count > EEPROM_MAX_BELLS)
    {
        ESP_LOGW(TAG,
                 "Stored bell count %d exceeds maximum %d",
                 count,
                 EEPROM_MAX_BELLS);

        count = EEPROM_MAX_BELLS;
    }


    /* =====================================================
     * No bells
     * ===================================================== */

    if (count == 0)
    {
        nvs_close(handle);

        ESP_LOGI(TAG,
                 "Bell schedule is empty");

        return ESP_OK;
    }


    /* =====================================================
     * Get actual stored blob size
     * ===================================================== */

    ret = nvs_get_blob(handle,
                       KEY_BELLS,
                       NULL,
                       &stored_size);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Could not get bell blob size: %s",
                 esp_err_to_name(ret));

        nvs_close(handle);

        return ret;
    }


    /* Expected size */
    size_t expected_size =
        sizeof(EEPROM_BellTime) * count;


    ESP_LOGI(TAG,
             "Stored blob size = %zu, expected = %zu",
             stored_size,
             expected_size);


    /* =====================================================
     * Check compatibility
     * ===================================================== */

    if (stored_size != expected_size)
    {
        ESP_LOGW(TAG,
                 "Old/incompatible bell schedule found");

        nvs_close(handle);

        /*
         * Clear incompatible schedule.
         */
        ret = eeprom_clear_bells();

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "Failed to clear incompatible schedule: %s",
                     esp_err_to_name(ret));

            return ret;
        }

        *total_bells = 0;

        return ESP_OK;
    }


    /* =====================================================
     * Read actual bell data
     * ===================================================== */

    size_t read_size = stored_size;

    ret = nvs_get_blob(handle,
                       KEY_BELLS,
                       bells,
                       &read_size);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to load bells: %s",
                 esp_err_to_name(ret));

        nvs_close(handle);

        return ret;
    }


    /* Final size check */
    if (read_size != expected_size)
    {
        ESP_LOGE(TAG,
                 "Read size mismatch: %zu != %zu",
                 read_size,
                 expected_size);

        nvs_close(handle);

        return ESP_ERR_INVALID_SIZE;
    }


    /* Return bell count */
    *total_bells = count;


    nvs_close(handle);


    ESP_LOGI(TAG,
             "Loaded %d bell(s) from NVS",
             count);

    return ESP_OK;
}


/* =========================================================
 * CLEAR ALL BELLS
 * ========================================================= */

esp_err_t eeprom_clear_bells(void)
{
    nvs_handle_t handle;

    esp_err_t ret;


    /* Open NVS */
    ret = nvs_open(NVS_NAMESPACE,
                   NVS_READWRITE,
                   &handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "NVS open failed while clearing: %s",
                 esp_err_to_name(ret));

        return ret;
    }


    /* Delete bell array */
    ret = nvs_erase_key(handle,
                        KEY_BELLS);

    /*
     * Key may already be absent.
     */
    if (ret != ESP_OK &&
        ret != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG,
                 "Failed to erase bell data: %s",
                 esp_err_to_name(ret));

        nvs_close(handle);

        return ret;
    }


    /* Set bell count to zero */
    ret = nvs_set_u8(handle,
                     KEY_COUNT,
                     0);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to clear bell count: %s",
                 esp_err_to_name(ret));

        nvs_close(handle);

        return ret;
    }


    /* Commit */
    ret = nvs_commit(handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to commit clear operation: %s",
                 esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG,
                 "All bells cleared from NVS");
    }


    nvs_close(handle);

    return ret;
}



