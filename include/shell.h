#ifndef SHELL_H
#define SHELL_H

void shell_run(void) __attribute__((noreturn));
int  shell_exec(const char *line); /* run one command line; returns the command's status */

#endif
