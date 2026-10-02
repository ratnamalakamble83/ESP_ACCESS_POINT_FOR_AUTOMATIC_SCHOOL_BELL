#ifndef EEPROM_H
#define EEPROM_H

#include "esp_err.h"
#include <stdint.h>

#define EEPROM_MAX_BELLS 20

typedef struct
{
    uint8_t hour;
    uint8_t minute;

    char event[30];

    uint8_t ringtone;

    // New settings
    uint8_t repeat;      // 1 to 10 repetitions
    uint8_t duration;    // 1 to 10 seconds
} EEPROM_BellTime;


/* =========================================================
 * Initialize NVS
 * ========================================================= */
esp_err_t eeprom_init(void);


/* =========================================================
 * Save all bell events
 * ========================================================= */
esp_err_t eeprom_save_bells(
    const EEPROM_BellTime *bells,
    uint8_t total_bells
);


/* =========================================================
 * Delete one bell
 * ========================================================= */
esp_err_t eeprom_delete_bell(uint8_t index);


/* =========================================================
 * Load all bell events
 * ========================================================= */
esp_err_t eeprom_load_bells(
    EEPROM_BellTime *bells,
    uint8_t *total_bells
);


/* =========================================================
 * Delete all stored bell events
 * ========================================================= */
esp_err_t eeprom_clear_bells(void);

#endif