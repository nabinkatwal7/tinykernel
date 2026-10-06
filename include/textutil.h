#ifndef TEXTUTIL_H
#define TEXTUTIL_H

/*
 * Text filters: each reads the files named on its command line, or standard input when it has none
 * (so they work at the end of a pipeline or after '<'). Same calling convention as the shell commands.
 */
int tu_grep(int argc, char **argv);   /* grep [-ivnc] pattern [file...]  ('^' and '$' anchor the pattern) */

int tu_wc(int argc, char **argv);     /* wc [-lwc] [file...]: lines, words and bytes */

int tu_head(int argc, char **argv);   /* head [-n N] [file...]: the first N lines (10) */
int tu_tail(int argc, char **argv);   /* tail [-n N] [file...]: the last N lines (10) */

#endif
