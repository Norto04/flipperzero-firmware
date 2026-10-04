#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Internal ATT payload helpers, independent of the RTOS and controller. */
static inline uint16_t ble_central_u16(const uint8_t* bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static inline bool ble_central_att_records_valid(
    size_t payload_len,
    uint8_t record_len,
    uint8_t short_len,
    uint8_t long_len) {
    return (record_len == short_len || record_len == long_len) && payload_len % record_len == 0;
}

/* Attribute Not Found is successful termination only for discovery. */
static inline bool ble_central_discovery_status_ok(uint8_t status, bool discovery) {
    return status == 0 || (discovery && status == 0x0A);
}

static inline bool ble_central_connection_params_valid(
    uint16_t min_interval,
    uint16_t max_interval,
    uint16_t latency,
    uint16_t timeout) {
    return min_interval >= 24 && min_interval <= max_interval && max_interval <= 3200 &&
           latency <= 499 && timeout >= 10 && timeout <= 3200 &&
           (uint32_t)timeout * 8 > (uint32_t)(latency + 1) * max_interval * 2;
}

/* One legacy advertising report: type, address type, address[6], data length,
 * data, RSSI. Return its length or zero for a truncated/invalid report. */
static inline size_t ble_central_advertisement_size(const uint8_t* data, size_t len) {
    if(len < 10 || data[8] > 31 || len < (size_t)data[8] + 10) return 0;
    return (size_t)data[8] + 10;
}
