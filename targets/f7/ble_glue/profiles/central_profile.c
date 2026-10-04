#include "central_profile.h"
#include <interface/patterns/ble_thread/tl/tl.h>
#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <ble/ble.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/central_packet_i.h>

#define CENTRAL_INVALID_HANDLE 0xFFFF
#define CENTRAL_PAYLOAD_MAX    20

typedef enum {
    CentralIdle,
    CentralScanning,
    CentralScanStopping,
    CentralConnecting,
    CentralCancelling,
    CentralConnected,
    CentralDisconnecting,
} CentralState;

typedef struct {
    FuriHalBleProfileBase base;
    GapSvcEventHandler* handler;
    FuriMutex* mutex;
    BleCentralParams params;
    CentralState state;
    uint16_t connection_handle;
    BleCentralProcedure pending;
    uint16_t pending_handle;
    uint8_t att_error;
    bool att_failed;
} BleProfileCentral;

static BleProfileCentral* central_get(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == ble_profile_central);
    return (BleProfileCentral*)base;
}

static void central_lock(BleProfileCentral* profile) {
    furi_check(furi_mutex_acquire(profile->mutex, FuriWaitForever) == FuriStatusOk);
}

static void central_unlock(BleProfileCentral* profile) {
    furi_check(furi_mutex_release(profile->mutex) == FuriStatusOk);
}

static bool central_is_discovery(BleCentralProcedure procedure) {
    return procedure == BleCentralProcedureServices ||
           procedure == BleCentralProcedureCharacteristics ||
           procedure == BleCentralProcedureDescriptors;
}

static void central_emit(BleProfileCentral* profile, const BleCentralEvent* event) {
    if(profile->params.callback) profile->params.callback(event, profile->params.context);
}

static void central_invalid_packet(BleProfileCentral* profile) {
    central_lock(profile);
    profile->att_error = BLE_STATUS_INVALID_PARAMS;
    BleCentralEvent event = {
        .type = BleCentralEventError,
        .connection_handle = profile->connection_handle,
        .procedure = profile->pending,
        .status = BLE_STATUS_INVALID_PARAMS,
    };
    central_unlock(profile);
    central_emit(profile, &event);
}

static void central_advertisements(BleProfileCentral* profile, const uint8_t* data, size_t len) {
    if(!len) return;
    uint8_t count = *data++;
    len--;
    for(uint8_t i = 0; i < count; i++) {
        size_t size = ble_central_advertisement_size(data, len);
        if(!size) {
            central_invalid_packet(profile);
            return;
        }
        BleCentralEvent event = {
            .type = BleCentralEventAdvertisement,
            .connection_handle = CENTRAL_INVALID_HANDLE,
            .address_type = data[1],
            .data = data + 9,
            .data_len = data[8],
            .rssi = (int8_t)data[size - 1],
        };
        memcpy(event.address, data + 2, sizeof(event.address));
        central_emit(profile, &event);
        data += size;
        len -= size;
    }
}

/* Discovery results are streamed, avoiding fixed-size arrays/truncated trees. */
static void central_records(
    BleProfileCentral* profile,
    BleCentralEventType type,
    const uint8_t* data,
    size_t len,
    uint8_t record_len) {
    uint8_t short_len = type == BleCentralEventService        ? 6 :
                        type == BleCentralEventCharacteristic ? 7 :
                                                                4;
    if(!ble_central_att_records_valid(len, record_len, short_len, short_len + 14)) {
        central_invalid_packet(profile);
        return;
    }
    for(size_t offset = 0; offset < len; offset += record_len) {
        const uint8_t* record = data + offset;
        BleCentralEvent event = {
            .type = type,
            .connection_handle = profile->connection_handle,
            .handle = ble_central_u16(record),
            .uuid_len = record_len - short_len + 2,
        };
        uint8_t uuid_offset;
        if(type == BleCentralEventService) {
            event.end_handle = ble_central_u16(record + 2);
            uuid_offset = 4;
        } else if(type == BleCentralEventCharacteristic) {
            event.properties = record[2];
            event.value_handle = ble_central_u16(record + 3);
            uuid_offset = 5;
        } else {
            uuid_offset = 2;
        }
        memcpy(event.uuid, record + uuid_offset, event.uuid_len);
        central_emit(profile, &event);
    }
}

