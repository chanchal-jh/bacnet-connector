#ifndef READPROP_H
#define READPROP_H

#include <stdint.h>

typedef enum {
    BACNET_VAL_REAL,
    BACNET_VAL_UNSIGNED,
    BACNET_VAL_SIGNED,
    BACNET_VAL_BOOLEAN,
    BACNET_VAL_ENUMERATED,
    BACNET_VAL_STRING,
    BACNET_VAL_OBJECT_ID,
    BACNET_VAL_ERROR
} bacnet_val_type_t;

typedef struct {
    bacnet_val_type_t type;
    union {
        float    real;
        uint32_t uval;
        int32_t  ival;
        int      boolean;
        uint32_t enumval;
        char     str[128];
        struct {
            uint16_t type;
            uint32_t instance;
        } obj_id;
    } v;
    char error_msg[128]; /* set when type == BACNET_VAL_ERROR */
} bacnet_value_t;

/*
 * Send ReadProperty request to device_id for (obj_type, obj_instance, prop_id).
 * Waits up to timeout_ms for a response.
 * Returns 0 on success, -1 on error/timeout.
 */
int readprop_read(uint32_t       device_id,
                  uint16_t       obj_type,
                  uint32_t       obj_instance,
                  uint32_t       prop_id,
                  uint32_t       array_index,
                  int            timeout_ms,
                  bacnet_value_t *out);

#endif /* READPROP_H */

