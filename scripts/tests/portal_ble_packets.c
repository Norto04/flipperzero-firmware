#include "../../targets/f7/ble_glue/furi_ble/central_packet_i.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    /* Reject zero/unknown strides and partial records before indexing a UUID. */
    assert(!ble_central_att_records_valid(20, 0, 6, 20));
    assert(!ble_central_att_records_valid(19, 20, 6, 20));
    assert(!ble_central_att_records_valid(8, 8, 6, 20));
    assert(ble_central_att_records_valid(12, 6, 6, 20));
    assert(ble_central_att_records_valid(40, 20, 6, 20));
    assert(ble_central_att_records_valid(42, 21, 7, 21));
    assert(ble_central_att_records_valid(36, 18, 4, 18));

    /* Empty service trees still complete; authentication failures stay errors. */
    assert(ble_central_discovery_status_ok(0, true));
    assert(ble_central_discovery_status_ok(0x0A, true));
    assert(!ble_central_discovery_status_ok(0x0A, false));
    assert(!ble_central_discovery_status_ok(0x05, true));

    assert(ble_central_connection_params_valid(24, 40, 0, 400));
    assert(!ble_central_connection_params_valid(6, 40, 0, 400));
    assert(!ble_central_connection_params_valid(40, 24, 0, 400));
    assert(!ble_central_connection_params_valid(24, 40, 499, 400));
    assert(!ble_central_connection_params_valid(24, 40, 0, 10));

    /* Portal's value is 0x000e and CCCD is 0x0010, separated by a descriptor. */
    const uint8_t descriptor_records[] = {0x0F, 0, 1, 0x29, 0x10, 0, 2, 0x29};
    assert(ble_central_u16(descriptor_records) == 0x000F);
    assert(ble_central_u16(descriptor_records + 4) == 0x0010);
    assert(ble_central_u16(descriptor_records + 6) == 0x2902);

    /* Walk two variable-size reports instead of treating the first as an array. */
    const uint8_t reports[] = {0,    1, 0xA5, 0x9B, 0xA4, 0x93, 0x11, 0xD0, 3, 2, 1,   6,
                               0xC0, 0, 0,    1,    2,    3,    4,    5,    6, 0, 0xD0};
    size_t first = ble_central_advertisement_size(reports, sizeof(reports));
    assert(first == 13);
    assert(ble_central_advertisement_size(reports + first, sizeof(reports) - first) == 10);
    assert(!ble_central_advertisement_size(reports, first - 1));
    assert(!ble_central_advertisement_size(reports, 8));
    uint8_t bad_report[10] = {0};
    bad_report[8] = 32;
    assert(!ble_central_advertisement_size(bad_report, sizeof(bad_report)));
    puts("Portal BLE packet checks passed");
    return 0;
}
