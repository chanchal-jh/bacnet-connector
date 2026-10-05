#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* bacnet-stack */
#include "bacnet/bacdef.h"
#include "bacnet/apdu.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/datalink/dlenv.h"
#include "bacnet/iam.h"
#include "bacnet/basic/service/h_iam.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/basic/npdu/h_npdu.h"
#include "bacnet/bactext.h"

#include "ipc.h"
#include "discovery.h"
#include "readprop.h"
#include "writeprop.h"
#include "readpropmultiple.h"

/* ------------------------------------------------------------------ */
/* Simple JSON helpers (no external library needed for Step 1)         */
/* ------------------------------------------------------------------ */

/* Extract string value for "key" from flat JSON object.
 * Returns 1 on success, 0 on not found. */
static int json_get_str(const char *json, const char *key,
                         char *out, size_t out_sz)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < out_sz - 1) out[i++] = *p++;
        out[i] = '\0';
        return 1;
    }
    return 0;
}

/* Extract numeric (integer) value for "key". Returns 1 on success. */
static int json_get_int(const char *json, const char *key, long *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    char *end;
    *out = strtol(p, &end, 10);
    return (end != p) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Convert bacnet_value_t strictly to its JSON string representation 
   (e.g. 23.5 or "Room") without the type envelope. */
static const char* val_to_str(char *out, size_t max, const bacnet_value_t *val)
{
    switch (val->type) {
    case BACNET_VAL_REAL:
        snprintf(out, max, "%.6g", val->v.real);
        break;
    case BACNET_VAL_UNSIGNED:
        snprintf(out, max, "%u", val->v.uval);
        break;
    case BACNET_VAL_SIGNED:
        snprintf(out, max, "%d", val->v.ival);
        break;
    case BACNET_VAL_BOOLEAN:
        snprintf(out, max, "%s", val->v.boolean ? "true" : "false");
        break;
    case BACNET_VAL_ENUMERATED:
        snprintf(out, max, "%u", val->v.enumval);
        break;
    case BACNET_VAL_STRING:
        snprintf(out, max, "\"%.60s\"", val->v.str);
        break;
    case BACNET_VAL_OBJECT_ID:
        snprintf(out, max, "{\"type\":%u,\"type_name\":\"%s\",\"instance\":%u}",
                 val->v.obj_id.type,
                 bactext_object_type_name(val->v.obj_id.type),
                 val->v.obj_id.instance);
        break;
    default:
        snprintf(out, max, "null");
        break;
    }
    return out;
}

/* Encode a bacnet_value_t into full JSON fragment: "value":<v>,"type":"<t>" */
static int val_to_json(char *buf, size_t buf_sz, size_t *pos,
                       const bacnet_value_t *val)
{
    const char *type_str = "unknown";
    char        val_str[128];

    val_to_str(val_str, sizeof(val_str), val);

    switch (val->type) {
    case BACNET_VAL_REAL:      type_str = "real"; break;
    case BACNET_VAL_UNSIGNED:  type_str = "unsigned"; break;
    case BACNET_VAL_SIGNED:    type_str = "signed"; break;
    case BACNET_VAL_BOOLEAN:   type_str = "boolean"; break;
    case BACNET_VAL_ENUMERATED:type_str = "enumerated"; break;
    case BACNET_VAL_STRING:    type_str = "string"; break;
    case BACNET_VAL_OBJECT_ID: type_str = "object_id"; break;
    default: return -1;
    }

    *pos += snprintf(buf + *pos, buf_sz - *pos,
                     "\"value\":%s,\"type\":\"%s\"", val_str, type_str);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_discover(int client_fd, const char *req_id)
{
    device_list_t list;
    int count = discovery_whois(&list, 3000); /* 3 s timeout */

    char buf[IPC_BUF_SIZE];
    int  pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":\"%s\",\"status\":\"ok\",\"devices\":[", req_id);

    for (int i = 0; i < count; i++) {
        bacnet_device_t *d = &list.devices[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"id\":%u,\"addr\":\"%s\",\"vendor_id\":%u,\"name\":\"%s\"}",
                        (i > 0) ? "," : "",
                        d->device_id, d->addr, d->vendor_id, d->name);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    ipc_writeline(client_fd, buf);
}

static void handle_read(int client_fd, const char *req_id, const char *json)
{
    long device_id = 0, obj_type = 0, obj_instance = 0, prop_id = 0;

    if (!json_get_int(json, "device",       &device_id)   ||
        !json_get_int(json, "obj_type",     &obj_type)    ||
        !json_get_int(json, "obj_instance", &obj_instance)||
        !json_get_int(json, "prop",         &prop_id)) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\","
                 "\"error\":\"missing fields\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    bacnet_value_t val;
    int rc = readprop_read((uint32_t)device_id,
                           (uint16_t)obj_type,
                           (uint32_t)obj_instance,
                           (uint32_t)prop_id,
                           BACNET_ARRAY_ALL,
                           5000, &val);

    char   buf[IPC_BUF_SIZE];
    size_t pos = 0;

    if (rc != 0 || val.type == BACNET_VAL_ERROR) {
        snprintf(buf, sizeof(buf),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"%s\"}",
                 req_id, val.error_msg);
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{\"id\":\"%s\",\"status\":\"ok\",", req_id);
        val_to_json(buf, sizeof(buf), &pos, &val);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "}");
    }

    ipc_writeline(client_fd, buf);
}

/*
 * device-info: reads the standard Device Object property set in one call.
 * Returns a map keyed by property ID so Go can pass it straight to the UI.
 * Properties that fail (optional / not supported) are silently skipped.
 */
/* ------------------------------------------------------------------ */
/* RPM handler                                                         */
/* ------------------------------------------------------------------ */
/*
 * IPC JSON format:
 *  {"cmd":"rpm","device":3489866,"objects":[
 *    {"obj_type":0,"obj_instance":1,"props":[77,85,117,111]},
 *    {"obj_type":2,"obj_instance":1,"props":[77,85,111]}
 *  ]}
 *
 * The "objects" array is parsed by iterating over the raw JSON using
 * a simple pattern-matching approach (no external JSON library).
 */
static void handle_rpm(int client_fd, const char *req_id, const char *json)
{
    long device_id = 0;
    if (!json_get_int(json, "device", &device_id)) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"missing device\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    /* Parse objects array: scan for each {"obj_type":...,"obj_instance":...,"props":[...]} */
    rpm_object_req_t req[RPM_MAX_OBJECTS];
    int req_count = 0;

    const char *p = strstr(json, "\"objects\"");
    if (!p) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"missing objects array\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }
    p = strchr(p, '[');
    if (!p) { p = ""; }

    while (*p && req_count < RPM_MAX_OBJECTS) {
        /* find next object start */
        p = strchr(p, '{');
        if (!p) break;

        long ot = 0, oi = 0;
        /* Extract obj_type */
        const char *ft = strstr(p, "\"obj_type\"");
        if (!ft) break;
        ft = strchr(ft, ':');
        if (!ft) break;
        ot = strtol(ft + 1, NULL, 10);

        /* Extract obj_instance */
        const char *fi = strstr(p, "\"obj_instance\"");
        if (!fi) break;
        fi = strchr(fi, ':');
        if (!fi) break;
        oi = strtol(fi + 1, NULL, 10);

        req[req_count].obj_type     = (uint16_t)ot;
        req[req_count].obj_instance = (uint32_t)oi;
        req[req_count].prop_count   = 0;

        /* Parse props array */
        const char *fp = strstr(p, "\"props\"");
        if (fp) {
            fp = strchr(fp, '[');
            if (fp) {
                fp++;
                while (*fp && *fp != ']' && req[req_count].prop_count < RPM_MAX_PROPS) {
                    while (*fp == ' ' || *fp == ',') fp++;
                    if (*fp == ']') break;
                    long prop = strtol(fp, (char **)&fp, 10);
                    if (prop > 0 || fp[-1] == '0') {
                        req[req_count].props[req[req_count].prop_count++].prop_id = (uint32_t)prop;
                    } else {
                        break;
                    }
                }
            }
        }

        req_count++;
        p++;  /* advance past current '{' to look for next object */
    }

    if (req_count == 0) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"no valid objects parsed\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    /* Call RPM */
    static char rpm_buf[32768];
    char err_msg[256] = {0};
    rpm_buf[0] = '\0';

    int rc = rpm_read((uint32_t)device_id, req, req_count,
                      rpm_buf, sizeof(rpm_buf),
                      err_msg, sizeof(err_msg),
                      5000);

    static char resp[34000];
    if (rc == 0) {
        snprintf(resp, sizeof(resp),
                 "{\"id\":\"%s\",\"status\":\"ok\",\"objects\":[%s]}", req_id, rpm_buf);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"%s\"}", req_id, err_msg);
    }
    ipc_writeline(client_fd, resp);
}

