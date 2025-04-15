/**
* Copyright (c) 2019 makerdiary
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*
* * Redistributions of source code must retain the above copyright
*   notice, this list of conditions and the following disclaimer.
*
* * Redistributions in binary form must reproduce the above
*   copyright notice, this list of conditions and the following
*   disclaimer in the documentation and/or other materials provided
*   with the distribution.

* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
* A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
* OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/
/** @file nfc_wifi_client_demo.c
 * @brief This example demonstrates the use of the Pitaya Go board
 * to retrieve NFC link information from a custom API server and
 * emulate it as an NFC tag.
 *
 */

#include "nrf_cli.h"
#include "nrf_delay.h"
#include "boards.h"
#include "app_timer.h"

#include "nfc_t2t_lib.h"
#include "nfc_uri_msg.h"
#include "sdk_config.h"

#include "driver/include/m2m_wifi.h"
#include "driver/source/nmasic.h"
#include "socket/include/socket.h"

// Forward declarations
static void nfc_init(void);
static void timers_init(void);
static void leds_init(void);
void nfc_client_init(void); // Add this line

/* Configuration - customize these values */
#define MAIN_HOST_PORT 443 // Changed to 443 for HTTPS
#define MAIN_NFC_SERVER_NAME "iato.ca"
#define MAIN_USE_SSL 1 // Enable SSL

/* HTTP Request formatting */
#define HTTP_REQUEST_TEMPLATE                 \
    "GET /api/nfc/generate-link HTTP/1.1\r\n" \
    "Host: %s\r\n"                            \
    "Connection: close\r\n"                   \
    "\r\n"

/* Wi-Fi credentials */
#define WIFI_SSID "CAL-Techno"             // Replace with your Wi-Fi SSID
#define WIFI_PASSWORD "technophys123"      // Replace with your Wi-Fi password
#define WIFI_SECURITY M2M_WIFI_SEC_WPA_PSK // Use appropriate security type

/* NFC link refresh interval in milliseconds */
#define NFC_LINK_REFRESH_INTERVAL_MS 60000 // 1 minute (adjust as needed)

/* LED indicators */
#define LED_WIFI_CONNECTED LED_B_IDX
#define LED_NFC_ACTIVITY LED_G_IDX
#define LED_ERROR LED_R_IDX

/** Receive buffer size. */
#define MAIN_WIFI_M2M_BUFFER_SIZE 1400

/** IP address parsing. */
#define IPV4_BYTE(val, index) ((val >> (index * 8)) & 0xFF)

const char *strSecType[M2M_WIFI_NUM_AUTH_TYPES] = {"Invalid", "Open", "WPA/WPA2 personal(PSK)", "WEP (40 or 104) OPEN OR SHARED", "WPA/WPA2 Enterprise.IEEE802.1x"};

/** Mac address information. */
static uint8_t m_mac_addr[M2M_MAC_ADDRES_LEN];

/** User define MAC Address. */
static char m_user_define_mac_address[] = {0xf8, 0xf0, 0x05, 0x00, 0x00, 0x00};

/** CLI context for output */
static nrf_cli_t const *mp_curr_cli = NULL;

/** Wi-Fi connection state. */
static bool m_wifi_connected = false;

/** TCP client socket handler. */
static SOCKET m_tcp_client_socket = -1;

/** Server host name. */
static char m_server_host_name[] = MAIN_NFC_SERVER_NAME;

/** Receive buffer definition. */
static uint8_t m_tcp_received_buffer[MAIN_WIFI_M2M_BUFFER_SIZE];

/** Timer instance for periodic NFC link refresh */
APP_TIMER_DEF(m_nfc_refresh_timer);

/** State variables for automatic operation */
typedef enum
{
    APP_STATE_INIT,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_NFC_REQUEST_PENDING,
    APP_STATE_NFC_REQUEST_DONE,
    APP_STATE_ERROR
} app_state_t;

static app_state_t m_app_state = APP_STATE_INIT;
static bool m_nfc_request_in_progress = false;

// Static buffer to accumulate response data across multiple receives
static char accumulated_response[MAIN_WIFI_M2M_BUFFER_SIZE * 2] = {0};
static int accumulated_length = 0;
static bool is_accumulating = false;
static int receive_attempts = 0;
static const int MAX_RECEIVE_ATTEMPTS = 2;

