#!/bin/bash
awk '
/BACNET_APPLICATION_DATA_VALUE val;/ {
    print "            int tag_len = 0;"
    print "            if (bacnet_is_opening_tag_number(apdu, apdu_len, 4, &tag_len)) {"
    print "                apdu += tag_len; apdu_len -= tag_len;"
    print ""
    print "                BACNET_APPLICATION_DATA_VALUE val;"
    print "                int val_len = bacapp_decode_application_data(apdu, apdu_len, &val);"
    print "                if (val_len > 0) {"
    print "                    char frag[256];"
    print "                    val_to_json_fragment(frag, sizeof(frag), &val);"
    print "                    pos += snprintf(buf + pos, max - pos, \"%s\", frag);"
    print "                    apdu += val_len; apdu_len -= val_len;"
    print "                } else {"
    print "                    pos += snprintf(buf + pos, max - pos, \"{\\\"value\\\":null,\\\"type\\\":\\\"decode_error\\\"}\");"
    print "                }"
    print ""
    print "                /* Advance past remaining data inside the propertyValue until closing tag 4 */"
    print "                while (apdu_len > 0) {"
    print "                    if (bacnet_is_closing_tag_number(apdu, apdu_len, 4, &tag_len)) {"
    print "                        apdu += tag_len; apdu_len -= tag_len;"
    print "                        break;"
    print "                    }"
    print "                    apdu++; apdu_len--;"
    print "                }"
    print "            } else if (bacnet_is_opening_tag_number(apdu, apdu_len, 5, &tag_len)) {"
    print "                /* Property returned an error */"
    print "                apdu += tag_len; apdu_len -= tag_len;"
    print "                pos += snprintf(buf + pos, max - pos, \"{\\\"value\\\":null,\\\"type\\\":\\\"error\\\"}\");"
    print "                /* Advance until closing tag 5 */"
    print "                while (apdu_len > 0) {"
    print "                    if (bacnet_is_closing_tag_number(apdu, apdu_len, 5, &tag_len)) {"
    print "                        apdu += tag_len; apdu_len -= tag_len;"
    print "                        break;"
    print "                    }"
    print "                    apdu++; apdu_len--;"
    print "                }"
    print "            }"
    skip = 1
    next
}
skip && /pos \+= snprintf.*\"\}\"\);/ {
    skip = 0
    next
}
skip { next }
{ print }
' /home/chanchal/bacnet/bacnetd/readpropmultiple.c > /tmp/rpm.tmp && mv /tmp/rpm.tmp /home/chanchal/bacnet/bacnetd/readpropmultiple.c
