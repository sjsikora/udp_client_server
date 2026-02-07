#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void err_sys(const char *x) {
    perror(x);
    exit(EXIT_FAILURE);
}

void print_safe_chars(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        printf("%c", isprint(c) ? c : '.'); // non-printable bytes → '.'
    }
    printf("\n");
}