// Storage for parsed NFC link data
typedef struct
{
    char token[256];
    char url[256];
    char expires[64];
    char expires_in[64];
    bool valid;
} nfc_link_data_t;

static nfc_link_data_t m_current_nfc_link = {0};

// NFC variables
static uint8_t m_ndef_msg_buf[256];

/**
 * \brief Update the NFC tag with current URL
 */
static void update_nfc_tag(void)
{
    ret_code_t err_code;

    // Get the full URL from the API response
    const char *full_url = m_current_nfc_link.url;
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Full URL from API: %s\r\n", full_url);

    // Stop NFC emulation first
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Stopping NFC emulation...\r\n");
    err_code = nfc_t2t_emulation_stop();
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Failed to stop NFC emulation. Error: 0x%X\r\n", err_code);
        return;
    }

    // Add a delay to ensure emulation is fully stopped
    nrf_delay_ms(100);

    // Prepare a buffer for the new message
    uint8_t new_msg_buf[256];
    uint32_t len = sizeof(new_msg_buf);

    // Encode the URI message with HTTP prefix using the full URL
    const uint8_t *url_data = (const uint8_t *)full_url;
    uint8_t url_len = strlen(full_url);

    err_code = nfc_uri_msg_encode(NFC_URI_HTTP, url_data, url_len, new_msg_buf, &len);
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Failed to encode NFC message. Error: 0x%X\r\n", err_code);

        // Try to restart emulation with original payload
        nfc_t2t_emulation_start();
        return;
    }

    // Set the new payload
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Setting new NFC payload (len=%d)...\r\n", len);
    err_code = nfc_t2t_payload_set(new_msg_buf, len);
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Failed to set payload. Error: 0x%X\r\n", err_code);

        // Try to restart emulation with original payload
        nfc_t2t_emulation_start();
        return;
    }

    // Restart emulation
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Restarting NFC emulation...\r\n");
    err_code = nfc_t2t_emulation_start();
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Failed to restart NFC emulation. Error: 0x%X\r\n", err_code);
        return;
    }

    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC tag updated successfully with full URL\r\n");
    nrf_cli_process(mp_curr_cli);
}

/**
 * \brief Find a JSON string value by key.
 *
 * \param[in] json_str The JSON string to search.
 * \param[in] key The key to find.
 * \param[out] value_buf Buffer to store the found value.
 * \param[in] buf_size Size of the value buffer.
 *
 * \return true if key was found and value copied, false otherwise.
 */
static bool find_json_string_value(const char *json_str, const char *key, char *value_buf, size_t buf_size)
{
    char key_pattern[64];
    char *value_start, *value_end;

    // Format the key pattern to search for, e.g. "token":"
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\":\"", key);

    // Find the key
    value_start = strstr(json_str, key_pattern);
    if (!value_start)
    {
        // Try with spaces
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\": \"", key);
        value_start = strstr(json_str, key_pattern);
        if (!value_start)
        {
            return false;
        }
    }

    // Move to the beginning of the value
    value_start += strlen(key_pattern);

    // Find the end of the value (closing quote)
    value_end = strchr(value_start, '\"');
    if (!value_end)
    {
        return false;
    }

    // Calculate value length
    size_t value_len = value_end - value_start;
    if (value_len >= buf_size)
    {
        value_len = buf_size - 1; // Ensure space for null terminator
    }

    // Copy the value
    memcpy(value_buf, value_start, value_len);
    value_buf[value_len] = '\0';

    return true;
}

/**
 * \brief Process the accumulated HTTP response and extract NFC link data.
 *
 * \param[in] response The HTTP response string to process.
 * \param[out] link_data Structure to store the extracted link data.
 *
 * \return true if successful parsing, false otherwise.
 */
