#ifndef AUTH_H
#define AUTH_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* Initialize authentication */
esp_err_t auth_init(void);

/* Check whether a password is already stored */
bool auth_password_is_set(void);

/* Set a new password */
esp_err_t auth_set_password(
    const char *password
);

/* Check entered password */
bool auth_check_password(
    const char *password
);

/* Create login session */
bool auth_create_session(
    char *session,
    size_t session_size
);

/* Check login session */
bool auth_check_session(
    const char *session
);

/* Logout */
void auth_logout(void);

/* TEMPORARY: Clear stored password */
esp_err_t auth_clear_password(void);

#endif