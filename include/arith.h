#ifndef ARITH_H
#define ARITH_H

#include <stdint.h>

/*
 * Integer expressions for the shell: numbers (decimal or 0x hex), shell variables by name, parentheses and
 * the operators  ! - (unary)   * / %   + -   < <= > >=   == !=   &&   ||   (C precedence; comparisons give 0 or 1).
 * Unset or non-numeric variables count as 0. Returns 0 on success, -1 on a syntax error or division by zero.
 */
int arith_eval(const char *expr, int32_t *result);

#endif