static bool process_nfc_response(const char *response, nfc_link_data_t *link_data)
{
    if (!response || !link_data)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Invalid response or link_data parameter\r\n");
        return false;
    }

    // Print the first portion of the response for debugging
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Processing response (first 200 chars):\r\n");
    int print_len = strlen(response) > 200 ? 200 : strlen(response);
    for (int i = 0; i < print_len; i++)
    {
        if (response[i] >= 32 && response[i] <= 126)
        {
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "%c", response[i]);
        }
        else
        {
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "\\x%02x", response[i]);
        }
    }
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "\r\n");

    // Check for successful HTTP response (be more flexible with status code)
    if (strstr(response, "HTTP/1.1 200") == NULL &&
        strstr(response, "HTTP/1.0 200") == NULL &&
        strstr(response, "200 OK") == NULL)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "HTTP request failed - status code not 200\r\n");
        return false;
    }

    // Find the JSON part
    char *json_start = (char *)response;

    // Look for the end of headers
    char *headers_end = strstr(response, "\r\n\r\n");
    if (headers_end)
    {
        json_start = headers_end + 4;
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Found end of headers\r\n");
    }
    else
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "Could not find end of headers, attempting to parse anyway\r\n");
    }

    // Look for the beginning of the JSON object
    char *brace = strchr(json_start, '{');
    if (!brace)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "No JSON object found in response\r\n");
        return false;
    }

    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Found JSON data\r\n");
    json_start = brace;

    // Extract token
    if (!find_json_string_value(json_start, "token", link_data->token, sizeof(link_data->token)))
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "No token found in response\r\n");
        link_data->token[0] = '\0';
    }

    // Extract URL
    if (!find_json_string_value(json_start, "url", link_data->url, sizeof(link_data->url)))
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "No URL found in response\r\n");
        link_data->url[0] = '\0';
    }

    // Extract expires time
    if (!find_json_string_value(json_start, "expires", link_data->expires, sizeof(link_data->expires)))
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "No expiration time found in response\r\n");
        link_data->expires[0] = '\0';
    }

    // Extract expires in
    if (!find_json_string_value(json_start, "expiresIn", link_data->expires_in, sizeof(link_data->expires_in)))
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "No expiration period found in response\r\n");
        link_data->expires_in[0] = '\0';
    }

    // Mark as valid if we have at least a token and URL
    link_data->valid = (link_data->token[0] != '\0' && link_data->url[0] != '\0');

    if (link_data->valid)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Successfully parsed NFC link data\r\n");
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Token: %s\r\n", link_data->token);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "URL: %s\r\n", link_data->url);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Expires: %s\r\n", link_data->expires);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Expires in: %s\r\n", link_data->expires_in);
    }
    else
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Failed to parse valid NFC link data\r\n");
    }

    return link_data->valid;
}

/**
 * \brief Trigger a request for a new NFC link.
 *
 * \return true if request was initiated, false otherwise.
 */
static bool request_nfc_link(void)
{
    if (!m_wifi_connected || m_nfc_request_in_progress)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "Cannot request NFC link - %s\r\n",
                        !m_wifi_connected ? "Wi-Fi not connected" : "Request already in progress");
        return false;
    }

    // Resolve the server hostname
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Requesting NFC link from server %s (HTTPS)\r\n", m_server_host_name);
    gethostbyname((uint8_t *)m_server_host_name);

    // Reset state variables for receiving response
    accumulated_length = 0;
    is_accumulating = true;
    receive_attempts = 0;
    memset(accumulated_response, 0, sizeof(accumulated_response));

    // Prepare the HTTP request
    memset(m_tcp_received_buffer, 0, sizeof(m_tcp_received_buffer));

    // Format the request using the template
    sprintf((char *)m_tcp_received_buffer, HTTP_REQUEST_TEMPLATE, MAIN_NFC_SERVER_NAME);

    // Log the request for debugging
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "HTTP Request to be sent:\r\n%s\r\n", m_tcp_received_buffer);

    m_nfc_request_in_progress = true;
    m_app_state = APP_STATE_NFC_REQUEST_PENDING;
    bsp_board_led_on(LED_NFC_ACTIVITY);

    return true;
}

/**
 * \brief Handle completion of NFC link request
 */
static void nfc_request_complete(bool success)
{
    m_nfc_request_in_progress = false;
    bsp_board_led_off(LED_NFC_ACTIVITY);

    if (success)
    {
        m_app_state = APP_STATE_NFC_REQUEST_DONE;
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC link request completed successfully\r\n");

        // Update the NFC tag with the new URL
        update_nfc_tag();
    }
    else
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "NFC link request failed\r\n");
    }
}

/**
 * \brief Callback function of IP address.
 *
 * \param[in] hostName Domain name.
 * \param[in] hostIp Server IP.
 *
 * \return None.
 */
