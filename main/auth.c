#include "auth.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"


/* =========================================================
 * AUTHENTICATION SETTINGS
 * ========================================================= */

#define AUTH_NAMESPACE     "school_auth"
#define AUTH_PASSWORD_KEY  "password_hash"

#define PASSWORD_MIN_LENGTH 4
#define PASSWORD_MAX_LENGTH 63

#define HASH_SIZE 32

#define SESSION_LENGTH 32


/* =========================================================
 * TAG
 * ========================================================= */

static const char *TAG = "AUTH";


/* =========================================================
 * CURRENT LOGIN SESSION
 * ========================================================= */

static char current_session[SESSION_LENGTH + 1] = {0};


/* =========================================================
 * PASSWORD HASH FUNCTION
 * ========================================================= */

static void hash_password(
    const char *password,
    uint8_t hash[HASH_SIZE]
)
{
    mbedtls_sha256(
        (const unsigned char *)password,
        strlen(password),
        hash,
        0
    );
}


/* =========================================================
 * AUTH INITIALIZATION
 * ========================================================= */

esp_err_t auth_init(void)
{
    nvs_handle_t handle;

    esp_err_t ret;

    ret = nvs_open(
        AUTH_NAMESPACE,
        NVS_READWRITE,
        &handle
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open authentication NVS: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    nvs_close(handle);

    ESP_LOGI(
        TAG,
        "Authentication initialized"
    );

    return ESP_OK;
}


/* =========================================================
 * CHECK WHETHER PASSWORD IS SET
 * ========================================================= */

bool auth_password_is_set(void)
{
    nvs_handle_t handle;

    uint8_t hash[HASH_SIZE];

    size_t size = sizeof(hash);

    esp_err_t ret;


    ret = nvs_open(
        AUTH_NAMESPACE,
        NVS_READONLY,
        &handle
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open auth NVS"
        );

        return false;
    }


    ret = nvs_get_blob(
        handle,
        AUTH_PASSWORD_KEY,
        hash,
        &size
    );


    nvs_close(handle);


    if (ret == ESP_OK &&
        size == sizeof(hash))
    {
        ESP_LOGI(
            TAG,
            "Password is already configured"
        );

        return true;
    }


    ESP_LOGI(
        TAG,
        "Password is NOT configured"
    );

    return false;
}


/* =========================================================
 * SET PASSWORD
 * ========================================================= */

esp_err_t auth_set_password(
    const char *password
)
{
    if (password == NULL)
    {
        ESP_LOGE(
            TAG,
            "Password is NULL"
        );

        return ESP_ERR_INVALID_ARG;
    }


    size_t length = strlen(password);


    ESP_LOGI(
        TAG,
        "Setting password, length = %d",
        (int)length
    );


    /* Check minimum length */

    if (length < PASSWORD_MIN_LENGTH)
    {
        ESP_LOGE(
            TAG,
            "Password too short"
        );

        return ESP_ERR_INVALID_ARG;
    }


    /* Check maximum length */

    if (length > PASSWORD_MAX_LENGTH)
    {
        ESP_LOGE(
            TAG,
            "Password too long"
        );

        return ESP_ERR_INVALID_SIZE;
    }


    /* Create password hash */

    uint8_t hash[HASH_SIZE];


    hash_password(
        password,
        hash
    );


    /* Open NVS */

    nvs_handle_t handle;

    esp_err_t ret;


    ret = nvs_open(
        AUTH_NAMESPACE,
        NVS_READWRITE,
        &handle
    );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open auth NVS: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* Store hash */

    ret = nvs_set_blob(
        handle,
        AUTH_PASSWORD_KEY,
        hash,
        sizeof(hash)
    );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to save password hash: %s",
            esp_err_to_name(ret)
        );

        nvs_close(handle);

        return ret;
    }


    /* Commit */

    ret = nvs_commit(handle);


    nvs_close(handle);


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to commit password: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "Password configured successfully"
    );


    return ESP_OK;
}


/* =========================================================
 * CHECK PASSWORD
 * ========================================================= */

