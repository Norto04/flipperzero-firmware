#pragma once

#include <furi_ble/profile_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An exclusive BLE central profile, using the Full radio stack.
 * Start with bt_profile_start(bt, ble_profile_central, &params).
 * Restore the serial profile with bt_profile_restore_default() before exiting.
 * This profile does not advertise or pair; it supports one unencrypted peer.
 */
extern const FuriHalBleProfileTemplate* const ble_profile_central;

typedef enum {
    BleCentralEventAdvertisement,
    BleCentralEventScanStopped,
    BleCentralEventConnected,
    BleCentralEventConnectFailed,
    BleCentralEventDisconnected,
    BleCentralEventService,
    BleCentralEventCharacteristic,
    BleCentralEventDescriptor,
    BleCentralEventRead,
    BleCentralEventNotification,
    BleCentralEventProcedureComplete,
    BleCentralEventError,
    BleCentralEventConnectionParameters,
} BleCentralEventType;

typedef enum {
    BleCentralProcedureNone,
    BleCentralProcedureServices,
    BleCentralProcedureCharacteristics,
    BleCentralProcedureDescriptors,
    BleCentralProcedureRead,
    BleCentralProcedureWrite,
    BleCentralProcedureSubscribe,
} BleCentralProcedure;

typedef struct {
    BleCentralEventType type;
    BleCentralProcedure procedure;
    uint16_t connection_handle; /**< 0xFFFF when not connected; handle 0 is valid. */
    uint8_t status; /**< HCI/ATT status, or disconnect reason. */
    uint16_t handle;
    uint16_t end_handle;
    uint16_t value_handle;
    uint8_t properties;
    uint8_t uuid_len; /**< 2 or 16, UUID bytes in ATT little-endian order. */
    uint8_t uuid[16];
    uint8_t address_type;
    uint8_t address[6]; /**< HCI little-endian order. */
    int8_t rssi;
    uint8_t identifier;
    GapConnectionParamsRequest connection_params;
    const uint8_t* data;
    uint16_t data_len;
} BleCentralEvent;

/** Called on the BLE event thread. Copy event/data into an application queue.
 * Never issue BLE commands, change profiles, block, or unregister from here.
 * Data is borrowed and is valid only for the duration of this callback.
 */
typedef void (*BleCentralCallback)(const BleCentralEvent* event, void* context);

typedef struct {
    BleCentralCallback callback;
    void* context;
} BleCentralParams;

/** Operations must be serialized by the caller on an application thread.
 * true means accepted by the controller, not completed successfully.
 * Wait for ScanStopped before connecting, and ProcedureComplete before the
 * next ATT operation. Stop using the instance before restoring the profile.
 */
bool ble_central_scan_start(FuriHalBleProfileBase* profile);
bool ble_central_scan_stop(FuriHalBleProfileBase* profile);
bool ble_central_connect(
    FuriHalBleProfileBase* profile,
    uint8_t address_type,
    const uint8_t address[6]);
bool ble_central_cancel_connect(FuriHalBleProfileBase* profile);
bool ble_central_disconnect(FuriHalBleProfileBase* profile);
/** Reply to a ConnectionParameters event from the application thread.
 * Accepts valid requests with intervals >=30 ms; rejects others to preserve
 * the controller's flash-access timing requirements.
 */
bool ble_central_respond_connection_parameters(
    FuriHalBleProfileBase* profile,
    const BleCentralEvent* event);
bool ble_central_discover_services(FuriHalBleProfileBase* profile);
bool ble_central_discover_characteristics(
    FuriHalBleProfileBase* profile,
    uint16_t start_handle,
    uint16_t end_handle);
bool ble_central_discover_descriptors(
    FuriHalBleProfileBase* profile,
    uint16_t start_handle,
    uint16_t end_handle);
bool ble_central_read(FuriHalBleProfileBase* profile, uint16_t value_handle);
bool ble_central_write(
    FuriHalBleProfileBase* profile,
    uint16_t value_handle,
    const uint8_t* data,
    uint8_t data_len);
/** Explicit CCCD handle, discovered through descriptor discovery.
 * Do not assume the CCCD is adjacent to the characteristic value.
 */
bool ble_central_subscribe(FuriHalBleProfileBase* profile, uint16_t cccd_handle, bool enable);

#ifdef __cplusplus
}
#endif
