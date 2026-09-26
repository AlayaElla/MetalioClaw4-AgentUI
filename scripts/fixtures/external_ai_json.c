#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "metalio_app_json.h"

int main(void) {
    char value[32];
    uint32_t number = 0;

    assert(metalio_app_json_get_string(" { \"mode\" : \"next\" } ", "mode",
                                       value, sizeof(value)));
    assert(strcmp(value, "next") == 0);
    assert(metalio_app_json_get_string("{\"na\\u006de\":\"ok\"}", "name",
                                       value, sizeof(value)));
    assert(strcmp(value, "ok") == 0);
    assert(!metalio_app_json_get_string("{\"mode\":\"a\",\"mode\":\"b\"}",
                                        "mode", value, sizeof(value)));
    assert(!metalio_app_json_get_uint("{\"index\":1.5}", "index", &number));
    assert(!metalio_app_json_get_uint("{\"index\":-1}", "index", &number));

    /* Standard JSON Unicode escapes are emitted as UTF-8, including a pair. */
    assert(metalio_app_json_get_string("{\"text\":\"\\u4f60\\u597d \\ud83d\\ude00\"}",
                                       "text", value, sizeof(value)));
    assert(strcmp(value, "\xe4\xbd\xa0\xe5\xa5\xbd \xf0\x9f\x98\x80") == 0);
    assert(!metalio_app_json_get_string("{\"text\":\"\\u0000\"}", "text",
                                        value, sizeof(value)));
    assert(!metalio_app_json_get_string("{\"text\":\"\\ud83d\"}", "text",
                                        value, sizeof(value)));
    assert(!metalio_app_json_get_string("{\"text\":\"\\ude00\"}", "text",
                                        value, sizeof(value)));

    const char* too_deep =
        "{\"value\":\"ok\",\"nested\":{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":{\"i\":{\"j\":{\"k\":{\"l\":{\"m\":{\"n\":{\"o\":{\"p\":0}}}}}}}}}}}}}}}}";
    assert(!metalio_app_json_get_string(too_deep, "value", value, sizeof(value)));

    puts("external AI JSON parser test passed");
    return 0;
}