static void resolve_cb(uint8_t *hostName, uint32_t hostIp)
{
    struct sockaddr_in addr_in;
    int ssl_cert_bypass = 1;  
    uint32_t u32Timeout = 10000;  // 10 second timeout

    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "\r\n%s IP address is %d.%d.%d.%d\r\n", hostName,
                    (int)IPV4_BYTE(hostIp, 0), (int)IPV4_BYTE(hostIp, 1),
                    (int)IPV4_BYTE(hostIp, 2), (int)IPV4_BYTE(hostIp, 3));

    addr_in.sin_family = AF_INET;
    addr_in.sin_port = _htons(MAIN_HOST_PORT);
    addr_in.sin_addr.s_addr = hostIp;

    /* Create secure socket using SSL */
    if (m_tcp_client_socket < 0)
    {
        /* Use SOCKET_FLAGS_SSL for HTTPS connections */
        m_tcp_client_socket = socket(AF_INET, SOCK_STREAM, SOCKET_FLAGS_SSL);
        
        /* Set options if socket created successfully */
        if (m_tcp_client_socket >= 0) {
            // Bypass SSL certificate verification
            setsockopt(m_tcp_client_socket, SOL_SSL_SOCKET, SO_SSL_BYPASS_X509_VERIF, &ssl_cert_bypass, sizeof(ssl_cert_bypass));
            
            // Try alternative socket option for timeout
            // Note: Using just the bypass may be enough to solve your connection issue
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Socket created with SSL, ID=%d (certificate verification bypassed)\r\n", m_tcp_client_socket);
        }
    }

    /* Check if socket was created successfully */
    if (m_tcp_client_socket == -1)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Socket error\r\n");
        nrf_cli_process(mp_curr_cli);
        close(m_tcp_client_socket);
        nfc_request_complete(false);
        return;
    }

    /* If success, connect to socket */
    int connect_result = connect(m_tcp_client_socket, (struct sockaddr *)&addr_in, sizeof(struct sockaddr_in));
    if (connect_result != SOCK_ERR_NO_ERROR)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Connect error! code(%d)\r\n", connect_result);

        /* Add detailed error description */
        switch (connect_result)
        {
        case SOCK_ERR_INVALID_ARG:
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Invalid argument\r\n");
            break;
        case SOCK_ERR_INVALID:
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Invalid socket\r\n");
            break;
        /* Add other error codes as needed */
        default:
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Unknown socket error\r\n");
            break;
        }

        nrf_cli_process(mp_curr_cli);
        close(m_tcp_client_socket);
        m_tcp_client_socket = -1;
        nfc_request_complete(false);
        return;
    }

    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Connecting to server using SSL (HTTPS)...\r\n");
    nrf_cli_process(mp_curr_cli);
}

/**
 * \brief Callback function of TCP client socket.
 *
 * \param[in] sock socket handler.
 * \param[in] u8Msg Type of Socket notification
 * \param[in] pvMsg A structure contains notification informations.
 *
 * \return None.
 */