static BleEventAckStatus central_event_handler(void* packet, void* context) {
    BleProfileCentral* profile = context;
    hci_event_pckt* hci = (void*)((hci_uart_pckt*)packet)->data;
    BleCentralEvent event = {.connection_handle = CENTRAL_INVALID_HANDLE};

    if(hci->evt == HCI_LE_META_EVT_CODE) {
        if(hci->plen < 1) return BleEventAckFlowEnable;
        evt_le_meta_event* meta = (void*)hci->data;
        if(meta->subevent == HCI_LE_ADVERTISING_REPORT_SUBEVT_CODE) {
            central_advertisements(profile, meta->data, hci->plen - 1);
        } else if(meta->subevent == HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE) {
            if((size_t)(hci->plen - 1) < sizeof(hci_le_connection_complete_event_rp0)) {
                central_invalid_packet(profile);
                return BleEventAckFlowEnable;
            }
            hci_le_connection_complete_event_rp0* connected = (void*)meta->data;
            central_lock(profile);
            if(connected->Status == BLE_STATUS_SUCCESS) {
                profile->connection_handle = connected->Connection_Handle;
                profile->state = CentralConnected;
                profile->att_failed = false;
                event.type = BleCentralEventConnected;
                event.connection_handle = connected->Connection_Handle;
            } else {
                profile->state = CentralIdle;
                event.type = BleCentralEventConnectFailed;
                event.status = connected->Status;
            }
            central_unlock(profile);
            central_emit(profile, &event);
        }
        /* This profile is exclusive: don't feed central events to peripheral GAP. */
        return BleEventAckFlowEnable;
    }

    if(hci->evt == HCI_DISCONNECTION_COMPLETE_EVT_CODE) {
        if(hci->plen < sizeof(hci_disconnection_complete_event_rp0)) {
            central_invalid_packet(profile);
            return BleEventAckFlowEnable;
        }
        hci_disconnection_complete_event_rp0* disconnected = (void*)hci->data;
        central_lock(profile);
        if(disconnected->Connection_Handle == profile->connection_handle) {
            if(disconnected->Status) {
                profile->state = CentralConnected;
                event.type = BleCentralEventError;
                event.status = disconnected->Status;
                event.connection_handle = disconnected->Connection_Handle;
                central_unlock(profile);
                central_emit(profile, &event);
                return BleEventAckFlowEnable;
            }
            profile->connection_handle = CENTRAL_INVALID_HANDLE;
            profile->state = CentralIdle;
            profile->pending = BleCentralProcedureNone;
            event.type = BleCentralEventDisconnected;
            event.connection_handle = disconnected->Connection_Handle;
            event.status = disconnected->Reason;
            central_unlock(profile);
            central_emit(profile, &event);
        } else {
            central_unlock(profile);
        }
        return BleEventAckFlowEnable;
    }

    if(hci->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE || hci->plen < 2) {
        return BleEventNotAck;
    }
    evt_blecore_aci* vendor = (void*)hci->data;
    const uint8_t* data = vendor->data;
    size_t len = hci->plen - 2;
    if(vendor->ecode == ACI_GAP_PROC_COMPLETE_VSEVT_CODE) {
        if(len < 3) return BleEventAckFlowEnable;
        central_lock(profile);
        if(data[0] == GAP_OBSERVATION_PROC) {
            profile->state = CentralIdle;
            event.type = BleCentralEventScanStopped;
        } else if(
            data[0] == GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC &&
            (profile->state == CentralConnecting || profile->state == CentralCancelling)) {
            profile->state = CentralIdle;
            event.type = BleCentralEventConnectFailed;
        } else {
            central_unlock(profile);
            return BleEventAckFlowEnable;
        }
        event.status = data[1];
        central_unlock(profile);
        central_emit(profile, &event);
        return BleEventAckFlowEnable;
    }

    /* Every ATT event handled below begins with the connection handle. */
    central_lock(profile);
    bool ours = len >= 2 && profile->connection_handle != CENTRAL_INVALID_HANDLE &&
                ble_central_u16(data) == profile->connection_handle;
    event.connection_handle = profile->connection_handle;
    event.procedure = profile->pending;
    event.handle = profile->pending_handle;
    central_unlock(profile);
    if(!ours) return BleEventNotAck;

    switch(vendor->ecode) {
    case ACI_L2CAP_CONNECTION_UPDATE_REQ_VSEVT_CODE:
        if(len < sizeof(aci_l2cap_connection_update_req_event_rp0)) {
            central_invalid_packet(profile);
        } else {
            aci_l2cap_connection_update_req_event_rp0* request = (void*)data;
            event.type = BleCentralEventConnectionParameters;
            event.identifier = request->Identifier;
            event.connection_params.conn_int_min = request->Interval_Min;
            event.connection_params.conn_int_max = request->Interval_Max;
            event.connection_params.slave_latency = request->Latency;
            event.connection_params.supervisor_timeout = request->Timeout_Multiplier;
            central_emit(profile, &event);
        }
        break;
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE:
    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE:
        if(len < 4 || data[3] > len - 4) {
            central_invalid_packet(profile);
        } else if(
            vendor->ecode == ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE &&
            event.procedure == BleCentralProcedureServices) {
            central_records(profile, BleCentralEventService, data + 4, data[3], data[2]);
        } else if(
            vendor->ecode == ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE &&
            event.procedure == BleCentralProcedureCharacteristics) {
            central_records(profile, BleCentralEventCharacteristic, data + 4, data[3], data[2]);
        }
        break;
    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE:
        if(len < 4 || data[3] > len - 4 || (data[2] != 1 && data[2] != 2)) {
            central_invalid_packet(profile);
        } else if(event.procedure == BleCentralProcedureDescriptors) {
            central_records(
                profile, BleCentralEventDescriptor, data + 4, data[3], data[2] == 1 ? 4 : 18);
        }
        break;
    case ACI_ATT_READ_RESP_VSEVT_CODE:
        if(len < 3 || data[2] > len - 3) {
            central_invalid_packet(profile);
        } else if(event.procedure == BleCentralProcedureRead) {
            event.type = BleCentralEventRead;
            event.data = data + 3;
            event.data_len = data[2];
            central_emit(profile, &event);
        }
        break;
    case ACI_GATT_NOTIFICATION_VSEVT_CODE:
        if(len < 5 || data[4] > len - 5) {
            central_invalid_packet(profile);
        } else {
            event.type = BleCentralEventNotification;
            event.handle = ble_central_u16(data + 2);
            event.data = data + 5;
            event.data_len = data[4];
            central_emit(profile, &event);
        }
        break;
    case ACI_GATT_ERROR_RESP_VSEVT_CODE:
        if(len < 6) {
            central_invalid_packet(profile);
        } else {
            central_lock(profile);
            profile->att_error = data[5];
            central_unlock(profile);
            /* Report once, at procedure completion, including failed discovery. */
        }
        break;
    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE:
        if(len < 3) {
            central_invalid_packet(profile);
        } else {
            central_lock(profile);
            uint8_t status = data[2] ? data[2] : profile->att_error;
            event.status =
                ble_central_discovery_status_ok(status, central_is_discovery(profile->pending)) ?
                    0 :
                    status;
            profile->pending = BleCentralProcedureNone;
            profile->att_error = 0;
            central_unlock(profile);
            event.type = BleCentralEventProcedureComplete;
            if(event.procedure != BleCentralProcedureNone) central_emit(profile, &event);
        }
        break;
    case ACI_GATT_PROC_TIMEOUT_VSEVT_CODE:
        central_lock(profile);
        profile->pending = BleCentralProcedureNone;
        profile->att_failed = true;
        central_unlock(profile);
        event.type = BleCentralEventError;
        event.status = 0xFF; /* Application should disconnect/reconnect after ATT timeout. */
        central_emit(profile, &event);
        break;
    default:
        /* No security or peripheral services in this profile. */
        break;
    }
    return BleEventAckFlowEnable;
}

