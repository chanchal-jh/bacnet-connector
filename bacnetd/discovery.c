#include "discovery.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

/* bacnet-stack headers */
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/apdu.h"
#include "bacnet/iam.h"
#include "bacnet/whois.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/npdu.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/basic/service/h_iam.h"
#include "bacnet/basic/npdu/h_npdu.h"

/* ------------------------------------------------------------------ */
/* Global state shared with main BACnet receive loop                   */
/* ------------------------------------------------------------------ */
static device_list_t *g_list   = NULL;

/* Called by the BACnet stack when an I-Am PDU is decoded */
void my_handler_i_am_add(uint8_t *service_request, uint16_t service_len, BACNET_ADDRESS *src)
{
    uint32_t device_id = 0;
    unsigned max_apdu = 0;
    int segmentation = 0;
    uint16_t vendor_id = 0;

    int len = bacnet_iam_request_decode(
        service_request, service_len, &device_id, &max_apdu, &segmentation,
        &vendor_id);
    if (len <= 0) return;

    /* Add to stack's address cache for later ReadProperty */
    address_add(device_id, max_apdu, src);

    if (!g_list || g_list->count >= MAX_DEVICES) {
        return;
    }

    bacnet_device_t *d = &g_list->devices[g_list->count];
    d->device_id = device_id;
    d->vendor_id = vendor_id;
    snprintf(d->name, sizeof(d->name), "device-%u", device_id);

    /* Convert BACnet address to dotted-quad string */
    if (src->net == 0 && src->mac_len == 6) {
        /* BACnet/IP: mac = 4-byte IP + 2-byte port */
        snprintf(d->addr, sizeof(d->addr),
                 "%u.%u.%u.%u",
                 src->mac[0], src->mac[1], src->mac[2], src->mac[3]);
    } else {
        snprintf(d->addr, sizeof(d->addr), "unknown");
    }

    g_list->count++;
    fprintf(stdout, "[discovery] Found device %u at %s\n",
            device_id, d->addr);
}

/* ------------------------------------------------------------------ */

int discovery_whois(device_list_t *list, int timeout_ms)
{
    BACNET_ADDRESS dest;
    BACNET_ADDRESS my_address;
    BACNET_NPDU_DATA npdu_data;
    uint8_t pdu[MAX_PDU];
    int pdu_len, apdu_len;

    list->count = 0;
    g_list      = list;

    /* Set global handler — the main receive loop calls apdu_handler()
     * which dispatches I-Am to handler_i_am_add above.             */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, my_handler_i_am_add);

    /* Build broadcast destination */
    datalink_get_broadcast_address(&dest);
    datalink_get_my_address(&my_address);

    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
    pdu_len = npdu_encode_pdu(&pdu[0], &dest, &my_address, &npdu_data);

    apdu_len = whois_encode_apdu(&pdu[pdu_len], -1, -1);   /* Who-Is all devices */
    if (apdu_len > 0) {
        datalink_send_pdu(&dest, &npdu_data, &pdu[0], pdu_len + apdu_len);
    }

    /* Wait timeout_ms for I-Am responses */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    uint8_t rx_buf[MAX_PDU];
    uint16_t rx_pdu_len;
    BACNET_ADDRESS src;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec)) {
            break;
        }

        rx_pdu_len = datalink_receive(&src, rx_buf, sizeof(rx_buf), 50);
        if (rx_pdu_len > 0) {
            npdu_handler(&src, rx_buf, rx_pdu_len);
        }
    }

    g_list = NULL;
    return list->count;
}

bool discovery_whois_target(uint32_t device_id, int timeout_ms)
{
    BACNET_ADDRESS dest;
    BACNET_ADDRESS my_address;
    BACNET_NPDU_DATA npdu_data;
    uint8_t pdu[MAX_PDU];
    int pdu_len, apdu_len;

    /* Build broadcast destination */
    datalink_get_broadcast_address(&dest);
    datalink_get_my_address(&my_address);

    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
    pdu_len = npdu_encode_pdu(&pdu[0], &dest, &my_address, &npdu_data);

    apdu_len = whois_encode_apdu(&pdu[pdu_len], device_id, device_id);
    if (apdu_len > 0) {
        datalink_send_pdu(&dest, &npdu_data, &pdu[0], pdu_len + apdu_len);
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    uint8_t rx_buf[MAX_PDU];
    uint16_t rx_pdu_len;
    BACNET_ADDRESS src;
    unsigned max_apdu = 0;

    while (1) {
        /* Check if it's already populated in the cache! */
        if (address_get_by_device(device_id, &max_apdu, &src)) {
            return true;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            break;
        }

        rx_pdu_len = datalink_receive(&src, rx_buf, sizeof(rx_buf), 50);
        if (rx_pdu_len > 0) {
            npdu_handler(&src, rx_buf, rx_pdu_len);
        }
    }

    return false;
}
