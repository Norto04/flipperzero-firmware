#include <furi.h>
#include <gui/gui.h>
#include <bt/bt_service/bt.h>
#include <profiles/central_profile.h>
#include <string.h>

#define TAG                  "PortalBleTest"
#define PORTAL_FLAG_EXIT     (1U << 0)
#define PORTAL_FLAG_OVERFLOW (1U << 1)

/* Test target D0:11:93:A4:9B:A5, in HCI byte order. */
static const uint8_t portal_address[6] = {0xA5, 0x9B, 0xA4, 0x93, 0x11, 0xD0};
static const uint8_t portal_service_uuid[16] =
    {0xA3, 0x8E, 0x0A, 0x8B, 0x4E, 0x59, 0x00, 0xCD, 0x3F, 0xF3, 0xBE, 0x3A, 0x30, 0x15, 0x3E, 0x53};

typedef struct {
    BleCentralEvent event;
    uint8_t data[20];
} PortalMessage;

typedef struct {
    FuriMessageQueue* queue;
    FuriMutex* display_mutex;
    FuriString* status;
    FuriEventFlag* flags;
    uint32_t notifications;
    uint8_t peer_type;
    uint16_t service_start;
    uint16_t service_end;
    uint16_t notify_handle;
    uint16_t notify_end;
    uint16_t write_handle;
    uint16_t cccd_handle;
    bool found;
    bool ready;
    bool write_pending;
    uint32_t deadline;
    uint32_t next_keepalive;
} PortalTest;

static void portal_status(PortalTest* test, const char* status) {
    furi_mutex_acquire(test->display_mutex, FuriWaitForever);
    furi_string_set(test->status, status);
    furi_mutex_release(test->display_mutex);
    FURI_LOG_I(TAG, "%s", status);
}

static void portal_draw(Canvas* canvas, void* context) {
    PortalTest* test = context;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Portal BLE Test");
    canvas_set_font(canvas, FontSecondary);
    furi_mutex_acquire(test->display_mutex, FuriWaitForever);
    canvas_draw_str(canvas, 2, 30, furi_string_get_cstr(test->status));
    furi_mutex_release(test->display_mutex);
    canvas_draw_str(canvas, 2, 60, "Back: exit / restore Bluetooth");
}

static void portal_input(InputEvent* event, void* context) {
    PortalTest* test = context;
    if(event->key == InputKeyBack && event->type == InputTypeShort)
        furi_event_flag_set(test->flags, PORTAL_FLAG_EXIT);
}

static void portal_event(const BleCentralEvent* event, void* context) {
    PortalTest* test = context;
    if(event->type == BleCentralEventAdvertisement &&
       memcmp(event->address, portal_address, sizeof(portal_address)) != 0)
        return;
    PortalMessage message = {.event = *event};
    if(event->type == BleCentralEventAdvertisement) message.event.data_len = 0;
    if(message.event.data_len > sizeof(message.data)) {
        furi_event_flag_set(test->flags, PORTAL_FLAG_OVERFLOW);
        return;
    }
    if(message.event.data_len) memcpy(message.data, event->data, message.event.data_len);
    message.event.data = NULL;
    if(furi_message_queue_put(test->queue, &message, 0) != FuriStatusOk)
        furi_event_flag_set(test->flags, PORTAL_FLAG_OVERFLOW);
}

static bool portal_uuid(const BleCentralEvent* event, uint8_t low) {
    uint8_t uuid[16];
    memcpy(uuid, portal_service_uuid, sizeof(uuid));
    uuid[12] = low;
    return event->uuid_len == sizeof(uuid) && memcmp(event->uuid, uuid, sizeof(uuid)) == 0;
}

