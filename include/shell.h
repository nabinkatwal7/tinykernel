#ifndef SHELL_H
#define SHELL_H

void shell_run(void) __attribute__((noreturn));
int  shell_interrupted(void); /* Ctrl+C pressed since the last prompt */
int  shell_exec(const char *line);
int  shell_exec_from_user(const char *line); /* like shell_exec, but refuses 'run' (one program at a time) */ /* run one command line; returns the command's status */

#endif
