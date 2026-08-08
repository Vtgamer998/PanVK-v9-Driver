#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kbase_winsys.h"

int main(void) {
    setenv("PANVK_WEDGE_MARKER", "/data/data/com.termux/files/usr/tmp/opencode/wedge_test", 1);
    unlink("/data/data/com.termux/files/usr/tmp/opencode/wedge_test");

    printf("no marker -> wedged=%d (expect 0)\n", kbase_wedge_check());

    kbase_wedge_mark();
    printf("after mark  -> wedged=%d (expect 1, same boot)\n", kbase_wedge_check());

    /* Simulate a reboot: overwrite the marker with a fake boot_id. */
    FILE *f = fopen("/data/data/com.termux/files/usr/tmp/opencode/wedge_test", "w");
    fprintf(f, "ffffffff-ffff-ffff-ffff-ffffffffffff\n");
    fclose(f);
    printf("after 'reboot' (fake boot_id) -> wedged=%d (expect 0, self-cleared)\n",
           kbase_wedge_check());
    printf("marker file exists after self-clear? %s\n",
           access("/data/data/com.termux/files/usr/tmp/opencode/wedge_test", F_OK) == 0 ? "yes" : "no");

    /* Poison round trip (dry-run gives us an opaque dev handle) */
    struct kbase_dev *dev = kbase_dev_open(NULL);
    if (!dev) { fprintf(stderr, "open failed\n"); return 1; }
    kbase_dev_set_poisoned(dev, 1);
    printf("poisoned=1 -> %d (expect 1)\n", kbase_dev_poisoned(dev));
    kbase_dev_set_poisoned(dev, 0);
    printf("poisoned=0 -> %d (expect 0)\n", kbase_dev_poisoned(dev));
    kbase_dev_close(dev);

    printf("WEDGE_TEST DONE\n");
    return 0;
}
