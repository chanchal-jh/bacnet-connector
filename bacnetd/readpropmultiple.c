#include "readpropmultiple.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* bacnet-stack headers */
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/apdu.h"
#include "bacnet/rpm.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/npdu.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/bacdcode.h"
#include "bacnet/bacapp.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/bactext.h"
#include "bacnet/basic/npdu/h_npdu.h"
#include "bacnet/basic/service/s_rpm.h"
#include "discovery.h"
#include "readprop.h"

static struct {
    int     in_flight;
    uint8_t invoke_id;
    int     done;
    int     error;
    char   *out_buf;
    size_t  out_sz;
    char   *err_buf;
    size_t  err_sz;
} g_rpm = { 0 };

static int val_to_json_fragment(char *dst, size_t max, const BACNET_APPLICATION_DATA_VALUE *val)
{
    switch (val->tag) {
    case BACNET_APPLICATION_TAG_REAL:
        return snprintf(dst, max,
            "{\"value\":%.6g,\"type\":\"real\"}", val->type.Real);
    case BACNET_APPLICATION_TAG_UNSIGNED_INT:
        return snprintf(dst, max,
            "{\"value\":%lu,\"type\":\"unsigned\"}", (unsigned long)val->type.Unsigned_Int);
    case BACNET_APPLICATION_TAG_SIGNED_INT:
        return snprintf(dst, max,
            "{\"value\":%d,\"type\":\"signed\"}", val->type.Signed_Int);
    case BACNET_APPLICATION_TAG_BOOLEAN:
        return snprintf(dst, max,
            "{\"value\":%s,\"type\":\"boolean\"}",
            val->type.Boolean ? "true" : "false");
    case BACNET_APPLICATION_TAG_ENUMERATED:
        return snprintf(dst, max,
            "{\"value\":%u,\"type\":\"enumerated\"}", val->type.Enumerated);
    case BACNET_APPLICATION_TAG_CHARACTER_STRING: {
        char tmp[128] = {0};
        characterstring_copy_value(tmp, sizeof(tmp),
                                   &val->type.Character_String);
        return snprintf(dst, max,
            "{\"value\":\"%.60s\",\"type\":\"string\"}", tmp);
    }
    case BACNET_APPLICATION_TAG_OBJECT_ID:
        return snprintf(dst, max,
            "{\"value\":{\"type\":%u,\"type_name\":\"%s\",\"instance\":%u},\"type\":\"object_id\"}",
            val->type.Object_Id.type,
            bactext_object_type_name(val->type.Object_Id.type),
            val->type.Object_Id.instance);
    default:
        return snprintf(dst, max, "{\"value\":null,\"type\":\"unknown\"}");
    }
}

static void rpm_ack_handler(uint8_t *service_request,
                             uint16_t service_len,
                             BACNET_ADDRESS *src,
                             BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    (void)src;
    if (service_data->invoke_id != g_rpm.invoke_id || !g_rpm.in_flight) return;

    char *buf = g_rpm.out_buf;
    size_t max = g_rpm.out_sz;
    size_t pos = 0;

    uint8_t *apdu = service_request;
    int apdu_len = service_len;

    int first_obj = 1;

    while (apdu_len > 0) {
        BACNET_OBJECT_TYPE obj_type = 0;
        uint32_t           obj_inst = 0;

        int hdr_len = rpm_ack_decode_object_id(apdu, apdu_len,
                                                &obj_type, &obj_inst);
        if (hdr_len <= 0) break;
        apdu     += hdr_len;
        apdu_len -= hdr_len;

        pos += snprintf(buf + pos, max - pos,
            "%s{\"obj_type\":%u,\"type_name\":\"%s\",\"obj_instance\":%u,\"properties\":{",
            first_obj ? "" : ",",
            obj_type,
            bactext_object_type_name(obj_type),
            obj_inst);
        first_obj = 0;

        int first_prop = 1;

        while (apdu_len > 0) {
            if (rpm_ack_decode_object_end(apdu, apdu_len) > 0) {
                apdu++; apdu_len--;
                break;
            }

            BACNET_PROPERTY_ID prop_id   = 0;
            BACNET_ARRAY_INDEX array_idx = 0;

            int prop_hdr = rpm_ack_decode_object_property(apdu, apdu_len,
                                                           &prop_id, &array_idx);
            if (prop_hdr <= 0) break;
            apdu     += prop_hdr;
            apdu_len -= prop_hdr;

            pos += snprintf(buf + pos, max - pos,
                "%s\"%u\":", first_prop ? "" : ",", prop_id);
            first_prop = 0;

            int tag_len = 0;
            if (bacnet_is_opening_tag_number(apdu, apdu_len, 4, &tag_len)) {
                apdu += tag_len; apdu_len -= tag_len;

                BACNET_APPLICATION_DATA_VALUE val;
                int val_len = bacapp_decode_application_data(apdu, apdu_len, &val);
                if (val_len > 0) {
                    char frag[256];
                    val_to_json_fragment(frag, sizeof(frag), &val);
                    pos += snprintf(buf + pos, max - pos, "%s", frag);
                    apdu += val_len; apdu_len -= val_len;
                } else {
                    pos += snprintf(buf + pos, max - pos, "{\"value\":null,\"type\":\"decode_error\"}");
                }

                while (apdu_len > 0) {
                    if (bacnet_is_closing_tag_number(apdu, apdu_len, 4, &tag_len)) {
                        apdu += tag_len; apdu_len -= tag_len;
                        break;
                    }
                    apdu++; apdu_len--;
                }
            } else if (bacnet_is_opening_tag_number(apdu, apdu_len, 5, &tag_len)) {
                apdu += tag_len; apdu_len -= tag_len;
                pos += snprintf(buf + pos, max - pos, "{\"value\":null,\"type\":\"error\"}");
                while (apdu_len > 0) {
                    if (bacnet_is_closing_tag_number(apdu, apdu_len, 5, &tag_len)) {
                        apdu += tag_len; apdu_len -= tag_len;
                        break;
                    }
                    apdu++; apdu_len--;
                }
            } else {
                /* Malformed tag or unknown context tag, break out of property loop */
                pos += snprintf(buf + pos, max - pos, "{\"value\":null,\"type\":\"unknown_tag\"}");
                break;
            }
        }
        pos += snprintf(buf + pos, max - pos, "}}");
    }

    g_rpm.done = 1;
}

