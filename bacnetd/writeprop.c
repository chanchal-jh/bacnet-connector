#include "writeprop.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* bacnet-stack headers */
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/apdu.h"
#include "bacnet/wp.h"
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
#include "bacnet/basic/service/s_wp.h"
#include "bacnet/basic/service/h_wp.h"

/* ------------------------------------------------------------------ */
/* Pending request context (one at a time)                            */
/* ------------------------------------------------------------------ */
static struct {
    int                 in_flight;
    uint8_t             invoke_id;
    writeprop_result_t *result;
    int                 done;
} g_wp = { 0 };

/* ------------------------------------------------------------------ */
/* Response handlers                                                  */
/* ------------------------------------------------------------------ */
static void wp_simple_ack_handler(BACNET_ADDRESS *src, uint8_t invoke_id)
{
    (void)src;
    if (invoke_id != g_wp.invoke_id || !g_wp.in_flight) return;
    g_wp.result->ok = 1;
    g_wp.done = 1;
}

static void wp_error_handler(BACNET_ADDRESS *src,
                             uint8_t         invoke_id,
                             BACNET_ERROR_CLASS  err_class,
                             BACNET_ERROR_CODE   err_code)
{
    (void)src;
    if (invoke_id != g_wp.invoke_id || !g_wp.in_flight) return;
    snprintf(g_wp.result->error_msg, sizeof(g_wp.result->error_msg),
             "BACnet error class=%d code=%d", err_class, err_code);
    g_wp.result->ok = 0;
    g_wp.done = 1;
}

static void wp_abort_handler(BACNET_ADDRESS *src,
                             uint8_t         invoke_id,
                             uint8_t         abort_reason,
                             bool            server)
{
    (void)src;
    (void)server;
    if (invoke_id != g_wp.invoke_id || !g_wp.in_flight) return;
    snprintf(g_wp.result->error_msg, sizeof(g_wp.result->error_msg),
             "BACnet abort reason=%d", abort_reason);
    g_wp.result->ok = 0;
    g_wp.done = 1;
}

static void wp_reject_handler(BACNET_ADDRESS *src,
                              uint8_t         invoke_id,
                              uint8_t         reject_reason)
{
    (void)src;
    if (invoke_id != g_wp.invoke_id || !g_wp.in_flight) return;
    snprintf(g_wp.result->error_msg, sizeof(g_wp.result->error_msg),
             "BACnet reject reason=%d", reject_reason);
    g_wp.result->ok = 0;
    g_wp.done = 1;
}

/* ------------------------------------------------------------------ */
/* Write                                                              */
/* ------------------------------------------------------------------ */
int writeprop_write(uint32_t          device_id,
                    uint16_t          obj_type,
                    uint32_t          obj_instance,
                    uint32_t          prop_id,
                    uint32_t          array_index,
                    uint8_t           priority,
                    const wp_val_t   *val,
                    int               timeout_ms,
                    writeprop_result_t *out)
{
    BACNET_ADDRESS dest;
    unsigned max_apdu = 0;
    bool found;

    out->ok = 0;
    out->error_msg[0] = '\0';

    if (g_wp.in_flight) {
        snprintf(out->error_msg, sizeof(out->error_msg), "busy");
        return -1;
    }

    found = address_get_by_device(device_id, &max_apdu, &dest);
    if (!found) {
        /* Not in cache. Attempt targeted auto-discovery! */
        fprintf(stdout, "[writeprop] Device %u not in cache, attempting auto-discovery...\n", device_id);
        if (discovery_whois_target(device_id, 2000)) {
            found = address_get_by_device(device_id, &max_apdu, &dest);
        }
        
        if (!found) {
            snprintf(out->error_msg, sizeof(out->error_msg),
                     "device %u not in address cache (auto-discovery timed out)", device_id);
            return -1;
        }
    }

    /* Register handlers */
    apdu_set_confirmed_simple_ack_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, wp_simple_ack_handler);
    apdu_set_error_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, wp_error_handler);
    apdu_set_abort_handler(wp_abort_handler);
    apdu_set_reject_handler(wp_reject_handler);

    /* Encode value */
    uint8_t application_data[MAX_APDU];
    int application_data_len = 0;
    BACNET_CHARACTER_STRING bstr;

    switch (val->type) {
    case WP_TYPE_REAL:
        application_data_len = encode_application_real(application_data, val->v.real);
        break;
    case WP_TYPE_UNSIGNED:
        application_data_len = encode_application_unsigned(application_data, val->v.uval);
        break;
    case WP_TYPE_SIGNED:
        application_data_len = encode_application_signed(application_data, val->v.ival);
        break;
    case WP_TYPE_BOOLEAN:
        application_data_len = encode_application_boolean(application_data, val->v.boolean);
        break;
    case WP_TYPE_ENUMERATED:
        application_data_len = encode_application_enumerated(application_data, val->v.enumval);
        break;
    case WP_TYPE_STRING:
        characterstring_init_ansi(&bstr, val->v.str);
        application_data_len = encode_application_character_string(application_data, &bstr);
        break;
    case WP_TYPE_NULL:
        application_data_len = encode_application_null(application_data);
        break;
    default:
        snprintf(out->error_msg, sizeof(out->error_msg), "unsupported write type");
        return -1;
    }

    uint8_t invoke_id = Send_Write_Property_Request_Data(
        device_id,
        obj_type,
        obj_instance,
        prop_id,
        application_data,
        application_data_len,
        priority,
        array_index);

    if (invoke_id == 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "failed to send (tsm full or unroutable)");
        return -1;
    }

    /* Wait for response */
    g_wp.in_flight = 1;
    g_wp.invoke_id = invoke_id;
    g_wp.result    = out;
    g_wp.done      = 0;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    uint8_t rx_buf[MAX_PDU];
    BACNET_ADDRESS src;
    while (!g_wp.done) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            snprintf(out->error_msg, sizeof(out->error_msg), "timeout");
            break;
        }
        int rx_pdu_len = datalink_receive(&src, rx_buf, sizeof(rx_buf), 50);
        if (rx_pdu_len > 0) {
            npdu_handler(&src, rx_buf, rx_pdu_len);
        }
    }

    if (!g_wp.done) {
        tsm_free_invoke_id(invoke_id);
    }
    
    g_wp.in_flight = 0;

    return out->ok ? 0 : -1;
}