static void socket_cb(SOCKET sock, uint8_t u8Msg, void *pvMsg)
{
    /* Check for socket event on TCP socket. */
    if (sock == m_tcp_client_socket)
    {
        switch (u8Msg)
        {
        case SOCKET_MSG_CONNECT:
        {
            tstrSocketConnectMsg *pstrConnect = (tstrSocketConnectMsg *)pvMsg;
            if (pstrConnect && pstrConnect->s8Error >= SOCK_ERR_NO_ERROR)
            {
                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Successfully connected to server (SSL handshake completed).\r\n");
                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Sending HTTP request for NFC link...\r\n");
                nrf_cli_process(mp_curr_cli);

                // Send HTTP request
                int send_result = send(m_tcp_client_socket, m_tcp_received_buffer, strlen((char *)m_tcp_received_buffer), 0);
                if (send_result < 0)
                {
                    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Failed to send HTTP request. Error: %d\r\n", send_result);
                    close(m_tcp_client_socket);
                    m_tcp_client_socket = -1;
                    nfc_request_complete(false);
                    return;
                }

                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Request sent successfully (%d bytes). Waiting for response...\r\n",
                                send_result);

                // Prepare to receive response
                memset(m_tcp_received_buffer, 0, sizeof(m_tcp_received_buffer));
                recv(m_tcp_client_socket, &m_tcp_received_buffer[0], MAIN_WIFI_M2M_BUFFER_SIZE, 0);
            }
            else
            {
                int error_code = (pstrConnect) ? pstrConnect->s8Error : -1;
                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "\r\nConnect error! code(%d)\r\n", error_code);

                // Provide more context for SSL/TLS errors
                if (error_code == SOCK_ERR_INVALID_ARG)
                {
                    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Invalid argument for SSL connection\r\n");
                }
                else if (error_code == -5)
                { // Custom code for SSL certificate verification error
                    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "SSL certificate verification failed\r\n");
                }

                nrf_cli_process(mp_curr_cli);
                close(m_tcp_client_socket);
                m_tcp_client_socket = -1;
                nfc_request_complete(false);
            }
        }
        break;

        case SOCKET_MSG_RECV:
        {
            tstrSocketRecvMsg *pstrRecv = (tstrSocketRecvMsg *)pvMsg;
            receive_attempts++;

            if (pstrRecv && pstrRecv->s16BufferSize > 0)
            {
                // Append this chunk to our accumulated response if there's room
                if (accumulated_length + pstrRecv->s16BufferSize < sizeof(accumulated_response))
                {
                    memcpy(accumulated_response + accumulated_length, pstrRecv->pu8Buffer, pstrRecv->s16BufferSize);
                    accumulated_length += pstrRecv->s16BufferSize;
                    accumulated_response[accumulated_length] = '\0'; // Ensure null termination
                }

                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Received %d bytes (total accumulated: %d, attempt: %d)\r\n",
                                pstrRecv->s16BufferSize, accumulated_length, receive_attempts);

                // Print a sample of the received data (first 50 bytes)
                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Sample data: ");
                int sample_length = pstrRecv->s16BufferSize > 50 ? 50 : pstrRecv->s16BufferSize;
                for (int i = 0; i < sample_length; i++)
                {
                    if (pstrRecv->pu8Buffer[i] >= 32 && pstrRecv->pu8Buffer[i] <= 126)
                    {
                        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "%c", pstrRecv->pu8Buffer[i]);
                    }
                    else
                    {
                        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, ".");
                    }
                }
                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "\r\n");

                nrf_cli_process(mp_curr_cli);

                // Continue receiving if we don't have complete data yet
                bool have_json = (strstr(accumulated_response, "{") != NULL && strstr(accumulated_response, "}") != NULL);
                bool should_continue = (!have_json && receive_attempts < MAX_RECEIVE_ATTEMPTS) ||
                                       (pstrRecv->u16RemainingSize > 0);

                if (should_continue)
                {
                    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Continuing to receive data...\r\n");
                    nrf_cli_process(mp_curr_cli);
                    recv(sock, &m_tcp_received_buffer[0], MAIN_WIFI_M2M_BUFFER_SIZE, 0);
                    return;
                }

                // Process the accumulated response
                bool success = process_nfc_response(accumulated_response, &m_current_nfc_link);

                nrf_cli_process(mp_curr_cli);

                close(m_tcp_client_socket);
                m_tcp_client_socket = -1;
                nfc_request_complete(success);
            }
            else
            {
                // Handle end of data or error
                if (pstrRecv)
                {
                    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "End of data received (remaining: %d)\r\n",
                                    pstrRecv->u16RemainingSize);

                    // If we got a zero-length response but have accumulated data, try to process it
                    if (accumulated_length > 0)
                    {
                        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Attempting to process accumulated data...\r\n");
                        bool success = process_nfc_response(accumulated_response, &m_current_nfc_link);
                        close(m_tcp_client_socket);
                        m_tcp_client_socket = -1;
                        nfc_request_complete(success);
                        return;
                    }
                }
                else
                {
                    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Socket receive error!\r\n");
                }

                // Even with no data, try to receive more if we don't have JSON yet and haven't reached max attempts
                bool have_json = (strstr(accumulated_response, "{") != NULL);
                if (!have_json && receive_attempts < MAX_RECEIVE_ATTEMPTS)
                {
                    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "No data received, but trying again (attempt %d/%d)...\r\n",
                                    receive_attempts, MAX_RECEIVE_ATTEMPTS);
                    nrf_cli_process(mp_curr_cli);
                    recv(sock, &m_tcp_received_buffer[0], MAIN_WIFI_M2M_BUFFER_SIZE, 0);
                    return;
                }

                nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Socket receive error or end of data\r\n");
                nrf_cli_process(mp_curr_cli);

                close(m_tcp_client_socket);
                m_tcp_client_socket = -1;
                nfc_request_complete(false);
            }
        }
        break;

        default:
            break;
        }
    }
}

