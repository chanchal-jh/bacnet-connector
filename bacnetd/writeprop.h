#ifndef WRITEPROP_H
#define WRITEPROP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Result of a WriteProperty request.
 */
typedef struct {
    int  ok;             /* 1 = success (SimpleACK received) */
    char error_msg[128]; /* set when ok == 0 */
} writeprop_result_t;

/*
 * Value types that can be written.
 */
typedef enum {
    WP_TYPE_REAL,
    WP_TYPE_UNSIGNED,
    WP_TYPE_SIGNED,
    WP_TYPE_BOOLEAN,
    WP_TYPE_ENUMERATED,
    WP_TYPE_STRING,
    WP_TYPE_NULL,
} wp_val_type_t;

typedef struct {
    wp_val_type_t type;
    union {
        float    real;
        uint32_t uval;
        int32_t  ival;
        int      boolean;
        uint32_t enumval;
        char     str[128];
    } v;
} wp_val_t;

int writeprop_write(uint32_t          device_id,
                    uint16_t          obj_type,
                    uint32_t          obj_instance,
                    uint32_t          prop_id,
                    uint32_t          array_index,
                    uint8_t           priority,
                    const wp_val_t   *val,
                    int               timeout_ms,
                    writeprop_result_t *out);

#endif /* WRITEPROP_H */
