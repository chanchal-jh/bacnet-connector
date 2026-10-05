#!/bin/bash
awk '
/handle_device_info/ && !added {
    print "static void handle_write(int client_fd, const char *req_id, const char *json)"
    print "{"
    print "    long device_id = 0, obj_type = 0, obj_instance = 0, prop_id = 0, priority = 0;"
    print "    char val_type[32] = {0};"
    print "    char val_str[128] = {0};"
    print ""
    print "    if (!json_get_int(json, \"device\",       &device_id)   ||"
    print "        !json_get_int(json, \"obj_type\",     &obj_type)    ||"
    print "        !json_get_int(json, \"obj_instance\", &obj_instance)||"
    print "        !json_get_int(json, \"prop\",         &prop_id)     ||"
    print "        !json_get_string(json, \"val_type\",  val_type, sizeof(val_type)) ||"
    print "        !json_get_string(json, \"value\",     val_str, sizeof(val_str))) {"
    print "        char err[256];"
    print "        snprintf(err, sizeof(err),"
    print "                 \"{\\\"id\\\":\\\"%s\\\",\\\"status\\\":\\\"error\\\",\\\"error\\\":\\\"missing fields\\\"}\", req_id);"
    print "        ipc_writeline(client_fd, err);"
    print "        return;"
    print "    }"
    print ""
    print "    if (!json_get_int(json, \"priority\", &priority)) {"
    print "        priority = 0; /* BACNET_NO_PRIORITY */"
    print "    }"
    print ""
    print "    wp_val_t val;"
    print "    memset(&val, 0, sizeof(val));"
    print "    if (strcmp(val_type, \"real\") == 0) {"
    print "        val.type = WP_TYPE_REAL;"
    print "        val.v.real = strtof(val_str, NULL);"
    print "    } else if (strcmp(val_type, \"unsigned\") == 0) {"
    print "        val.type = WP_TYPE_UNSIGNED;"
    print "        val.v.uval = strtoul(val_str, NULL, 10);"
    print "    } else if (strcmp(val_type, \"signed\") == 0) {"
    print "        val.type = WP_TYPE_SIGNED;"
    print "        val.v.ival = strtol(val_str, NULL, 10);"
    print "    } else if (strcmp(val_type, \"boolean\") == 0) {"
    print "        val.type = WP_TYPE_BOOLEAN;"
    print "        val.v.boolean = (strcmp(val_str, \"true\") == 0 || strcmp(val_str, \"1\") == 0);"
    print "    } else if (strcmp(val_type, \"enumerated\") == 0) {"
    print "        val.type = WP_TYPE_ENUMERATED;"
    print "        val.v.enumval = strtoul(val_str, NULL, 10);"
    print "    } else if (strcmp(val_type, \"string\") == 0) {"
    print "        val.type = WP_TYPE_STRING;"
    print "        strncpy(val.v.str, val_str, sizeof(val.v.str));"
    print "    } else if (strcmp(val_type, \"null\") == 0) {"
    print "        val.type = WP_TYPE_NULL;"
    print "    } else {"
    print "        char err[256];"
    print "        snprintf(err, sizeof(err),"
    print "                 \"{\\\"id\\\":\\\"%s\\\",\\\"status\\\":\\\"error\\\",\\\"error\\\":\\\"unsupported val_type\\\"}\", req_id);"
    print "        ipc_writeline(client_fd, err);"
    print "        return;"
    print "    }"
    print ""
    print "    writeprop_result_t out;"
    print "    int rc = writeprop_write(device_id, obj_type, obj_instance, prop_id, BACNET_ARRAY_ALL, priority, &val, 3000, &out);"
    print ""
    print "    char buf[512];"
    print "    if (rc == 0) {"
    print "        snprintf(buf, sizeof(buf), \"{\\\"id\\\":\\\"%s\\\",\\\"status\\\":\\\"ok\\\"}\", req_id);"
    print "    } else {"
    print "        snprintf(buf, sizeof(buf), \"{\\\"id\\\":\\\"%s\\\",\\\"status\\\":\\\"error\\\",\\\"error\\\":\\\"%s\\\"}\", req_id, out.error_msg);"
    print "    }"
    print "    ipc_writeline(client_fd, buf);"
    print "}"
    print ""
    added = 1
}
{ print }
' /home/chanchal/bacnet/bacnetd/main.c > /tmp/main.c.tmp && mv /tmp/main.c.tmp /home/chanchal/bacnet/bacnetd/main.c

awk '
/handle_object_list\(client_fd/ {
    print $0
    print "    } else if (strcmp(cmd, \"write\") == 0) {"
    print "        handle_write(client_fd, req_id, json);"
    next
}
{ print }
' /home/chanchal/bacnet/bacnetd/main.c > /tmp/main.c.tmp && mv /tmp/main.c.tmp /home/chanchal/bacnet/bacnetd/main.c