static void rpm_error_handler(BACNET_ADDRESS *src,
                               uint8_t invoke_id,
                               BACNET_ERROR_CLASS err_class,
                               BACNET_ERROR_CODE err_code)
{
    (void)src;
    if (invoke_id != g_rpm.invoke_id || !g_rpm.in_flight) return;
    snprintf(g_rpm.err_buf, g_rpm.err_sz,
             "BACnet error class=%d code=%d", err_class, err_code);
    g_rpm.error = 1;
    g_rpm.done  = 1;
}

static void rpm_abort_handler(BACNET_ADDRESS *src, uint8_t invoke_id,
                               uint8_t reason, bool server)
{
    (void)src; (void)server;
    if (invoke_id != g_rpm.invoke_id || !g_rpm.in_flight) return;
    snprintf(g_rpm.err_buf, g_rpm.err_sz, "BACnet abort reason=%d", reason);
    g_rpm.error = 1;
    g_rpm.done  = 1;
}

static void rpm_reject_handler(BACNET_ADDRESS *src, uint8_t invoke_id,
                                uint8_t reason)
{
    (void)src;
    if (invoke_id != g_rpm.invoke_id || !g_rpm.in_flight) return;
    snprintf(g_rpm.err_buf, g_rpm.err_sz, "BACnet reject reason=%d", reason);
    g_rpm.error = 1;
    g_rpm.done  = 1;
}

int rpm_read(uint32_t         device_id,
             rpm_object_req_t *req,
             int               req_count,
             char             *out_buf,
             size_t            out_sz,
             char             *err_buf,
             size_t            err_sz,
             int               timeout_ms)
{
    if (g_rpm.in_flight) {
        snprintf(err_buf, err_sz, "busy");
        return -1;
    }

    BACNET_ADDRESS dest;
    unsigned max_apdu = 0;
    bool found = address_get_by_device(device_id, &max_apdu, &dest);
    if (!found) {
        if (discovery_whois_target(device_id, 2000)) {
            found = address_get_by_device(device_id, &max_apdu, &dest);
        }
        if (!found) {
            snprintf(err_buf, err_sz, "device %u not in address cache", device_id);
            return -1;
        }
    }

    BACNET_READ_ACCESS_DATA rad[RPM_MAX_OBJECTS];
    BACNET_PROPERTY_REFERENCE refs[RPM_MAX_OBJECTS][RPM_MAX_PROPS];

    memset(rad,  0, sizeof(rad));
    memset(refs, 0, sizeof(refs));

    for (int i = 0; i < req_count; i++) {
        rad[i].object_type     = req[i].obj_type;
        rad[i].object_instance = req[i].obj_instance;
        rad[i].listOfProperties = refs[i];
        rad[i].next             = (i + 1 < req_count) ? &rad[i + 1] : NULL;

        for (int j = 0; j < req[i].prop_count; j++) {
            refs[i][j].propertyIdentifier = req[i].props[j].prop_id;
            refs[i][j].propertyArrayIndex = BACNET_ARRAY_ALL;
            refs[i][j].next = (j + 1 < req[i].prop_count) ? &refs[i][j + 1] : NULL;
        }
    }

    apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, rpm_ack_handler);
    apdu_set_error_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, rpm_error_handler);
    apdu_set_abort_handler(rpm_abort_handler);
    apdu_set_reject_handler(rpm_reject_handler);

    static uint8_t pdu[MAX_PDU];
    uint8_t invoke_id = Send_Read_Property_Multiple_Request(pdu, sizeof(pdu), device_id, &rad[0]);

    if (invoke_id == 0) {
        snprintf(err_buf, err_sz, "failed to send RPM");
        return -1;
    }

    g_rpm.in_flight = 1;
    g_rpm.invoke_id = invoke_id;
    g_rpm.done      = 0;
    g_rpm.error     = 0;
    g_rpm.out_buf   = out_buf;
    g_rpm.out_sz    = out_sz;
    g_rpm.err_buf   = err_buf;
    g_rpm.err_sz    = err_sz;

    out_buf[0] = '\0';

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    BACNET_ADDRESS src;
    uint8_t rx_buf[MAX_PDU];

    while (!g_rpm.done) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            snprintf(err_buf, err_sz, "timeout waiting for RPM response");
            g_rpm.error = 1;
            break;
        }
        int rx_len = datalink_receive(&src, rx_buf, sizeof(rx_buf), 50);
        if (rx_len > 0) {
            npdu_handler(&src, rx_buf, rx_len);
        }
    }

    if (!g_rpm.done) tsm_free_invoke_id(invoke_id);
    g_rpm.in_flight = 0;

    return g_rpm.error ? -1 : 0;
}