static bool
    portal_process(PortalTest* test, FuriHalBleProfileBase* profile, const PortalMessage* message) {
    const BleCentralEvent* event = &message->event;
    bool accepted = true;
    switch(event->type) {
    case BleCentralEventConnectionParameters:
        accepted = ble_central_respond_connection_parameters(profile, event);
        break;
    case BleCentralEventAdvertisement:
        if(!test->found && memcmp(event->address, portal_address, sizeof(portal_address)) == 0) {
            test->found = true;
            test->peer_type = event->address_type;
            portal_status(test, "Portal found");
            accepted = ble_central_scan_stop(profile);
        }
        break;
    case BleCentralEventScanStopped:
        if(!test->found || event->status) return false;
        portal_status(test, "Connecting...");
        accepted = ble_central_connect(profile, test->peer_type, portal_address);
        break;
    case BleCentralEventConnected:
        portal_status(test, "Discovering services...");
        accepted = ble_central_discover_services(profile);
        break;
    case BleCentralEventService:
        if(portal_uuid(event, 0x30)) {
            test->service_start = event->handle;
            test->service_end = event->end_handle;
        }
        break;
    case BleCentralEventCharacteristic:
        if(portal_uuid(event, 0x42) && (event->properties & 0x10)) {
            test->notify_handle = event->value_handle;
            test->notify_end = test->service_end;
        } else if(portal_uuid(event, 0x43) && (event->properties & 0x08)) {
            test->write_handle = event->value_handle;
        }
        if(test->notify_handle && event->handle > test->notify_handle &&
           event->handle - 1 < test->notify_end) {
            test->notify_end = event->handle - 1;
        }
        break;
    case BleCentralEventDescriptor:
        if(event->uuid_len == 2 && event->uuid[0] == 0x02 && event->uuid[1] == 0x29) {
            test->cccd_handle = event->handle;
        }
        break;
    case BleCentralEventProcedureComplete:
        if(event->status) {
            FURI_LOG_E(TAG, "ATT procedure failed: %02X", event->status);
            return false;
        }
        switch(event->procedure) {
        case BleCentralProcedureServices:
            accepted = test->service_start && ble_central_discover_characteristics(
                                                  profile, test->service_start, test->service_end);
            portal_status(test, "Discovering commands...");
            break;
        case BleCentralProcedureCharacteristics:
            accepted = test->notify_handle && test->write_handle &&
                       test->notify_end > test->notify_handle &&
                       ble_central_discover_descriptors(
                           profile, test->notify_handle + 1, test->notify_end);
            portal_status(test, "Finding notification CCCD...");
            break;
        case BleCentralProcedureDescriptors:
            FURI_LOG_I(
                TAG,
                "Notify=%04X CCCD=%04X Write=%04X",
                test->notify_handle,
                test->cccd_handle,
                test->write_handle);
            accepted = test->cccd_handle &&
                       ble_central_subscribe(profile, test->cccd_handle, true);
            break;
        case BleCentralProcedureSubscribe: {
            const uint8_t blue[20] = {'C', 0, 0, 0xFF};
            accepted = ble_central_write(profile, test->write_handle, blue, sizeof(blue));
            test->write_pending = accepted;
            portal_status(test, "Setting solid blue...");
            break;
        }
        case BleCentralProcedureWrite:
            test->write_pending = false;
            test->ready = true;
            test->next_keepalive = furi_get_tick() + furi_ms_to_ticks(1000);
            test->deadline = test->next_keepalive + furi_ms_to_ticks(10000);
            portal_status(test, "Blue / status keepalive active");
            break;
        default:
            break;
        }
        break;
    case BleCentralEventNotification:
        if(event->handle == test->notify_handle) {
            test->notifications++;
            FURI_LOG_I(
                TAG,
                "Notification #%lu: %u bytes, command %02X",
                test->notifications,
                event->data_len,
                event->data_len ? message->data[0] : 0);
        }
        break;
    case BleCentralEventError:
    case BleCentralEventConnectFailed:
    case BleCentralEventDisconnected:
        FURI_LOG_E(TAG, "BLE event %u: %02X", event->type, event->status);
        return false;
    default:
        break;
    }
    return accepted;
}

int32_t portal_ble_test_app(void* context) {
    UNUSED(context);
    PortalTest* test = malloc(sizeof(PortalTest));
    memset(test, 0, sizeof(*test));
    test->flags = furi_event_flag_alloc();
    test->queue = furi_message_queue_alloc(32, sizeof(PortalMessage));
    test->display_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    test->status = furi_string_alloc();
    portal_status(test, "Starting Full BLE profile...");
    ViewPort* view = view_port_alloc();
    view_port_draw_callback_set(view, portal_draw, test);
    view_port_input_callback_set(view, portal_input, test);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view, GuiLayerFullscreen);
    Bt* bt = furi_record_open(RECORD_BT);
    BleCentralParams params = {.callback = portal_event, .context = test};
    FuriHalBleProfileBase* profile = bt_profile_start(bt, ble_profile_central, &params);
    bool failure_shown = false;
    bool ok = profile && ble_central_scan_start(profile);
    portal_status(test, ok ? "Turn on the mobile portal" : "Full BLE profile unavailable");
    test->deadline = furi_get_tick() + furi_ms_to_ticks(30000);

    while(!(furi_event_flag_get(test->flags) & PORTAL_FLAG_EXIT)) {
        PortalMessage message;
        if(furi_message_queue_get(test->queue, &message, furi_ms_to_ticks(100)) == FuriStatusOk &&
           ok) {
            ok = portal_process(test, profile, &message);
        }
        uint32_t now = furi_get_tick();
        if(ok && ((furi_event_flag_get(test->flags) & PORTAL_FLAG_OVERFLOW) ||
                  (int32_t)(now - test->deadline) >= 0)) {
            ok = false;
        }
        if(ok && test->ready && !test->write_pending &&
           (int32_t)(now - test->next_keepalive) >= 0) {
            const uint8_t status[20] = {'S'};
            ok = ble_central_write(profile, test->write_handle, status, sizeof(status));
            test->write_pending = ok;
        }
        if(!ok && !failure_shown) {
            portal_status(test, "Stopped - see logs / Back");
            failure_shown = true;
        }
        view_port_update(view);
    }
    /* Restoring the profile resets the controller and unregisters this callback
     * before any application queue/context is freed. */
    if(!bt_profile_restore_default(bt)) FURI_LOG_E(TAG, "Could not restore serial profile");
    furi_record_close(RECORD_BT);
    gui_remove_view_port(gui, view);
    view_port_free(view);
    furi_record_close(RECORD_GUI);
    furi_string_free(test->status);
    furi_mutex_free(test->display_mutex);
    furi_message_queue_free(test->queue);
    furi_event_flag_free(test->flags);
    free(test);
    return 0;
}
