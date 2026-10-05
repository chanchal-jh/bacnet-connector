#ifndef READPROPMULTIPLE_H
#define READPROPMULTIPLE_H

#include <stdint.h>
#include <stddef.h>

#define RPM_MAX_OBJECTS    64
#define RPM_MAX_PROPS      32

/* One property entry to read */
typedef struct {
    uint32_t prop_id;
} rpm_property_t;

/* One object with a list of properties */
typedef struct {
    uint16_t     obj_type;
    uint32_t     obj_instance;
    rpm_property_t props[RPM_MAX_PROPS];
    int          prop_count;
} rpm_object_req_t;

/*
 * Read multiple properties from multiple objects in a single BACnet request.
 *
 *   device_id   – BACnet Device Instance
 *   req         – array of objects (each has a list of property IDs)
 *   req_count   – number of objects in req
 *   out_buf     – caller-supplied buffer to hold the JSON response
 *   out_sz      – size of out_buf
 *   timeout_ms  – max wait ms
 *
 * Writes a JSON fragment like:
 *   [{"obj_type":0,"type_name":"analog-input","obj_instance":1,
 *     "properties":{"77":{"value":"AI-1","type":"string"},...}},...]
 *
 * Returns 0 on success, -1 on error (out_buf will contain an error string).
 */
int rpm_read(uint32_t         device_id,
             rpm_object_req_t *req,
             int               req_count,
             char             *out_buf,
             size_t            out_sz,
             char             *err_buf,
             size_t            err_sz,
             int               timeout_ms);

#endif /* READPROPMULTIPLE_H */