static FuriHalBleProfileBase* central_start(FuriHalBleProfileParams params) {
    if(!params || furi_hal_bt_get_radio_stack() != FuriHalBtStackFull) return NULL;
    BleProfileCentral* profile = malloc(sizeof(BleProfileCentral));
    memset(profile, 0, sizeof(*profile));
    profile->base.config = ble_profile_central;
    profile->params = *(BleCentralParams*)params;
    profile->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    profile->connection_handle = CENTRAL_INVALID_HANDLE;
    profile->handler = ble_event_dispatcher_register_svc_handler(central_event_handler, profile);
    /* Exclusive client profile: discovery, reads, notifications, errors and completion. */
    if(aci_gatt_set_event_mask(0x0001C26A) != BLE_STATUS_SUCCESS) {
        ble_event_dispatcher_unregister_svc_handler(profile->handler);
        furi_mutex_free(profile->mutex);
        free(profile);
        return NULL;
    }
    furi_hal_power_insomnia_enter();
    return &profile->base;
}

static void central_stop(FuriHalBleProfileBase* base) {
    BleProfileCentral* profile = central_get(base);
    ble_event_dispatcher_unregister_svc_handler(profile->handler);
    furi_mutex_free(profile->mutex);
    free(profile);
    furi_hal_power_insomnia_exit();
}