/**
 * \brief Callback function for handling NFC events.
 */
static void nfc_callback(void *p_context, nfc_t2t_event_t event, const uint8_t *p_data, size_t data_length)
{
    (void)p_context;

    switch (event)
    {
    case NFC_T2T_EVENT_FIELD_ON:
        bsp_board_led_on(LED_NFC_ACTIVITY);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC field detected\r\n");
        nrf_cli_process(mp_curr_cli);
        break;

    case NFC_T2T_EVENT_FIELD_OFF:
        bsp_board_led_off(LED_NFC_ACTIVITY);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC field lost\r\n");
        nrf_cli_process(mp_curr_cli);
        break;

    default:
        break;
    }
}

/**
 * \brief Callback to get the Wi-Fi status update.
 *
 * \param[in] u8MsgType type of Wi-Fi notification. Possible types are:
 *  - [M2M_WIFI_RESP_CON_STATE_CHANGED](@ref M2M_WIFI_RESP_CON_STATE_CHANGED)
 *  - [M2M_WIFI_REQ_DHCP_CONF](@ref M2M_WIFI_REQ_DHCP_CONF)
 *  - [M2M_WIFI_RESP_CONN_INFO](@ref M2M_WIFI_RESP_CONN_INFO)
 * \param[in] pvMsg A pointer to a buffer containing the notification parameters
 * (if any). It should be casted to the correct data type corresponding to the
 * notification type.
 */
static void wifi_cb(uint8_t u8MsgType, void *pvMsg)
{
    switch (u8MsgType)
    {
    case M2M_WIFI_RESP_CON_STATE_CHANGED:
    {
        tstrM2mWifiStateChanged *pstrWifiState = (tstrM2mWifiStateChanged *)pvMsg;
        if (pstrWifiState->u8CurrState == M2M_WIFI_CONNECTED)
        {
            m2m_wifi_request_dhcp_client();
        }
        else if (pstrWifiState->u8CurrState == M2M_WIFI_DISCONNECTED)
        {
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "\r\nWi-Fi disconnected\r\n");
            nrf_cli_process(mp_curr_cli);

            m_wifi_connected = false;

            // Close any open socket
            if (m_tcp_client_socket >= 0)
            {
                close(m_tcp_client_socket);
                m_tcp_client_socket = -1;
            }

            bsp_board_led_off(LED_WIFI_CONNECTED);
            m_app_state = APP_STATE_WIFI_CONNECTING;

            // Attempt to reconnect
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Attempting to reconnect to Wi-Fi\r\n");
            m2m_wifi_connect(WIFI_SSID, strlen(WIFI_SSID), WIFI_SECURITY, WIFI_PASSWORD, M2M_WIFI_CH_ALL);
        }
        break;
    }

    case M2M_WIFI_REQ_DHCP_CONF:
    {
        uint8_t *pu8IPAddress = (uint8_t *)pvMsg;
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "\r\nWi-Fi connected\r\n");
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Wi-Fi IP is %u.%u.%u.%u\r\n",
                        pu8IPAddress[0], pu8IPAddress[1], pu8IPAddress[2], pu8IPAddress[3]);
        nrf_cli_process(mp_curr_cli);

        m_wifi_connected = true;
        m_app_state = APP_STATE_WIFI_CONNECTED;
        bsp_board_led_on(LED_WIFI_CONNECTED);

        // Initiate NFC link request automatically
        if (!m_nfc_request_in_progress)
        {
            request_nfc_link();
        }
        break;
    }

    case M2M_WIFI_RESP_CONN_INFO:
    {
        tstrM2MConnInfo *pstrConnInfo = (tstrM2MConnInfo *)pvMsg;
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "\r\nCONNECTED AP INFO\r\n");
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "SSID                : %s\r\n", pstrConnInfo->acSSID);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "SEC TYPE            : %s\r\n", strSecType[pstrConnInfo->u8SecType]);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Signal Strength     : %d\r\n", pstrConnInfo->s8RSSI);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "RF Channel          : %d\r\n", pstrConnInfo->u8CurrChannel);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Local IP Address    : %d.%d.%d.%d\r\n",
                        pstrConnInfo->au8IPAddr[0], pstrConnInfo->au8IPAddr[1], pstrConnInfo->au8IPAddr[2], pstrConnInfo->au8IPAddr[3]);
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "MAC Address         : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                        pstrConnInfo->au8MACAddress[0], pstrConnInfo->au8MACAddress[1], pstrConnInfo->au8MACAddress[2], pstrConnInfo->au8MACAddress[3], pstrConnInfo->au8MACAddress[4], pstrConnInfo->au8MACAddress[5]);
        nrf_cli_process(mp_curr_cli);
        break;
    }

    default:
        break;
    }
}

