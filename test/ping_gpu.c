#include <stdio.h>
#include <stdint.h>
#include "kbase_winsys.h"
int main(void) {
    struct kbase_dev *dev = kbase_dev_open("/dev/mali0");
    if (!dev) { printf("PING FAIL open\n"); return 1; }
    uint32_t atom=0, code=0;
    printf("PING: waiting for any kernel event...\n");
    int ret = kbase_wait_event_timeout(dev, &atom, &code, 500, 0);
    printf("PING: ret=%d (0=event, -11=timeout/no event) atom=%u code=0x%x\n", ret, atom, code);
    kbase_dev_close(dev);
    printf(ret==0 ? "EVENT_FLOW_OPEN\n" : "NO_EVENT\n");
    return 0;
}