static void central_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    memset(config, 0, sizeof(*config));
    config->adv_service.UUID_Type = UUID_TYPE_16;
    config->pairing_method = GapPairingNone;
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    strlcpy(
        config->adv_name,
        furi_hal_version_get_ble_local_device_name_ptr(),
        sizeof(config->adv_name));
}

static const FuriHalBleProfileTemplate central_template = {
    .start = central_start,
    .stop = central_stop,
    .get_gap_config = central_config,
};

const FuriHalBleProfileTemplate* const ble_profile_central = &central_template;

static bool central_transition(BleProfileCentral* profile, CentralState from, CentralState to) {
    central_lock(profile);
    bool ok = profile->state == from;
    if(ok) profile->state = to;
    central_unlock(profile);
    return ok;
}

static bool central_transition_result(
    BleProfileCentral* profile,
    tBleStatus status,
    CentralState expected,
    CentralState original) {
    if(status == BLE_STATUS_SUCCESS) return true;
    central_transition(profile, expected, original);
    return false;
}

bool ble_central_scan_start(FuriHalBleProfileBase* base) {
    BleProfileCentral* profile = central_get(base);
    if(!central_transition(profile, CentralIdle, CentralScanning)) return false;
    return central_transition_result(
        profile,
        aci_gap_start_observation_proc(0x60, 0x30, 1, GAP_PUBLIC_ADDR, 0, 0),
        CentralScanning,
        CentralIdle);
}

bool ble_central_scan_stop(FuriHalBleProfileBase* base) {
    BleProfileCentral* profile = central_get(base);
    if(!central_transition(profile, CentralScanning, CentralScanStopping)) return false;
    return central_transition_result(
        profile,
        aci_gap_terminate_gap_proc(GAP_OBSERVATION_PROC),
        CentralScanStopping,
        CentralScanning);
}

bool ble_central_connect(
    FuriHalBleProfileBase* base,
    uint8_t address_type,
    const uint8_t address[6]) {
    BleProfileCentral* profile = central_get(base);
    if(!address || address_type > 1 ||
       !central_transition(profile, CentralIdle, CentralConnecting)) {
        return false;
    }
    return central_transition_result(
        profile,
        aci_gap_create_connection(
            0x60, 0x30, address_type, address, GAP_PUBLIC_ADDR, 24, 40, 0, 400, 0, 0),
        CentralConnecting,
        CentralIdle);
}

bool ble_central_cancel_connect(FuriHalBleProfileBase* base) {
    BleProfileCentral* profile = central_get(base);
    if(!central_transition(profile, CentralConnecting, CentralCancelling)) return false;
    return central_transition_result(
        profile,
        aci_gap_terminate_gap_proc(GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC),
        CentralCancelling,
        CentralConnecting);
}

bool ble_central_disconnect(FuriHalBleProfileBase* base) {
    BleProfileCentral* profile = central_get(base);
    if(!central_transition(profile, CentralConnected, CentralDisconnecting)) return false;
    return central_transition_result(
        profile,
        aci_gap_terminate(profile->connection_handle, 0x13),
        CentralDisconnecting,
        CentralConnected);
}