bool auth_check_password(
    const char *password
)
{
    if (password == NULL)
    {
        ESP_LOGE(
            TAG,
            "Password is NULL"
        );

        return false;
    }


    size_t entered_length = strlen(password);


    ESP_LOGI(
        TAG,
        "Password check: entered password length = %d",
        (int)entered_length
    );


    /* =====================================================
     * LOAD STORED HASH
     * ===================================================== */

    uint8_t stored_hash[HASH_SIZE];

    uint8_t entered_hash[HASH_SIZE];

    size_t size = sizeof(stored_hash);


    nvs_handle_t handle;

    esp_err_t ret;


    ret = nvs_open(
        AUTH_NAMESPACE,
        NVS_READONLY,
        &handle
    );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open authentication NVS: %s",
            esp_err_to_name(ret)
        );

        return false;
    }


    ret = nvs_get_blob(
        handle,
        AUTH_PASSWORD_KEY,
        stored_hash,
        &size
    );


    nvs_close(handle);


    /* =====================================================
     * CHECK STORED HASH
     * ===================================================== */

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Password hash not found: %s",
            esp_err_to_name(ret)
        );

        return false;
    }


    if (size != sizeof(stored_hash))
    {
        ESP_LOGE(
            TAG,
            "Invalid stored password hash size: %d",
            (int)size
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "Stored password hash loaded successfully"
    );


    /* =====================================================
     * HASH ENTERED PASSWORD
     * ===================================================== */

    hash_password(
        password,
        entered_hash
    );


    /* =====================================================
     * COMPARE HASHES
     * ===================================================== */

    if (memcmp(
            stored_hash,
            entered_hash,
            sizeof(stored_hash)
        ) == 0)
    {
        ESP_LOGI(
            TAG,
            "Password MATCH"
        );

        return true;
    }


    ESP_LOGW(
        TAG,
        "Password does NOT match"
    );


    return false;
}


/* =========================================================
 * CREATE LOGIN SESSION
 * ========================================================= */

bool auth_create_session(
    char *session,
    size_t session_size
)
{
    if (session == NULL)
    {
        return false;
    }


    if (session_size < SESSION_LENGTH + 1)
    {
        return false;
    }


    /* Random bytes */

    uint8_t random_bytes[
        SESSION_LENGTH / 2
    ];


    esp_fill_random(
        random_bytes,
        sizeof(random_bytes)
    );


    /* Convert bytes to hexadecimal */

    for (
        int i = 0;
        i < sizeof(random_bytes);
        i++
    )
    {
        sprintf(
            &current_session[i * 2],
            "%02X",
            random_bytes[i]
        );
    }


    /* NULL terminate */

    current_session[
        SESSION_LENGTH
    ] = '\0';


    /* Copy session to caller */

    strcpy(
        session,
        current_session
    );


    ESP_LOGI(
        TAG,
        "Login session created"
    );


    return true;
}


/* =========================================================
 * CHECK SESSION
 * ========================================================= */

bool auth_check_session(
    const char *session
)
{
    if (session == NULL)
    {
        return false;
    }


    if (current_session[0] == '\0')
    {
        return false;
    }


    if (strcmp(
            session,
            current_session
        ) == 0)
    {
        return true;
    }


    return false;
}


/* =========================================================
 * LOGOUT
 * ========================================================= */

void auth_logout(void)
{
    memset(
        current_session,
        0,
        sizeof(current_session)
    );


    ESP_LOGI(
        TAG,
        "User logged out"
    );
}


/* =========================================================
 * TEMPORARY FUNCTION
 *
 * Clear old password from NVS
 *
 * USE THIS ONLY ONCE DURING TESTING
 * ========================================================= */

esp_err_t auth_clear_password(void)
{
    nvs_handle_t handle;

    esp_err_t ret;


    ret = nvs_open(
        AUTH_NAMESPACE,
        NVS_READWRITE,
        &handle
    );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open auth NVS: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /* Delete password */

    ret = nvs_erase_key(
        handle,
        AUTH_PASSWORD_KEY
    );


    /*
     * If the key doesn't exist,
     * consider it already cleared.
     */

    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(
            TAG,
            "Password was already cleared"
        );

        nvs_close(handle);

        return ESP_OK;
    }


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to erase password: %s",
            esp_err_to_name(ret)
        );

        nvs_close(handle);

        return ret;
    }


    /* Commit */

    ret = nvs_commit(handle);


    nvs_close(handle);


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to commit password erase: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "Password cleared successfully"
    );


    return ESP_OK;
}