#ifndef SCRIPT_H
#define SCRIPT_H

/*
 * Shell scripts: a file of commands run one line at a time. Blank lines and lines starting with '#'
 * are skipped, 'exit [n]' stops the script, and the arguments are available as $0 (the script),
 * $1..$9 and $# while it runs. Control flow (each keyword on a line of its own, nesting allowed):
 *     if COMMAND          while COMMAND
 *     then                do
 *       ...                  ... (break, continue)
 *     else                done
 *       ...
 *     fi
 * A command is true when its status is 0; see the shell's "test" and "let" commands for conditions.
 */
#define SCRIPT_MAX_SIZE  16384
#define SCRIPT_MAX_LINES 256
#define SCRIPT_MAX_LOOPS 10000   /* iterations of one while loop before it is stopped */

int         script_run(const char *path, int argc, char **argv); /* the status of the last command, or of 'exit' */
const char *script_param(int n);   /* $n inside a script (NULL when there is no such parameter or no script is running) */
int         script_nargs(void);    /* $# (0 outside a script) */

#endif
