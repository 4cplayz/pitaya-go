#ifndef NFC_WIFI_CLIENT_H__
#define NFC_WIFI_CLIENT_H__

#include "nrf_cli.h"  // Include this for the nrf_cli_t type

/**
 * @brief Sets the CLI context for the NFC WiFi client
 *
 * @param p_cli Pointer to the CLI instance
 */
void nfc_client_set_cli(nrf_cli_t const * p_cli);

/**
 * @brief Initializes the NFC functionality
 */
void nfc_client_init(void);  // ADD THIS LINE

/**
 * @brief Initializes the WiFi module and connects to the configured AP
 */
void wifi_setup(void);

/**
 * @brief Processes WiFi events
 */
void wifi_process(void);

#endif // NFC_WIFI_CLIENT_H__