static void handle_write(int client_fd, const char *req_id, const char *json)
{
    long device_id = 0, obj_type = 0, obj_instance = 0, prop_id = 0, priority = 0;
    char val_type[32] = {0};
    char val_str[128] = {0};

    if (!json_get_int(json, "device",       &device_id)   ||
        !json_get_int(json, "obj_type",     &obj_type)    ||
        !json_get_int(json, "obj_instance", &obj_instance)||
        !json_get_int(json, "prop",         &prop_id)     ||
        !json_get_str(json, "val_type",  val_type, sizeof(val_type)) ||
        !json_get_str(json, "value",     val_str, sizeof(val_str))) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"missing fields\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    if (!json_get_int(json, "priority", &priority)) {
        priority = 0; /* BACNET_NO_PRIORITY */
    }

    wp_val_t val;
    memset(&val, 0, sizeof(val));
    if (strcmp(val_type, "real") == 0) {
        val.type = WP_TYPE_REAL;
        val.v.real = strtof(val_str, NULL);
    } else if (strcmp(val_type, "unsigned") == 0) {
        val.type = WP_TYPE_UNSIGNED;
        val.v.uval = strtoul(val_str, NULL, 10);
    } else if (strcmp(val_type, "signed") == 0) {
        val.type = WP_TYPE_SIGNED;
        val.v.ival = strtol(val_str, NULL, 10);
    } else if (strcmp(val_type, "boolean") == 0) {
        val.type = WP_TYPE_BOOLEAN;
        val.v.boolean = (strcmp(val_str, "true") == 0 || strcmp(val_str, "1") == 0);
    } else if (strcmp(val_type, "enumerated") == 0) {
        val.type = WP_TYPE_ENUMERATED;
        val.v.enumval = strtoul(val_str, NULL, 10);
    } else if (strcmp(val_type, "string") == 0) {
        val.type = WP_TYPE_STRING;
        strncpy(val.v.str, val_str, sizeof(val.v.str));
    } else if (strcmp(val_type, "null") == 0) {
        val.type = WP_TYPE_NULL;
    } else {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"unsupported val_type\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    writeprop_result_t out;
    int rc = writeprop_write(device_id, obj_type, obj_instance, prop_id, BACNET_ARRAY_ALL, priority, &val, 3000, &out);

    char buf[512];
    if (rc == 0) {
        snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"status\":\"ok\"}", req_id);
    } else {
        snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"%s\"}", req_id, out.error_msg);
    }
    ipc_writeline(client_fd, buf);
}

