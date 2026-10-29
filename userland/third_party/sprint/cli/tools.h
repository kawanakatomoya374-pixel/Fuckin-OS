/* sprint CLI tools (fmt, lint, repl). Each returns a process exit code; 2 means "usage error". */
#ifndef SPRINT_TOOLS_H
#define SPRINT_TOOLS_H
int tool_fmt(int argc, char **argv);
int tool_lint(int argc, char **argv);
int tool_repl(int argc, char **argv, int *usage_error); /* exit code of the session */
#endif
