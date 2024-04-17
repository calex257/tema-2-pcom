#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "common.h"
#include "debug.h"

void add_to_epollin(int epoll_inst, int fd)
{
    struct epoll_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.events = EPOLLIN;
    evt.data.fd = fd;
    int rc = epoll_ctl(epoll_inst, EPOLL_CTL_ADD, fd, &evt);
    CHECK(rc < 0, "epoll_ctl", FATAL);
}
