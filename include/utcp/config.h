#ifndef CONFIG_H
#define CONFIG_H

// This IW calcuation comes from RFC 5681
#define IW_CALC(size) ((size) > 2190 ? 2 : ((size) > 1095 ? 3 : 4))

#define UTCP_SERVER_PORT_NUMBER 1970
#define MAX_LINE                500
#define MAX_UTCP_SOCKETS        6
#define MSS                     1400
#define IW                      IW_CALC(MSS) // Initial congestion control window in segments

#endif
