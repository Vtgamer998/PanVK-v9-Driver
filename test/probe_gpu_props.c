#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#define KBASE_IOCTL_TYPE 0x80
static int fd;
static int try_props(uint32_t nr, size_t sz, const char *label){
    uint8_t buf[4096];
    unsigned long cmd = _IOC(_IOC_READ, KBASE_IOCTL_TYPE, nr, sz);
    memset(buf,0,sizeof(buf));
    int r = ioctl(fd, cmd, buf);
    uint32_t pid = *(uint32_t*)buf;
    printf("%s (nr=%u sz=%zu): ret=%d errno=%s product_id=0x%08x\n", label, nr, sz, r, r<0?strerror(errno):"ok", pid);
    return r;
}
int main(void){
    fd = open("/dev/mali0", O_RDWR|O_CLOEXEC);
    if(fd<0){perror("open");return 1;}
    uint32_t vc[2]={10,2};
    if(ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE,0x80,0,4), vc)<0) perror("version_check");
    uint32_t fl=0; ioctl(fd, _IOC(_IOC_WRITE,0x80,1,4), &fl);
    printf("kernel v%u.%u\n", vc[0], vc[1]);
    try_props(24, 512, "GPU_PROPS nr=24 sz=512");
    try_props(24, 1024, "GPU_PROPS nr=24 sz=1024");
    try_props(24, 160,  "GPU_PROPS nr=24 sz=160");
    try_props(9,  512,  "GPU_PROPS nr=9 sz=512");
    try_props(9,  256,  "GPU_PROPS nr=9 sz=256");
    close(fd);
    return 0;
}
