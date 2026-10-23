#!/usr/bin/env bash
# Unit tests of the plugin logic (text tools, formatter, calculator, grep/head/tail/wc, alias, tidy) against a fake host - no VM needed.
set -e
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -fsanitize=address,undefined -I../../userland/include -o /tmp/plugintest.bin main.c mock.c t_studio.c t_studio2.c t_calc.c t_textutils.c t_alias.c t_tidy.c
/tmp/plugintest.bin
