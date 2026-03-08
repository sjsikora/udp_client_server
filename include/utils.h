/*
 * Functions that are simply just pure utilities. These
 * should be indifferent to the UTCP system
 */
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef UTILS_H
#define UTILS_H

void err_sys(const char *x);
void print_safe_chars(uint8_t *buf, size_t len);

#endif
