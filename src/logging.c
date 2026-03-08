#include "utils.h"
#include "zlog.h"

zlog_category_t *cc_logger = NULL;

int init_zlog(int is_client) {
    int rc = dzlog_init("zlog.conf", is_client ? "client" : "server");

    if (rc) {
        err_sys("zlog init failed! Check if zlog.conf exists in the current directory.\n");
        return -1;
    }

    // Init Structured data logging
    cc_logger = zlog_get_category("cc_data");
    if (!cc_logger) {
        dzlog_error("Failed to get cc_data zlog category");
    }

    zlog_put_mdc("thread_name", "Main");

    dzlog_debug(" ---------- RUN START ----------");
    return 0;
}