bool ble_central_respond_connection_parameters(
    FuriHalBleProfileBase* base,
    const BleCentralEvent* event) {
    BleProfileCentral* profile = central_get(base);
    central_lock(profile);
    bool connected = profile->state == CentralConnected && event &&
                     event->type == BleCentralEventConnectionParameters &&
                     event->connection_handle == profile->connection_handle;
    central_unlock(profile);
    if(!connected) return false;
    const GapConnectionParamsRequest* params = &event->connection_params;
    bool accept = ble_central_connection_params_valid(
        params->conn_int_min,
        params->conn_int_max,
        params->slave_latency,
        params->supervisor_timeout);
    return aci_l2cap_connection_parameter_update_resp(
               profile->connection_handle,
               accept ? params->conn_int_min : 24,
               accept ? params->conn_int_max : 40,
               accept ? params->slave_latency : 0,
               accept ? params->supervisor_timeout : 400,
               0,
               0,
               event->identifier,
               accept) == BLE_STATUS_SUCCESS;
}

static bool
    central_begin_att(BleProfileCentral* profile, BleCentralProcedure procedure, uint16_t handle) {
    central_lock(profile);
    bool ready = profile->state == CentralConnected && !profile->att_failed &&
                 profile->pending == BleCentralProcedureNone;
    if(ready) {
        profile->pending = procedure;
        profile->pending_handle = handle;
        profile->att_error = 0;
    }
    central_unlock(profile);
    return ready;
}

static bool central_att_result(BleProfileCentral* profile, tBleStatus status) {
    if(status == BLE_STATUS_SUCCESS) return true;
    central_lock(profile);
    profile->pending = BleCentralProcedureNone;
    central_unlock(profile);
    return false;
}

bool ble_central_discover_services(FuriHalBleProfileBase* base) {
    BleProfileCentral* profile = central_get(base);
    if(!central_begin_att(profile, BleCentralProcedureServices, 0)) return false;
    return central_att_result(
        profile, aci_gatt_disc_all_primary_services(profile->connection_handle));
}

bool ble_central_discover_characteristics(
    FuriHalBleProfileBase* base,
    uint16_t start_handle,
    uint16_t end_handle) {
    BleProfileCentral* profile = central_get(base);
    if(!start_handle || end_handle < start_handle ||
       !central_begin_att(profile, BleCentralProcedureCharacteristics, start_handle)) {
        return false;
    }
    return central_att_result(
        profile,
        aci_gatt_disc_all_char_of_service(profile->connection_handle, start_handle, end_handle));
}

bool ble_central_discover_descriptors(
    FuriHalBleProfileBase* base,
    uint16_t start_handle,
    uint16_t end_handle) {
    BleProfileCentral* profile = central_get(base);
    if(!start_handle || end_handle < start_handle ||
       !central_begin_att(profile, BleCentralProcedureDescriptors, start_handle)) {
        return false;
    }
    return central_att_result(
        profile,
        aci_gatt_disc_all_char_desc(profile->connection_handle, start_handle, end_handle));
}

bool ble_central_read(FuriHalBleProfileBase* base, uint16_t value_handle) {
    BleProfileCentral* profile = central_get(base);
    if(!value_handle || !central_begin_att(profile, BleCentralProcedureRead, value_handle))
        return false;
    return central_att_result(
        profile, aci_gatt_read_char_value(profile->connection_handle, value_handle));
}

bool ble_central_write(
    FuriHalBleProfileBase* base,
    uint16_t value_handle,
    const uint8_t* data,
    uint8_t data_len) {
    BleProfileCentral* profile = central_get(base);
    if(!value_handle || !data || !data_len || data_len > CENTRAL_PAYLOAD_MAX ||
       !central_begin_att(profile, BleCentralProcedureWrite, value_handle)) {
        return false;
    }
    return central_att_result(
        profile,
        aci_gatt_write_char_value(profile->connection_handle, value_handle, data_len, data));
}

bool ble_central_subscribe(FuriHalBleProfileBase* base, uint16_t cccd_handle, bool enable) {
    BleProfileCentral* profile = central_get(base);
    if(!cccd_handle || !central_begin_att(profile, BleCentralProcedureSubscribe, cccd_handle)) {
        return false;
    }
    uint8_t value[2] = {enable ? 1 : 0, 0};
    return central_att_result(
        profile,
        aci_gatt_write_char_desc(profile->connection_handle, cccd_handle, sizeof(value), value));
}
