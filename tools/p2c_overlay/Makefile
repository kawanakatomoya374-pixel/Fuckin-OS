# Makefile for building Py2C ON C-OS with the on-device TinyCC (C-OS Studio's terminal: `make`, `make runtime`, `make install`).
# The upstream GNU Makefile (host builds, tests, tcc-full) is Makefile.gnu. Studio's make has no shell, so object files are named flat
# (build/<file>.o) and every rule is spelled out.  See README.cos.md.

CC = tcc
CFLAGS = -O1 -U__linux__ -DP2C_COS_PORT -DP2C_COS_WINDOW -Iinclude

all: build/python-code-to-c.c-os

runtime: build/libp2crt.a

# the command line tool: converts, `run` (convert + compile with tcc + run), c2py, --self-test, --supported ...
build/python-code-to-c.c-os: build/python_code_to_c_codegen.o build/python_code_to_c_common.o build/python_code_to_c_unicode_tables.o build/python_code_to_c.o build/python_code_to_c_lexer.o build/python_code_to_c_pygame.o build/python_code_to_c_ast.o build/python_code_to_c_astdump.o build/python_code_to_c_parser.o build/python_code_to_c_embed.o build/python_code_to_c_gui.o build/python_code_to_c_platform.o build/python_code_to_c_platform_hosted.o build/python_code_to_c_runtime.o build/python_code_to_c_semantic.o build/c2py.o build/main.o build/python_code_to_c_runtime_locate.o
	$(CC) build/python_code_to_c_codegen.o build/python_code_to_c_common.o build/python_code_to_c_unicode_tables.o build/python_code_to_c.o build/python_code_to_c_lexer.o build/python_code_to_c_pygame.o build/python_code_to_c_ast.o build/python_code_to_c_astdump.o build/python_code_to_c_parser.o build/python_code_to_c_embed.o build/python_code_to_c_gui.o build/python_code_to_c_platform.o build/python_code_to_c_platform_hosted.o build/python_code_to_c_runtime.o build/python_code_to_c_semantic.o build/c2py.o build/main.o build/python_code_to_c_runtime_locate.o -o build/python-code-to-c.c-os

# the runtime library a converted program links against (the same one /system/sdk/lib/libp2crt.a holds)
build/libp2crt.a: build/python_code_to_c_unicode_tables.o build/python_code_to_c_runtime.o build/python_code_to_c_common.o build/python_code_to_c_platform.o build/python_code_to_c_platform_hosted.o build/python_code_to_c_pygame.o
	ar rcs build/libp2crt.a build/python_code_to_c_unicode_tables.o build/python_code_to_c_runtime.o build/python_code_to_c_common.o build/python_code_to_c_platform.o build/python_code_to_c_platform_hosted.o build/python_code_to_c_pygame.o

build/python_code_to_c_codegen.o: src/codegen/python_code_to_c_codegen.c build
	$(CC) $(CFLAGS) -c src/codegen/python_code_to_c_codegen.c -o build/python_code_to_c_codegen.o

build/python_code_to_c_common.o: src/common/python_code_to_c_common.c build
	$(CC) $(CFLAGS) -c src/common/python_code_to_c_common.c -o build/python_code_to_c_common.o

build/python_code_to_c_unicode_tables.o: src/common/python_code_to_c_unicode_tables.c build
	$(CC) $(CFLAGS) -c src/common/python_code_to_c_unicode_tables.c -o build/python_code_to_c_unicode_tables.o

build/python_code_to_c.o: src/core/python_code_to_c.c build
	$(CC) $(CFLAGS) -c src/core/python_code_to_c.c -o build/python_code_to_c.o

build/python_code_to_c_lexer.o: src/lexer/python_code_to_c_lexer.c build
	$(CC) $(CFLAGS) -c src/lexer/python_code_to_c_lexer.c -o build/python_code_to_c_lexer.o

build/python_code_to_c_pygame.o: src/modules/python_code_to_c_pygame.c build
	$(CC) $(CFLAGS) -c src/modules/python_code_to_c_pygame.c -o build/python_code_to_c_pygame.o

build/python_code_to_c_ast.o: src/parser/python_code_to_c_ast.c build
	$(CC) $(CFLAGS) -c src/parser/python_code_to_c_ast.c -o build/python_code_to_c_ast.o

build/python_code_to_c_astdump.o: src/parser/python_code_to_c_astdump.c build
	$(CC) $(CFLAGS) -c src/parser/python_code_to_c_astdump.c -o build/python_code_to_c_astdump.o

build/python_code_to_c_parser.o: src/parser/python_code_to_c_parser.c build
	$(CC) $(CFLAGS) -c src/parser/python_code_to_c_parser.c -o build/python_code_to_c_parser.o

build/python_code_to_c_embed.o: src/platform/python_code_to_c_embed.c build
	$(CC) $(CFLAGS) -c src/platform/python_code_to_c_embed.c -o build/python_code_to_c_embed.o

build/python_code_to_c_gui.o: src/platform/python_code_to_c_gui.c build
	$(CC) $(CFLAGS) -c src/platform/python_code_to_c_gui.c -o build/python_code_to_c_gui.o

build/python_code_to_c_platform.o: src/platform/python_code_to_c_platform.c build
	$(CC) $(CFLAGS) -c src/platform/python_code_to_c_platform.c -o build/python_code_to_c_platform.o

build/python_code_to_c_platform_hosted.o: src/platform/python_code_to_c_platform_hosted.c build
	$(CC) $(CFLAGS) -c src/platform/python_code_to_c_platform_hosted.c -o build/python_code_to_c_platform_hosted.o

build/python_code_to_c_runtime.o: src/runtime/python_code_to_c_runtime.c build
	$(CC) $(CFLAGS) -c src/runtime/python_code_to_c_runtime.c -o build/python_code_to_c_runtime.o

build/python_code_to_c_semantic.o: src/semantic/python_code_to_c_semantic.c build
	$(CC) $(CFLAGS) -c src/semantic/python_code_to_c_semantic.c -o build/python_code_to_c_semantic.o

build/c2py.o: src/tools/c2py.c build
	$(CC) $(CFLAGS) -c src/tools/c2py.c -o build/c2py.o

build/main.o: src/tools/main.c build
	$(CC) $(CFLAGS) -c src/tools/main.c -o build/main.o

build/python_code_to_c_runtime_locate.o: src/tools/python_code_to_c_runtime_locate.c build
	$(CC) $(CFLAGS) -c src/tools/python_code_to_c_runtime_locate.c -o build/python_code_to_c_runtime_locate.o

build:
	mkdir build

install: all runtime
	cp build/python-code-to-c.c-os /bin/python-code-to-c.c-os
	mkdir /system/sdk/lib
	cp build/libp2crt.a /system/sdk/lib/libp2crt.a

clean:
	rm -rf build

.PHONY: all runtime install clean
