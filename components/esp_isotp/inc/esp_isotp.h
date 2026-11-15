/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
/**
 * @file esp_isotp.h
 * @brief ISO-TP (ISO 15765-2) Transport Protocol Implementation
 *
 * ISO-TP enables transmission of data larger than 8 bytes over TWAI networks
 * through automatic fragmentation and reassembly.
 *
 * ## How it Works
 *
 * **Small packets (≤7 bytes)**: Sent in a single TWAI frame immediately.
 * **Large packets (>7 bytes)**: Split into multiple frames - first frame sent immediately,
 * remaining frames sent during esp_isotp_poll() calls.
 *
 */

#include <stdbool.h>
#include "esp_err.h"
#include "esp_twai.h"
#include "esp_twai_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ISO-TP link handle
 */
typedef struct esp_isotp_link_t *esp_isotp_handle_t;

/**
 * @brief ISO-TP receive callback function type
 *
 * Called when a complete message has been received successfully.
 *
 * @warning This callback executes in the same context as isotp_on_can_message(),
 *          which is typically ISR context. Keep callback execution time minimal
 *          and avoid blocking operations.
 * @note The data pointer is only valid during the callback execution.
 *       Copy data immediately if needed beyond the callback scope.
 *
 * @param[in] handle ISO-TP handle that received the message
 * @param[in] data Pointer to received data (valid only during callback)
 * @param[in] size Size of received data in bytes
 * @param[in] user_arg User argument provided during configuration
 */
typedef void (*esp_isotp_rx_callback_t)(esp_isotp_handle_t handle, const uint8_t *data, uint32_t size, void *user_arg);

/**
 * @brief ISO-TP transmit callback function type
 *
 * Called when a complete message has been transmitted successfully.
 *
 * @note Execution context depends on message type:
 *       - Single-frame: Called immediately from isotp_send() in caller's context
 *       - Multi-frame: Called from isotp_poll() in task context
 * @note Keep callback execution time minimal to avoid affecting system performance.
 *
 * @param[in] handle ISO-TP handle that transmitted the message
 * @param[in] tx_size Size of transmitted data in bytes
 * @param[in] user_arg User argument provided during configuration
 */
typedef void (*esp_isotp_tx_callback_t)(esp_isotp_handle_t handle, uint32_t tx_size, void *user_arg);

/**
 * @brief CAN frame transmit callback for externally managed transports
 *
 * Use this when another component owns the CAN controller and `esp_isotp`
 * should forward generated CAN frames through that component instead of
 * transmitting directly with a TWAI node.
 *
 * @param[in] arbitration_id TWAI identifier (11-bit or 29-bit)
 * @param[in] data Pointer to frame payload
 * @param[in] size Payload length in bytes (0-8)
 * @param[in] user_arg User argument provided during configuration
 * @return
 *     - ESP_OK on success
 *     - Any non-ESP_OK value on transmit failure
 */
typedef esp_err_t (*esp_isotp_can_tx_callback_t)(uint32_t arbitration_id, const uint8_t *data, uint8_t size, void *user_arg);

/**
 * @brief Configuration structure for creating a new ISO-TP link
 */
typedef struct {
    uint32_t tx_id;                  /*!< TWAI ID for transmitting ISO-TP frames (11-bit or 29-bit, auto-detected from value) */
    uint32_t rx_id;                  /*!< TWAI ID for receiving ISO-TP frames (11-bit or 29-bit, auto-detected from value) */
    uint32_t tx_buffer_size;         /*!< Size of the transmit buffer (max message size to send) */
    uint32_t rx_buffer_size;         /*!< Size of the receive buffer (max message size to receive) */
    uint32_t tx_frame_pool_size;     /*!< Size of TX frame pool */
    bool tx_use_padding;             /*!< Pad generated CAN frames to 8 bytes when true */
    uint8_t tx_padding_value;        /*!< Byte value used for transmit padding */

    esp_isotp_rx_callback_t rx_callback; /*!< Receive completion callback (NULL for polling mode) */
    esp_isotp_tx_callback_t tx_callback; /*!< Transmit completion callback (NULL to disable) */
    void *callback_arg;               /*!< User argument passed to callbacks */
    bool externally_managed_twai;     /*!< When true, caller owns TWAI callbacks/lifecycle and feeds RX frames manually */
    esp_isotp_can_tx_callback_t can_tx_callback; /*!< Optional CAN TX callback for externally managed transports */
    void *can_tx_callback_arg;        /*!< User argument passed to can_tx_callback */
    bool flow_override;               /*!< When true, flow_block_size / flow_st_min_us replace the Kconfig defaults */
    uint8_t flow_block_size;          /*!< Receiver BS. 0 = sender streams the rest of the PDU */
    uint32_t flow_st_min_us;          /*!< Receiver STmin in microseconds. 0 = no separation */
} esp_isotp_config_t;

