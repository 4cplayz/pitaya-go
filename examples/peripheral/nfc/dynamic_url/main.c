/**
 * Copyright (c) 2014 - 2018, Nordic Semiconductor ASA
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/** @file
 *
 * @defgroup nfc_url_record_example_main main.c
 * @{
 * @ingroup nfc_url_record_example
 * @brief NFC URL record example application main file.
 *
 */
#include <stdint.h>
#include <string.h>
#include "nrf_drv_clock.h"
#include "app_scheduler.h"
#include "app_timer.h"
#include "nfc_t2t_lib.h"
#include "nfc_uri_msg.h"
#include "boards.h"
#include "app_error.h"
#include "bsp.h"
#include "sdk_config.h"

#define SCHED_MAX_EVENT_DATA_SIZE       APP_TIMER_SCHED_EVENT_DATA_SIZE             /**< Maximum size of scheduler events. */
#define SCHED_QUEUE_SIZE                60                                          /**< Maximum number of events in the scheduler queue. */

#define BTN_USER                 0

#define BTN_USER_KEY_RELEASE     (bsp_event_t)(BSP_EVENT_KEY_LAST + 1)
#define BTN_USER_KEY_LONG_PUSH   (bsp_event_t)(BSP_EVENT_KEY_LAST + 2)

// Define an array of URLs
static const uint8_t url_1[] = {'m', 'a', 'k', 'e', 'r', 'd', 'i', 'a', 'r', 'y', '.', 'c', 'o', 'm'};
static const uint8_t url_2[] = {'n', 'o', 'r', 'd', 'i', 'c', 's', 'e', 'm', 'i', '.', 'c', 'o', 'm'};
static const uint8_t url_3[] = {'g', 'i', 't', 'h', 'u', 'b', '.', 'c', 'o', 'm'};

// Define the message buffer
uint8_t m_ndef_msg_buf[256];

// Structure to hold URL data and its length
typedef struct
{
    const uint8_t *data;
    size_t length;
} url_info_t;

static uint32_t button_press_count = 0;  // Counter for button presses (0-2 internally, displays as 1-3)

// Forward declarations
static void update_nfc_payload(void);

// Function to get URL based on index
static url_info_t get_url(uint8_t url_index)
{
    url_info_t url_info = {0};

    switch (url_index)
    {
    case 1:
        url_info.data = url_1;
        url_info.length = sizeof(url_1);
        break;

    case 2:
        url_info.data = url_2;
        url_info.length = sizeof(url_2);
        break;

    case 3:
        url_info.data = url_3;
        url_info.length = sizeof(url_3);
        break;

    default:
        // Default to the first URL if invalid index
        url_info.data = url_1;
        url_info.length = sizeof(url_1);
        break;
    }

    return url_info;
}

// Update NFC payload function - scheduled to run outside interrupt context
static void update_nfc_task_handler(void * p_context, uint16_t event_size)
{
    // Run NFC update outside of button interrupt context
    update_nfc_payload();
}

// Function to update NFC URL based on button press count
static void update_nfc_payload(void)
{
    ret_code_t err_code;
    uint32_t len = sizeof(m_ndef_msg_buf);

    // Get URL based on current button count (adding 1 to convert from 0-2 to 1-3)
    url_info_t url_info = get_url(button_press_count + 1);

    // Encode the new URI message
    err_code = nfc_uri_msg_encode(NFC_URI_HTTP_WWW,
                                 url_info.data,
                                 url_info.length,
                                 m_ndef_msg_buf,
                                 &len);
    if (err_code != NRF_SUCCESS)
    {
        return;
    }

    // Update the NFC payload
    err_code = nfc_t2t_payload_set(m_ndef_msg_buf, len);
    if (err_code != NRF_SUCCESS)
    {
        return;
    }
}

// Button event callback function
static void bsp_event_callback(bsp_event_t ev)
{
    switch ((unsigned int)ev)
    {
        case CONCAT_2(BSP_EVENT_KEY_, BTN_USER):
        {
            // Increment counter on button press
            button_press_count = (button_press_count + 1) % 3;  // Cycles through 0, 1, 2 internally
            bsp_board_led_on(LED_R_IDX);
            
            // Schedule the NFC update to avoid interrupt context conflicts
            app_sched_event_put(NULL, 0, update_nfc_task_handler);
            break;
        }
        
        case BTN_USER_KEY_RELEASE:
        {
            bsp_board_led_off(LED_R_IDX);
            break;
        }
        
        default:
            return; // no implementation needed
    }
}

/**
 * @brief Callback function for handling NFC events.
 */
static void nfc_callback(void *p_context, nfc_t2t_event_t event, const uint8_t *p_data, size_t data_length)
{
    (void)p_context;

    switch (event)
    {
    case NFC_T2T_EVENT_FIELD_ON:
        bsp_board_led_on(LED_B_IDX);
        break;

    case NFC_T2T_EVENT_FIELD_OFF:
        bsp_board_led_off(LED_B_IDX);
        break;

    default:
        break;
    }
}

static void init_bsp(void)
{
    ret_code_t ret;
    ret = bsp_init(BSP_INIT_BUTTONS, bsp_event_callback);
    APP_ERROR_CHECK(ret);
    
    // Register custom button events
    UNUSED_RETURN_VALUE(bsp_event_to_button_action_assign(BTN_USER,
                                                         BSP_BUTTON_ACTION_RELEASE,
                                                         BTN_USER_KEY_RELEASE));

    UNUSED_RETURN_VALUE(bsp_event_to_button_action_assign(BTN_USER,
                                                         BSP_BUTTON_ACTION_LONG_PUSH,
                                                         BTN_USER_KEY_LONG_PUSH));
    
    /* Configure LEDs */
    bsp_board_init(BSP_INIT_LEDS);
}

/**
 * @brief Function for application main entry.
 */
int main(void)
{
    // Initialize scheduler first (just like in the working button example)
    APP_SCHED_INIT(SCHED_MAX_EVENT_DATA_SIZE, SCHED_QUEUE_SIZE);

    // Initialize clock
    APP_ERROR_CHECK(nrf_drv_clock_init());
    
    nrf_drv_clock_lfclk_request(NULL);
    
    while(!nrf_drv_clock_lfclk_is_running())
    {
        /* Just waiting */
    }
    
    // Initialize timer
    APP_ERROR_CHECK(app_timer_init());

    /* Configure board */
    init_bsp();
    
    // Set up NFC
    ret_code_t err_code = nfc_t2t_setup(nfc_callback, NULL);
    APP_ERROR_CHECK(err_code);

    // Get the initial URL (start with URL 1)
    url_info_t url_info = get_url(1);
    
    // Provide information about available buffer size to encoding function
    uint32_t len = sizeof(m_ndef_msg_buf);

    // Encode URI message into buffer
    err_code = nfc_uri_msg_encode(NFC_URI_HTTP_WWW,
                                 url_info.data,
                                 url_info.length,
                                 m_ndef_msg_buf,
                                 &len);
    APP_ERROR_CHECK(err_code);

    // Set created message as the NFC payload
    err_code = nfc_t2t_payload_set(m_ndef_msg_buf, len);
    APP_ERROR_CHECK(err_code);

    // Start sensing NFC field
    err_code = nfc_t2t_emulation_start();
    APP_ERROR_CHECK(err_code);
    
    // Turn on green LED to indicate system is running
    bsp_board_led_on(LED_G_IDX);

    // Main loop - exactly like the working button example
    while (1)
    {
        app_sched_execute();
        
        // Wait for an event.
        __WFE();
        // Clear the internal event register.
        __SEV();
        __WFE();
    }
}

/** @} */