static void handle_device_info(int client_fd, const char *req_id,
                                const char *json)
{
    long device_id = 0;
    if (!json_get_int(json, "device", &device_id)) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\","
                 "\"error\":\"missing device id\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    /* Standard Device Object properties to attempt */
    static const uint32_t prop_ids[] = {
        77,   /* object-name                    */
        28,   /* description                    */
        58,   /* location                       */
        121,  /* vendor-name                    */
        120,  /* vendor-identifier              */
        70,   /* model-name                     */
        44,   /* firmware-revision              */
        12,   /* application-software-version   */
        98,   /* protocol-version               */
        139,  /* protocol-revision              */
        112,  /* system-status                  */
        62,   /* max-apdu-length-accepted       */
        11,   /* apdu-timeout                   */
        73,   /* number-of-apdu-retries         */
        155,  /* database-revision              */
        0     /* sentinel */
    };

    /* Use a larger buffer — all properties in one JSON object */
    char   buf[IPC_BUF_SIZE * 4];
    size_t pos    = 0;
    int    first  = 1;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":\"%s\",\"status\":\"ok\",\"properties\":{",
                    req_id);

    for (int i = 0; prop_ids[i] != 0; i++) {
        bacnet_value_t val;
        int rc = readprop_read((uint32_t)device_id,
                               8,                    /* Device object type */
                               (uint32_t)device_id,  /* Instance = Device ID */
                               prop_ids[i],
                               BACNET_ARRAY_ALL,
                               3000, &val);

        if (rc != 0 || val.type == BACNET_VAL_ERROR) {
            continue; /* property not supported — skip silently */
        }

        char val_str[128];
        val_to_str(val_str, sizeof(val_str), &val);

        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s\"%s\":%s", 
                        first ? "" : ",", 
                        bactext_property_name(prop_ids[i]), 
                        val_str);
        first = 0;
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
    ipc_writeline(client_fd, buf);
}