/**
 * @brief Create a new ISO-TP transport bound to a TWAI node.
 *
 * Allocates internal buffers, creates TX frame pool, and binds to the provided
 * TWAI node.
 *
 * By default this function registers TWAI callbacks and enables the node.
 * When `config->externally_managed_twai` is true, the caller keeps ownership of
 * TWAI callbacks and node lifecycle and must pass matching RX frames to
 * esp_isotp_feed_can_frame().
 *
 * @param twai_node TWAI node handle to bind. May be NULL only when
 *        `config->externally_managed_twai` is true and
 *        `config->can_tx_callback` is provided.
 * @param config Transport configuration.
 * @param[out] out_handle Returned ISO-TP transport handle.
 * @return esp_err_t
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_ARG for invalid parameters
 *  - ESP_ERR_INVALID_SIZE for invalid buffer sizes
 *  - ESP_ERR_NO_MEM when allocation fails
 *  - Other error codes from TWAI functions
 */
esp_err_t esp_isotp_new_transport(twai_node_handle_t twai_node, const esp_isotp_config_t *config, esp_isotp_handle_t *out_handle);

/** Replace the flow control this transport sends as receiver. Safe between PDUs. */
esp_err_t esp_isotp_set_flow_control(esp_isotp_handle_t handle, uint8_t block_size, uint32_t st_min_us);

/**
 * @brief Feed a CAN frame into an externally managed ISO-TP transport.
 *
 * Use this when the TWAI node is shared with other tasks/components and the
 * caller owns CAN receive dispatch. Frames with non-matching IDs are ignored.
 *
 * @note Safe to call from ISR context if the provided frame buffer remains
 *       valid for the duration of the call.
 *
 * @param handle ISO-TP handle
 * @param frame Received TWAI frame to process
 * @return
 *     - ESP_OK: Frame accepted (or ignored because the ID does not match)
 *     - ESP_ERR_INVALID_ARG: Invalid parameters or frame contents
 */
esp_err_t esp_isotp_feed_can_frame(esp_isotp_handle_t handle, const twai_frame_t *frame);

/**
 * @brief Send data over an ISO-TP link (non-blocking, ISR-safe)
 *
 * Immediately sends first/single frame and returns. For multi-frame messages,
 * remaining frames are sent during subsequent esp_isotp_poll() calls.
 *
 * @note This function is ISR-safe and can be called from interrupt context.
 * @note TX completion callback timing:
 *       - Single-frame: Called immediately from isotp_send()
 *       - Multi-frame: Called from isotp_poll() when last frame is sent
 *
 * @param handle ISO-TP handle
 * @param data Data to send
 * @param size Data length in bytes
 * @return
 *     - ESP_OK: Send initiated successfully
 *     - ESP_ERR_NOT_FINISHED: Previous send still in progress
 *     - ESP_ERR_NO_MEM: Data too large for buffer or no space available
 *     - ESP_ERR_INVALID_SIZE: Invalid data size
 *     - ESP_ERR_TIMEOUT: Send operation timed out
 *     - ESP_ERR_INVALID_ARG: Invalid parameters
 *     - ESP_FAIL: Other send errors
 */
esp_err_t esp_isotp_send(esp_isotp_handle_t handle, const uint8_t *data, uint32_t size);

