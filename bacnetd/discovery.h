#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>
#include "bacnet/bacdef.h"

#define MAX_DEVICES 128

typedef struct {
    uint32_t device_id;
    char     addr[64];   /* IP address string */
    uint16_t vendor_id;
    char     name[64];   /* object name, filled in after ReadProperty */
} bacnet_device_t;

typedef struct {
    bacnet_device_t devices[MAX_DEVICES];
    int             count;
} device_list_t;

/*
 * I-Am handler that adds devices to the internal address cache.
 */
void my_handler_i_am_add(uint8_t *service_request, uint16_t service_len, BACNET_ADDRESS *src);

/*
 * Broadcast Who-Is, wait timeout_ms for I-Am responses.
 * Fills out *list. Returns number of devices found.
 */
int discovery_whois(device_list_t *list, int timeout_ms);

/*
 * Broadcast targeted Who-Is for a single device ID.
 * Blocks until the device responds or timeout_ms expires.
 * Returns true if the device is now in the address cache.
 */
bool discovery_whois_target(uint32_t device_id, int timeout_ms);

#endif /* DISCOVERY_H */