static void handle_object_list(int client_fd, const char *req_id, const char *json)
{
    long device_id = 0;
    if (!json_get_int(json, "device", &device_id)) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\","
                 "\"error\":\"missing device id\"}", req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    /* 1. Read array_index = 0 to get the count */
    bacnet_value_t count_val;
    int rc = readprop_read((uint32_t)device_id, 8, (uint32_t)device_id, 76, 0, 3000, &count_val);
    
    if (rc != 0 || count_val.type == BACNET_VAL_ERROR) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"failed to read object list size: %s\"}",
                 req_id, count_val.error_msg);
        ipc_writeline(client_fd, err);
        return;
    }

    if (count_val.type != BACNET_VAL_UNSIGNED) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"object list size was not an unsigned integer\"}",
                 req_id);
        ipc_writeline(client_fd, err);
        return;
    }

    uint32_t count = count_val.v.uval;
    
    char   buf[IPC_BUF_SIZE * 4];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":\"%s\",\"status\":\"ok\",\"objects\":[", req_id);

    int first = 1;
    for (uint32_t i = 1; i <= count; i++) {
        bacnet_value_t val;
        rc = readprop_read((uint32_t)device_id, 8, (uint32_t)device_id, 76, i, 3000, &val);
        if (rc == 0 && val.type == BACNET_VAL_OBJECT_ID) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%s{\"type\":%u,\"type_name\":\"%s\",\"instance\":%u}",
                            first ? "" : ",", 
                            val.v.obj_id.type, 
                            bactext_object_type_name(val.v.obj_id.type),
                            val.v.obj_id.instance);
            first = 0;
        }
    }
    
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    ipc_writeline(client_fd, buf);
}

static void dispatch(int client_fd, const char *json)
{
    char req_id[64] = "unknown";
    char cmd[32]    = "";

    json_get_str(json, "id",  req_id, sizeof(req_id));
    json_get_str(json, "cmd", cmd,    sizeof(cmd));

    fprintf(stdout, "[ipc] cmd=%s id=%s\n", cmd, req_id);

    if (strcmp(cmd, "discover") == 0) {
        handle_discover(client_fd, req_id);
    } else if (strcmp(cmd, "read") == 0) {
        handle_read(client_fd, req_id, json);
    } else if (strcmp(cmd, "device-info") == 0) {
        handle_device_info(client_fd, req_id, json);
    } else if (strcmp(cmd, "object-list") == 0) {
        handle_object_list(client_fd, req_id, json);
    } else if (strcmp(cmd, "write") == 0) {
        handle_write(client_fd, req_id, json);
    } else if (strcmp(cmd, "rpm") == 0) {
        handle_rpm(client_fd, req_id, json);
    } else {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"id\":\"%s\",\"status\":\"error\","
                 "\"error\":\"unknown command\"}",
                 req_id);
        ipc_writeline(client_fd, err);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

int main(int argc, char *argv[])
{
    const char *socket_path = IPC_SOCKET_PATH;

    if (argc > 1) socket_path = argv[1];

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* --- BACnet stack init --- */
    /* BACNET_IFACE env var selects network interface (e.g. eth0).
     * BACNET_IP_PORT overrides port (default 47808).              */
    dlenv_init();
    address_init();

    /* Register I-Am handler so discovery can populate address cache */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, my_handler_i_am_add);

    fprintf(stdout, "[main] BACnet stack initialised\n");

    /* --- IPC server --- */
    int server_fd = ipc_server_start(socket_path);
    if (server_fd < 0) {
        fprintf(stderr, "[main] Failed to start IPC server\n");
        return 1;
    }

    fprintf(stdout, "[main] Waiting for connections...\n");

    while (g_running) {
        /* Process any pending BACnet packets (e.g. unsolicited I-Am) */
        uint16_t pdu_len;
        BACNET_ADDRESS src;
        uint8_t rx_buf[MAX_MPDU];
        
        do {
            pdu_len = datalink_receive(&src, rx_buf, sizeof(rx_buf), 0);
            if (pdu_len > 0) {
                npdu_handler(&src, rx_buf, pdu_len);
            }
        } while (pdu_len > 0);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) {
            continue; /* timeout or signal interruption, check g_running */
        }

        int client_fd = ipc_accept(server_fd);
        if (client_fd < 0) continue;

        fprintf(stdout, "[main] Client connected\n");

        char line[IPC_BUF_SIZE];
        int  n;

        while ((n = ipc_readline(client_fd, line, sizeof(line))) > 0) {
            dispatch(client_fd, line);
        }

        close(client_fd);
        fprintf(stdout, "[main] Client disconnected\n");
    }

    close(server_fd);
    unlink(socket_path);
    datalink_cleanup();

    fprintf(stdout, "[main] Shutdown\n");
    return 0;
}
