/*
 * Defines some very helpful utility functions for the utcp
 * package. These utils should purely be for debugging and
 * ease of use. If ever these utils would break the system,
 * they belong under a specific named file/folder.
 */

#include <stdbool.h>
#include <utcp/net/tcp.h>

/*
 * @brief Print out the contents of a tcp header
 */
void debug_print_tcp_packet(tcphdr *hdr, bool net_ordered);

/*
 * @brief Helper function to print out the state of a tcb
 */
void dump_tcb(int fd);
