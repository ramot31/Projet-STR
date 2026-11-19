#include <assert.h>
#include "../common/protocol.h"
#include "../common/types.h"
int main(void) {
    message_t a = { .id = 1042, .type = EV_FLIGHT_DELAY, .origin = "NODE_A", .timestamp_ns = now_ns() };
    strcpy(a.payload, "flight=TU701,delay=20");
    char buf[MSG_MAX]; msg_format(&a, buf, sizeof buf);
    printf("ligne : %s", buf);
    message_t b; assert(msg_parse(buf, &b) == 0);
    assert(b.id == 1042 && b.type == EV_FLIGHT_DELAY && b.timestamp_ns == a.timestamp_ns);
    assert(strcmp(b.payload, "flight=TU701,delay=20") == 0);
    printf("test_protocol OK\n");
    return 0;
}