/**
 * \brief Timer handler for NFC link refresh.
 */
static void nfc_refresh_timer_handler(void *p_context)
{
    // Request a new NFC link if we're connected and not already in process
    if (m_wifi_connected && !m_nfc_request_in_progress)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Refresh timer: Requesting new NFC link\r\n");
        nrf_cli_process(mp_curr_cli);
        request_nfc_link();
    }
}

/**
 * \brief Initialize the application timers.
 */
static void timers_init(void)
{
    ret_code_t err_code;

    // Create timer for NFC link refresh
    err_code = app_timer_create(&m_nfc_refresh_timer, APP_TIMER_MODE_REPEATED, nfc_refresh_timer_handler);
    APP_ERROR_CHECK(err_code);
}

/**
 * \brief Start the periodic timer for NFC link refresh.
 */
static void start_nfc_refresh_timer(void)
{
    ret_code_t err_code;

    // Start timer with configured interval
    err_code = app_timer_start(m_nfc_refresh_timer, APP_TIMER_TICKS(NFC_LINK_REFRESH_INTERVAL_MS), NULL);
    APP_ERROR_CHECK(err_code);

    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC refresh timer started (%d ms interval)\r\n", NFC_LINK_REFRESH_INTERVAL_MS);
    nrf_cli_process(mp_curr_cli);
}

/**
 * @brief Initialize NFC functionality
 */
static void nfc_init(void)
{
    ret_code_t err_code;

    // Initialize NFC
    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Initializing NFC Tag emulation...\r\n");

    // Setup NFC callbacks
    err_code = nfc_t2t_setup(nfc_callback, NULL);
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Error setting up NFC (%d)\r\n", err_code);
        m_app_state = APP_STATE_ERROR;
        bsp_board_led_on(LED_ERROR);
        return;
    }

    // Set a default URL until we get one from the API
    const uint8_t default_url[] = {'m', 'a', 'k', 'e', 'r', 'd', 'i', 'a', 'r', 'y', '.', 'c', 'o', 'm'};
    uint32_t len = sizeof(m_ndef_msg_buf);

    // Encode URI message
    err_code = nfc_uri_msg_encode(NFC_URI_HTTP_WWW,
                                  default_url,
                                  sizeof(default_url),
                                  m_ndef_msg_buf,
                                  &len);
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Error encoding URI message (%d)\r\n", err_code);
        return;
    }

    // Set created message as the NFC payload
    err_code = nfc_t2t_payload_set(m_ndef_msg_buf, len);
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Error setting NFC payload (%d)\r\n", err_code);
        return;
    }

    // Start sensing NFC field
    err_code = nfc_t2t_emulation_start();
    if (err_code != NRF_SUCCESS)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Error starting NFC emulation (%d)\r\n", err_code);
        return;
    }

    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC Tag emulation started with default URL\r\n");
    nrf_cli_process(mp_curr_cli);
}

/**
 * @brief Function for initializing LEDs.
 */
static void leds_init(void)
{
    // All LEDs off initially
    bsp_board_led_off(LED_WIFI_CONNECTED);
    bsp_board_led_off(LED_NFC_ACTIVITY);
    bsp_board_led_off(LED_ERROR);
}

/**
 * @brief Function for Wi-Fi module initialization.
 *
 * This function initializes the Wi-Fi module and starts the connection process.
 * It should be called from the main application before initializing NFC.
 */
