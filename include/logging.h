#include <zlog.h>

extern zlog_category_t *cc_logger;
extern zlog_category_t *rtt_logger;
extern zlog_category_t *lstm_logger;

int init_zlog(int is_client);
