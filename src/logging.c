#include "utils.h"
#include "zlog.h"

int init_zlog(int is_client) {
    int rc = dzlog_init("zlog.conf", is_client ? "client" : "server");

    if (rc) {
        err_sys("zlog init failed! Check if zlog.conf exists in the current directory.\n");
        return -1;
    }

    zlog_put_mdc("thread_name", "Main");

    dzlog_debug(" ---------- RUN START ----------");
}
