/**
 * This file is repsonsible for setting up the UTCP package. It mainly holds
 * all the globals in the package such as the file descriptiors to TCB array
 * and the global UTCP port.
 */
#ifndef UTCP_GLOBALS_H
#define UTCP_GLOBALS_H

#include <utcp/config.h>
#include <utcp/net/tcp.h>

extern struct tcb *utcp_fd_table[MAX_UTCP_SOCKETS];
extern int         udp_fd;
extern int         UDP_PORT; // Host order for the global UDP port

void utcp_package_init(int global_udp_port);

#endif