/**
 * @brief Send data over an ISO-TP link with specified TWAI ID (non-blocking, ISR-safe)
 *
 * Similar to esp_isotp_send(), but allows specifying a different TWAI ID for transmission.
 * This function is primarily used for functional addressing where multiple nodes
 * may respond to the same request.
 *
 * @note This function is ISR-safe and can be called from interrupt context.
 * @note TX completion callback timing:
 *       - Single-frame: Called immediately from isotp_send_with_id()
 *       - Multi-frame: Called from isotp_poll() when last frame is sent
 *
 * @param handle ISO-TP handle
 * @param id TWAI identifier to use for transmission (overrides configured tx_id)
 * @param data Data to send
 * @param size Data length in bytes
 * @return
 *     - ESP_OK: Send initiated successfully
 *     - ESP_ERR_NOT_FINISHED: Previous send still in progress
 *     - ESP_ERR_NO_MEM: Data too large for buffer or no space available
 *     - ESP_ERR_INVALID_SIZE: Invalid data size
 *     - ESP_ERR_TIMEOUT: Send operation timed out
 *     - ESP_ERR_INVALID_ARG: Invalid parameters or ID exceeds maximum value
 *     - ESP_FAIL: Other send errors
 */
esp_err_t esp_isotp_send_with_id(esp_isotp_handle_t handle, uint32_t id, const uint8_t *data, uint32_t size);

/**
 * @brief Extract a complete received message (non-blocking, task context only)
 *
 * This function only extracts data that has already been assembled by esp_isotp_poll().
 * It does NOT process incoming TWAI frames - that happens in esp_isotp_poll().
 *
 * Process: TWAI frames → esp_isotp_poll() assembles → esp_isotp_receive() extracts
 *
 * @warning This function is NOT ISR-safe and must only be called from task context.
 * @note When using callback mode (rx_callback != NULL), received messages are
 *       automatically delivered via callback. This function should only be used
 *       in polling mode (rx_callback == NULL).
 *
 * @param handle ISO-TP handle
 * @param data Buffer to store received data
 * @param size Buffer size in bytes
 * @param[out] received_size Actual received data length
 * @return
 *     - ESP_OK: Complete message extracted and internal buffer cleared
 *     - ESP_ERR_NOT_FOUND: No complete message ready for extraction
 *     - ESP_ERR_INVALID_SIZE: Receive buffer overflow or invalid size
 *     - ESP_ERR_INVALID_RESPONSE: Invalid sequence number or protocol error
 *     - ESP_ERR_TIMEOUT: Receive operation timed out
 *     - ESP_ERR_INVALID_ARG: Invalid parameters
 *     - ESP_FAIL: Other receive errors
 */
esp_err_t esp_isotp_receive(esp_isotp_handle_t handle, uint8_t *data, uint32_t size, uint32_t *received_size);

/**
 * @brief Poll the ISO-TP link to process messages (CRITICAL - call regularly, task context only)
 *
 * This function drives the ISO-TP state machine. Call every 1-10ms for proper operation.
 *
 * What it does:
 * - Sends remaining frames for multi-frame messages
 * - Processes incoming TWAI frames and assembles complete messages
 * - Handles flow control and timeouts
 * - Updates internal state machine
 * - Triggers TX completion callbacks for multi-frame messages
 *
 * Without regular polling: multi-frame sends will stall and receives won't complete.
 *
 * @warning This function is NOT ISR-safe and must only be called from task context.
 * @note TX completion callbacks for multi-frame messages are triggered from this function.
 *
 * @param handle ISO-TP handle
 * @return
 *     - ESP_OK: Processing successful
 *     - ESP_ERR_INVALID_ARG: Invalid parameters
 */
esp_err_t esp_isotp_poll(esp_isotp_handle_t handle);

/**
 * @brief Delete an ISO-TP link
 *
 * @param handle The handle of the ISO-TP link to delete
 * @return
 *     - ESP_OK: Success (or TWAI disable warning logged)
 *     - ESP_ERR_INVALID_ARG: Invalid argument
 *     - Other ESP error codes: TWAI node disable failed
 */
esp_err_t esp_isotp_delete(esp_isotp_handle_t handle);

#ifdef __cplusplus
}
#endif
