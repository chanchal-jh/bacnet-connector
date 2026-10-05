#include "readprop.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* bacnet-stack headers */
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/apdu.h"
#include "bacnet/rp.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/npdu.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/bacdcode.h"
#include "bacnet/bacapp.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/bactext.h"
#include "discovery.h"
#include "bacnet/basic/npdu/h_npdu.h"
#include "bacnet/basic/service/h_rp.h"

/* ------------------------------------------------------------------ */
/* Pending request context (one at a time for Step 1)                  */
/* ------------------------------------------------------------------ */
static struct {
    int             in_flight;
    uint8_t         invoke_id;
    bacnet_value_t *result;
    int             done;
} g_rp = { 0 };

/* ------------------------------------------------------------------ */
/* Response handler — registered with apdu handler                     */
/* ------------------------------------------------------------------ */
static void rp_ack_handler(uint8_t *service_request,
                           uint16_t service_len,
                           BACNET_ADDRESS *src,
                           BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    (void)src;
    uint8_t invoke_id = service_data->invoke_id;
    if (invoke_id != g_rp.invoke_id || !g_rp.in_flight) return;

    BACNET_READ_PROPERTY_DATA data;
    int rc = rp_ack_decode_service_request(service_request, service_len, &data);
    if (rc < 0) return;

    BACNET_APPLICATION_DATA_VALUE val;
    int len = bacapp_decode_application_data(
        data.application_data,
        data.application_data_len,
        &val);

    if (len <= 0) {
        snprintf(g_rp.result->error_msg, sizeof(g_rp.result->error_msg),
                 "decode failed");
        g_rp.result->type = BACNET_VAL_ERROR;
    } else {
        switch (val.tag) {
        case BACNET_APPLICATION_TAG_REAL:
            g_rp.result->type   = BACNET_VAL_REAL;
            g_rp.result->v.real = val.type.Real;
            break;
        case BACNET_APPLICATION_TAG_UNSIGNED_INT:
            g_rp.result->type   = BACNET_VAL_UNSIGNED;
            g_rp.result->v.uval = val.type.Unsigned_Int;
            break;
        case BACNET_APPLICATION_TAG_SIGNED_INT:
            g_rp.result->type   = BACNET_VAL_SIGNED;
            g_rp.result->v.ival = val.type.Signed_Int;
            break;
        case BACNET_APPLICATION_TAG_BOOLEAN:
            g_rp.result->type      = BACNET_VAL_BOOLEAN;
            g_rp.result->v.boolean = val.type.Boolean;
            break;
        case BACNET_APPLICATION_TAG_ENUMERATED:
            g_rp.result->type      = BACNET_VAL_ENUMERATED;
            g_rp.result->v.enumval = val.type.Enumerated;
            break;
        case BACNET_APPLICATION_TAG_CHARACTER_STRING:
            g_rp.result->type = BACNET_VAL_STRING;
            characterstring_copy_value(
                g_rp.result->v.str,
                sizeof(g_rp.result->v.str),
                &val.type.Character_String);
            break;
        case BACNET_APPLICATION_TAG_OBJECT_ID:
            g_rp.result->type = BACNET_VAL_OBJECT_ID;
            g_rp.result->v.obj_id.type = val.type.Object_Id.type;
            g_rp.result->v.obj_id.instance = val.type.Object_Id.instance;
            break;
        default:
            snprintf(g_rp.result->error_msg, sizeof(g_rp.result->error_msg),
                     "unsupported tag %d", val.tag);
            g_rp.result->type = BACNET_VAL_ERROR;
            break;
        }
    }

    g_rp.done = 1;
}

static void rp_error_handler(BACNET_ADDRESS *src,
                               uint8_t         invoke_id,
                               BACNET_ERROR_CLASS  err_class,
                               BACNET_ERROR_CODE   err_code)
{
    (void)src;
    if (invoke_id != g_rp.invoke_id || !g_rp.in_flight) return;
    snprintf(g_rp.result->error_msg, sizeof(g_rp.result->error_msg),
             "BACnet error class=%d code=%d", err_class, err_code);
    g_rp.result->type = BACNET_VAL_ERROR;
    g_rp.done = 1;
}

/* ------------------------------------------------------------------ */

int readprop_read(uint32_t        device_id,
                  uint16_t        obj_type,
                  uint32_t        obj_instance,
                  uint32_t        prop_id,
                  uint32_t        array_index,
                  int             timeout_ms,
                  bacnet_value_t *out)
{
    BACNET_ADDRESS  dest;
    BACNET_ADDRESS  my_address;
    unsigned        max_apdu = 0;
    bool            found;
    int             pdu_len, apdu_len;

    memset(out, 0, sizeof(*out));

    found = address_get_by_device(device_id, &max_apdu, &dest);
    if (!found) {
        /* Not in cache. Attempt targeted auto-discovery! */
        fprintf(stdout, "[readprop] Device %u not in cache, attempting auto-discovery...\n", device_id);
        if (discovery_whois_target(device_id, 2000)) {
            found = address_get_by_device(device_id, &max_apdu, &dest);
        }
        
        if (!found) {
            snprintf(out->error_msg, sizeof(out->error_msg),
                     "device %u not in address cache (auto-discovery timed out)", device_id);
            out->type = BACNET_VAL_ERROR;
            return -1;
        }
    }

    /* Register handlers */
    apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROPERTY,
                                   rp_ack_handler);
    apdu_set_error_handler(SERVICE_CONFIRMED_READ_PROPERTY,
                           rp_error_handler);

    /* Build and send ReadProperty request */
    BACNET_READ_PROPERTY_DATA rp_data;
    rp_data.object_type       = (BACNET_OBJECT_TYPE)obj_type;
    rp_data.object_instance   = obj_instance;
    rp_data.object_property   = (BACNET_PROPERTY_ID)prop_id;
    rp_data.array_index       = array_index;

    uint8_t invoke_id = tsm_next_free_invokeID();
    if (invoke_id == 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "no free invoke IDs");
        out->type = BACNET_VAL_ERROR;
        return -1;
    }

    uint8_t pdu[MAX_PDU];
    BACNET_NPDU_DATA npdu_data;
    
    datalink_get_my_address(&my_address);
    npdu_encode_npdu_data(&npdu_data, true, MESSAGE_PRIORITY_NORMAL);
    pdu_len = npdu_encode_pdu(&pdu[0], &dest, &my_address, &npdu_data);

    apdu_len = rp_encode_apdu(&pdu[pdu_len], invoke_id, &rp_data);
    if (apdu_len <= 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "encode failed");
        out->type = BACNET_VAL_ERROR;
        return -1;
    }

    g_rp.invoke_id = invoke_id;
    g_rp.result    = out;
    g_rp.done      = 0;
    g_rp.in_flight = 1;

    datalink_send_pdu(&dest, &npdu_data, &pdu[0], pdu_len + apdu_len);

    /* Wait for response */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    uint8_t rx_buf[MAX_PDU];
    uint16_t rx_len;
    BACNET_ADDRESS rx_src;

    while (!g_rp.done) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec)) {
            snprintf(out->error_msg, sizeof(out->error_msg), "timeout");
            out->type = BACNET_VAL_ERROR;
            g_rp.in_flight = 0;
            return -1;
        }
        rx_len = datalink_receive(&rx_src, rx_buf, sizeof(rx_buf), 50);
        if (rx_len > 0) {
            npdu_handler(&rx_src, rx_buf, rx_len);
        }
    }

    g_rp.in_flight = 0;
    return (out->type == BACNET_VAL_ERROR) ? -1 : 0;
}