void wifi_setup(void)
{
    tstrWifiInitParam param;
    int8_t ret;
    uint8_t u8IsMacAddrValid;

    // Initialize LEDs
    leds_init();

    // Initialize timers
    timers_init();

    if (mp_curr_cli != NULL)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Initializing Wi-Fi module...\r\n");
        nrf_cli_process(mp_curr_cli);
    }

    // Initialize the BSP
    nm_bsp_init();

    // Initialize Wi-Fi parameters structure
    memset((uint8_t *)&param, 0, sizeof(tstrWifiInitParam));

    // Initialize Wi-Fi driver with data and status callbacks
    param.pfAppWifiCb = wifi_cb;
    ret = m2m_wifi_init(&param);
    if (M2M_SUCCESS != ret)
    {
        if (mp_curr_cli != NULL)
        {
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_ERROR, "Failed to initialize Wi-Fi driver (%d)\r\n", ret);
            nrf_cli_process(mp_curr_cli);
        }
        m_app_state = APP_STATE_ERROR;
        bsp_board_led_on(LED_ERROR);
        return;
    }

    // Configure power save mode AFTER initialization
    ret = m2m_wifi_set_sleep_mode(M2M_NO_PS, 1);
    if (M2M_SUCCESS != ret)
    {
        if (mp_curr_cli != NULL)
        {
            nrf_cli_fprintf(mp_curr_cli, NRF_CLI_WARNING, "Failed to set power mode (%d)\r\n", ret);
            nrf_cli_process(mp_curr_cli);
        }
    }

    // Get MAC Address from OTP
    m2m_wifi_get_otp_mac_address(m_mac_addr, &u8IsMacAddrValid);
    if (!u8IsMacAddrValid)
    {
        // Cannot found MAC Address from OTP. Set user define MAC address
        m_user_define_mac_address[3] = NRF_FICR->DEVICEID[0] & 0xFF;
        m_user_define_mac_address[4] = (NRF_FICR->DEVICEID[0] >> 8) & 0xFF;
        m_user_define_mac_address[5] = (NRF_FICR->DEVICEID[0] >> 16) & 0xFF;

        m2m_wifi_set_mac_address((uint8_t *)m_user_define_mac_address);
    }

    // Initialize socket API
    socketInit();
    registerSocketCallback(socket_cb, resolve_cb);

    if (mp_curr_cli != NULL)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Wi-Fi module initialized successfully\r\n");
        nrf_cli_process(mp_curr_cli);
    }

    // Set state to connecting
    m_app_state = APP_STATE_WIFI_CONNECTING;

    // Connect to configured AP
    if (mp_curr_cli != NULL)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "Connecting to Wi-Fi SSID: %s\r\n", WIFI_SSID);
        nrf_cli_process(mp_curr_cli);
    }
    m2m_wifi_connect(WIFI_SSID, strlen(WIFI_SSID), WIFI_SECURITY, WIFI_PASSWORD, M2M_WIFI_CH_ALL);

    // Initialize NFC after WiFi is set up
    nfc_client_init();
}

/**
 * @brief Function for processing the Wi-Fi events.
 */
void wifi_process(void)
{
    while (m2m_wifi_handle_events(NULL) != M2M_SUCCESS)
    {
        // No implementation needed
    }

    // If we're in error state, blink the error LED
    if (m_app_state == APP_STATE_ERROR)
    {
        static uint32_t last_blink_time = 0;
        uint32_t current_time = app_timer_cnt_get();

        if (app_timer_cnt_diff_compute(current_time, last_blink_time) >= APP_TIMER_TICKS(500))
        {
            bsp_board_led_invert(LED_ERROR);
            last_blink_time = current_time;
        }
    }
}

/**
 * @brief Set CLI context for output
 *
 * This function should be called before wifi_setup to ensure
 * console output is properly directed.
 */
void nfc_client_set_cli(nrf_cli_t const *p_cli)
{
    mp_curr_cli = p_cli;

    if (mp_curr_cli)
    {
        nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC Client: CLI context set\r\n");
        nrf_cli_process(mp_curr_cli);
    }
}

/**
 * @brief Initialize NFC portion of the application
 *
 * This function initializes the NFC functionality and should be called
 * after the WiFi setup is complete.
 */
void nfc_client_init(void)
{
    // Initialize NFC
    nfc_init();

    // Start the refresh timer
    start_nfc_refresh_timer();

    nrf_cli_fprintf(mp_curr_cli, NRF_CLI_NORMAL, "NFC functionality initialized\r\n");
    nrf_cli_process(mp_curr_cli);
}