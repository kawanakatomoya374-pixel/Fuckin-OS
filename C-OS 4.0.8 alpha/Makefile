# C-OS 4.0.8 alpha Makefile - 64-bit Build
.DEFAULT_GOAL := iso

# Build with: make
# Requires: gcc (x86_64), nasm, ld, grub-mkrescue, xorriso

# ---- Toolchain ----
CC      = gcc
CXX     = g++
AS      = nasm
LD      = ld
OBJCOPY = objcopy
GO      = go
PACK_STORAGE = $(BUILD_DIR)/pack_storage
COSNET = $(BUILD_DIR)/cosnet
COSNET_STATE = $(BUILD_DIR)/cosnet_state.h
TEXT_BROWSER = $(BUILD_DIR)/text_browser

# ---- Compiler flags (optimized 64-bit freestanding) ----
# A dedicated validation image may enable the in-guest HTTP/2+Brotli smoke
# worker. Production/normal Secure Boot artifacts remain at 0 by default.
HTTP_RUNTIME_SMOKE ?= 0
# Loads the storage-backed local QuickJS document as the Browser start page in
# validation-only media. Normal artifacts remain at 0.
BROWSER_FILE_SMOKE ?= 0
# When BROWSER_FILE_SMOKE=1, tests may override this with any public HTTPS URL
# (for example https://www.google.com/) without changing browser source code.
BROWSER_SMOKE_START_URL ?= file:///browser/index.html
CFLAGS  = -m64 -ffreestanding -nostdlib -fno-stack-protector -O0 -g \
          	-DCOS_LUA_LINKED=1 -DCOS_HTTP_RUNTIME_SMOKE=$(HTTP_RUNTIME_SMOKE) \
          	-DCOS_BROWSER_FILE_SMOKE=$(BROWSER_FILE_SMOKE) \
          	-DCOS_BROWSER_SMOKE_START_URL=\"$(BROWSER_SMOKE_START_URL)\" \
	-Isrc/fs \
	-fno-pic -fno-pie -fno-strict-aliasing -fno-builtin -mcmodel=kernel \
	-mno-red-zone -msse2 -mfpmath=sse \
	-O2 -Wall -Wextra -Wno-unused-parameter \
	-Wno-unused-function -Wno-sign-compare \
	-Ibuild -Ibuild/genhdr \
	-Isrc/third_party/micropython -Isrc/third_party/micropython/py \
	-Isrc/kernel/ai -Isrc/kernel/python \
	\
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	-D__x86_64__ -D__LP64__ -DCOS_KERNEL -DCOS_ENABLE_FULL_DESKTOP=1 \
	-DBR_RDRAND=0 -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 \
	-MMD -MP

# ---- Host compiler flags (tools only) ----
HOST_CFLAGS = -O2 -Wall -Wextra
CXXFLAGS = -m64 -ffreestanding -nostdlib -fno-stack-protector -O0 -g \
	-fno-pic -fno-pie -fno-strict-aliasing -fno-builtin -mcmodel=kernel \
	-mno-red-zone -msse2 -mfpmath=sse \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
	-O2 -Wall -Wextra -Wno-unused-parameter \
	-Wno-unused-function -Wno-sign-compare \
	\
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	-D__x86_64__ -D__LP64__ -DCOS_KERNEL -DCOS_ENABLE_FULL_DESKTOP=1 \
	-MMD -MP

# ---- Include paths ----
INCLUDES =  -Isrc/include -Ibuild -I$(BUILD_DIR) -I$(SRCROOT)/include -I$(SRCROOT)/lib -I$(SRCROOT)/kernel -I$(SRCROOT)/kernel/arch \
	-I$(SRCROOT)/drivers/video -I$(SRCROOT)/drivers/input -I$(SRCROOT)/drivers/disk \
	-I$(SRCROOT)/kernel/drivers -I$(SRCROOT)/fs -I$(SRCROOT)/gui -I$(SRCROOT)/gui/core -I$(SRCROOT)/gui/browser -I$(SRCROOT)/gui/file_manager -I$(SRCROOT)/gui/components -I$(SRCROOT)/gui/apps -I$(SRCROOT)/gui/apps/system -I$(SRCROOT)/gui/apps/tools -I$(SRCROOT)/gui/apps/development -I$(SRCROOT)/gui/apps/games -I$(SRCROOT)/gui/apps/ui -I$(SRCROOT)/gui/apps/common \
	-I$(SRCROOT)/bios -I$(SRCROOT)/fs \
	-I$(SRCROOT)/third_party/bearssl/inc -I$(SRCROOT)/third_party/bearssl/src -I$(SRCROOT)/third_party/minimp3 \
	-I$(SRCROOT)/third_party/tinygl/include -I$(SRCROOT)/third_party/tinygl/src \
	-I$(SRCROOT)/third_party/micropython -I$(SRCROOT)/kernel/ai -I$(SRCROOT)/third_party/openssl/include \
	-I$(SRCROOT)/third_party/acpica/source/include \
		-I$(SRCROOT)/kernel/power \
		-I$(SRCROOT)/netsurf \
		-I$(NSALL_DIR)/libdom/include -I$(NSALL_DIR)/libwapcaplet/include \

# ---- TinyUSB (host mode: EHCI + HID keyboard/mouse - see tusb_config.h) ----
TINYUSB_DIR = $(SRCROOT)/third_party/tinyusb/SRC/src
# tusb_config.h lives alongside the board glue in kernel/drivers, not
# inside the vendored library tree, per TinyUSB's usual convention of
# the *integrator* supplying that header.
TINYUSB_INCLUDES = -I$(TINYUSB_DIR) -I$(SRCROOT)/include -I$(SRCROOT)/kernel/drivers


# ---- NASM flags (64-bit ELF) ----
ASFLAGS = -f elf64


# ---- Directories ----
OBJ_DIR   = obj
BUILD_DIR = build
SRCROOT := $(if $(wildcard src/boot/linker.ld),src,.)
KERNEL    = kernel.elf
ISO       = C-OS_4.0.8_alpha.iso
# Secure Boot output is intentionally separate from the conventional hybrid
# ISO. Signing credentials must be supplied explicitly from outside this tree.
SECUREBOOT_DIR ?= $(BUILD_DIR)/secureboot
SECUREBOOT_KEY ?=
SECUREBOOT_CERT ?=
STORAGE_IMG = $(BUILD_DIR)/storage.img
STORAGE_SIZE_MB = 1536
GRUB_DIR  = iso/boot/grub
GRUB_CFG  = iso/boot/grub/grub.cfg


# ---- MicroPython build tools ----
PYTHON ?= python3
CAT ?= cat
SED ?= sed
CPP ?= cpp

# Define before any object prerequisites use it
MP_SRC_DIR = $(SRCROOT)/third_party/micropython
MP_SRC_PY = $(MP_SRC_DIR)/py
MICROPY_GENHDR_DIR = $(BUILD_DIR)/genhdr
MICROPY_INC := -Isrc/third_party/micropython -Isrc/third_party/micropython/py -Isrc/kernel/ai -Isrc/kernel/python

MPCONFIGPORT_MK = $(wildcard mpconfigport.mk)


# ---- Object files ----
OBJS = \
	$(OBJ_DIR)/netsurf/cos_fetch.o \
	$(OBJ_DIR)/netsurf/cos_fetch_file.o \
	$(OBJ_DIR)/netsurf/cos_fetch_http.o \
	$(OBJ_DIR)/netsurf/cos_netsurf.o \
	$(OBJ_DIR)/netsurf/cos_gui_table.o \
	$(OBJ_DIR)/netsurf/cos_netsurf_adapter.o \
	$(OBJ_DIR)/netsurf/cos_netsurf_browser.o \
		$(OBJ_DIR)/netsurf/cos_netsurf_render.o \
		$(OBJ_DIR)/netsurf/cos_netsurf_png.o \
	$(OBJ_DIR)/netsurf/cos_netsurf_jpeg.o \
	$(OBJ_DIR)/netsurf/cos_urldb_stub.o \
		$(OBJ_DIR)/netsurf/cos_nsoptions.o \
		$(OBJ_DIR)/netsurf/cos_netsurf_tierb_stubs.o \
		$(OBJ_DIR)/netsurf/cos_parserutils_filter_stub.o \
	$(OBJ_DIR)/boot/boot.o \
	$(OBJ_DIR)/boot/irq.o \
	$(OBJ_DIR)/boot/isr.o \
	$(OBJ_DIR)/boot/storage_blob.o \
	$(OBJ_DIR)/boot/smp_trampoline.o \
	$(OBJ_DIR)/kernel/context_switch.o \
	$(OBJ_DIR)/kernel/kernel.o \
	$(OBJ_DIR)/kernel/io.o \
	$(OBJ_DIR)/kernel/idt.o \
	$(OBJ_DIR)/kernel/syscall.o \
	$(OBJ_DIR)/kernel/userspace_demo.o \
	$(OBJ_DIR)/kernel/serial.o \
	$(OBJ_DIR)/kernel/timer.o \
	$(OBJ_DIR)/kernel/scheduler.o \
	$(OBJ_DIR)/kernel/smp.o \
	$(OBJ_DIR)/kernel/task.o \
	$(OBJ_DIR)/kernel/sync.o \
	$(OBJ_DIR)/kernel/ipc.o \
	$(OBJ_DIR)/kernel/mm/paging.o \
	$(OBJ_DIR)/kernel/rtc.o \
	$(OBJ_DIR)/kernel/api/cos_api.o \
	$(OBJ_DIR)/kernel/api/fs_api.o \
	$(OBJ_DIR)/kernel/api/net_api.o \
	$(OBJ_DIR)/kernel/settings_manager.o \
	$(OBJ_DIR)/kernel/page_fault.o \
	$(OBJ_DIR)/kernel/arch/gdt.o \
	$(OBJ_DIR)/kernel/memory.o \
	$(OBJ_DIR)/kernel/memory_virtual.o \
	$(OBJ_DIR)/kernel/memory_physical.o \
	$(OBJ_DIR)/kernel/irq.o \
	$(OBJ_DIR)/kernel/irq_handlers.o \
	$(OBJ_DIR)/kernel/shell.o \
	$(OBJ_DIR)/kernel/system/mk_mp3_backend.o \
	$(OBJ_DIR)/third_party/minimp3/minimp3.o \
	$(OBJ_DIR)/bios/bios.o \
	$(OBJ_DIR)/kernel/system/storage_manager.o \
	$(OBJ_DIR)/kernel/security/permission_manager.o \
	$(OBJ_DIR)/kernel/system/mk_storage_compat.o \
		$(OBJ_DIR)/kernel/ai/mphalport.o \
		$(OBJ_DIR)/kernel/ai/micropython_repl.o \
	$(OBJ_DIR)/kernel/ai/micropython_bridge.o \
	$(OBJ_DIR)/kernel/ai/micropython_port.o \
		$(OBJ_DIR)/kernel/ai/micropython_vfs.o \
			$(OBJ_DIR)/kernel/ai/micropython_debug.o \
			$(OBJ_DIR)/kernel/ai/lua_config.o \
			$(OBJ_DIR)/kernel/ai/lua_bridge.o \
			$(MICROPY_OBJS) \
	$(OBJ_DIR)/drivers/input/keyboard.o \
	$(OBJ_DIR)/drivers/video/vga.o \
		$(OBJ_DIR)/drivers/video/font24x24_data.o \
		$(OBJ_DIR)/drivers/video/jp_font16.o \
	$(OBJ_DIR)/drivers/video/gfx_blit.o \
	$(OBJ_DIR)/drivers/video/gfx_blit_avx2.o \
	$(TINYGL_OBJS) \
	$(OBJ_DIR)/gui/core/output/gui_render_engine.o \
	$(OBJ_DIR)/gui/core/gui_lifecycle.o \
	$(OBJ_DIR)/gui/core/window/gui_settings.o \
	$(OBJ_DIR)/gui/core/output/gui_desktop_render.o \
	$(OBJ_DIR)/gui/core/output/gui_taskbar_frame.o \
	$(OBJ_DIR)/gui/core/window/gui_menus.o \
	$(OBJ_DIR)/gui/core/input/gui_input.o \
	$(OBJ_DIR)/gui/core/gui_render_loop.o \
	$(OBJ_DIR)/gui/notification_center.o \
	$(OBJ_DIR)/gui/theme_system.o \
	$(OBJ_DIR)/gui/components/boot_animation.o \
	$(OBJ_DIR)/gui/apps/ui/modern_ui.o \
	$(OBJ_DIR)/gui/core/window/gui_create_window.o \
	$(OBJ_DIR)/gui/apps/common/gui_apps_common.o \
	$(OBJ_DIR)/gui/apps/core/gui_apps_core.o \
	$(OBJ_DIR)/gui/apps/terminal/gui_apps_terminal.o \
	$(OBJ_DIR)/gui/apps/calculator/gui_apps_calculator.o \
	$(OBJ_DIR)/gui/apps/settings/gui_apps_settings.o \
	$(OBJ_DIR)/gui/apps/about/gui_apps_about.o \
	$(OBJ_DIR)/gui/apps/browser/gui_apps_browser.o \
	$(OBJ_DIR)/gui/apps/taskmanager/gui_apps_taskmanager.o \
	$(OBJ_DIR)/gui/apps/paint/gui_apps_paint.o \
	$(OBJ_DIR)/gui/apps/clock/gui_apps_clock.o \
	$(OBJ_DIR)/gui/apps/storage/gui_apps_storage.o \
	$(OBJ_DIR)/gui/apps/memory/gui_apps_memory.o \
	$(OBJ_DIR)/gui/apps/birthday/gui_apps_birthday.o \
	$(OBJ_DIR)/gui/apps/music/gui_apps_music.o \
	$(OBJ_DIR)/gui/apps/games/voxel_games_advanced.o \
	$(OBJ_DIR)/gui/apps/games/tinygl_viewer.o \
	$(OBJ_DIR)/gui/apps/games/games2d.o \
	$(OBJ_DIR)/gui/core/gui_utils.o \
	$(OBJ_DIR)/gui/apps/tools/calc_school_math.o \
	$(OBJ_DIR)/kernel/calc_engine.o \
	$(OBJ_DIR)/gui/core/window/gui_windows.o \
	$(OBJ_DIR)/gui/apps/development/python_ide_gui.o \
	$(OBJ_DIR)/gui/apps/tools/http_downloader_gui.o \
	$(OBJ_DIR)/gui/apps/system/password_screen.o \
	$(OBJ_DIR)/gui/apps/system/password_screen_enhanced.o \
	$(OBJ_DIR)/gui/browser/modern_browser.o \
	$(OBJ_DIR)/gui/browser/cos_browser.o \
	$(OBJ_DIR)/gui/components/enhanced_context_menu.o \
	$(OBJ_DIR)/gui/file_manager/efm_util.o \
	$(OBJ_DIR)/gui/file_manager/efm_core.o \
	$(OBJ_DIR)/gui/file_manager/efm_fileops.o \
	$(OBJ_DIR)/gui/file_manager/efm_search_bookmark.o \
	$(OBJ_DIR)/gui/file_manager/efm_thumbnail_preview.o \
	$(OBJ_DIR)/gui/file_manager/efm_render.o \
	$(OBJ_DIR)/gui/file_manager/efm_input.o \
	$(OBJ_DIR)/gui/core/input/input_manager.o \
	$(OBJ_DIR)/gui/core/output/gui_renderer.o \
	$(OBJ_DIR)/gui/file_manager/file_manager_explorer.o \
	$(OBJ_DIR)/gui/core/gui_file_manager_enhanced_bridge.o \
	$(OBJ_DIR)/drivers/input/mouse.o \
	$(OBJ_DIR)/fs/fs.o \
	$(OBJ_DIR)/third_party/fatfs/ff.o \
	$(OBJ_DIR)/third_party/fatfs/ffunicode.o \
	$(OBJ_DIR)/third_party/fatfs/diskio.o \
	$(OBJ_DIR)/third_party/fatfs/ffsystem.o \
	$(OBJ_DIR)/fs/fs_unified.o \
	$(OBJ_DIR)/fs/text_editor.o \
	$(OBJ_DIR)/drivers/disk/storage.o \
	$(OBJ_DIR)/drivers/disk/ide.o \
	$(OBJ_DIR)/drivers/disk/crc32_common.o \
	$(OBJ_DIR)/kernel/drivers/arp.o \
	$(OBJ_DIR)/kernel/drivers/dhcp.o \
	$(OBJ_DIR)/kernel/drivers/dns.o \
	$(OBJ_DIR)/kernel/drivers/udp.o \
	$(OBJ_DIR)/kernel/drivers/tcp.o \
	$(OBJ_DIR)/kernel/drivers/http.o \
	$(OBJ_DIR)/kernel/drivers/http2_client.o \
	$(OBJ_DIR)/kernel/drivers/brotli_decoder.o \
	$(TLS_BACKEND_OBJ) \
	$(BROTLI_OBJS) \
	$(OBJ_DIR)/kernel/drivers/pci.o \
	$(OBJ_DIR)/kernel/drivers/e1000.o \
	$(OBJ_DIR)/kernel/drivers/ac97.o \
	$(OBJ_DIR)/kernel/drivers/net.o \
	$(OBJ_DIR)/kernel/filemanager/mk_advanced_filemanager.o \
	$(OBJ_DIR)/cpp/browser_core.o \
	$(OBJ_DIR)/cpp/module_registry.o \
	$(OBJ_DIR)/cpp/storage_manager.o \
	$(OBJ_DIR)/lib/string.o \
	$(OBJ_DIR)/lib/cos_itoa.o \
		$(OBJ_DIR)/lib/cos_string_util.o \
	$(OBJ_DIR)/lib/ctype.o \
	$(OBJ_DIR)/lib/cos_assert.o \
	$(OBJ_DIR)/lib/math.o \
	$(OBJ_DIR)/lib/cos_pthread_stub.o \
	$(OBJ_DIR)/lib/time_impl.o \
	$(OBJ_DIR)/lib/cos_http_date.o \
	$(OBJ_DIR)/lib/cos_nslog.o \
		$(OBJ_DIR)/apps/config_manager.o \
	$(OBJ_DIR)/apps/jpeg_viewer.o \
	$(OBJ_DIR)/apps/png_decoder.o \
	$(OBJ_DIR)/assets/desktop_featured_png.o \
	$(OBJ_DIR)/assets/sample_icon_bmp.o \
	$(OBJ_DIR)/assets/sample_beep_mp3.o \
	$(OBJ_DIR)/assets/sample_photo_jpg.o \
	$(OBJ_DIR)/assets/sample_beep_wav.o \
	$(OBJ_DIR)/kernel/power/acpica_backend.o \
	$(OBJ_DIR)/kernel/power/acpi_power.o \
	$(OBJ_DIR)/kernel/drivers/usb.o \
	$(OBJ_DIR)/kernel/drivers/tusb_port_pc.o \
	$(OBJ_DIR)/kernel/drivers/tusb_hid_bridge.o \
	$(OBJ_DIR)/kernel/drivers/tusb_msc_bridge.o \
	$(OBJ_DIR)/third_party/tinyusb/tusb.o \
	$(OBJ_DIR)/third_party/tinyusb/tusb_fifo.o \
	$(OBJ_DIR)/third_party/tinyusb/usbh.o \
	$(OBJ_DIR)/third_party/tinyusb/hub.o \
	$(OBJ_DIR)/third_party/tinyusb/hid_host.o \
	$(OBJ_DIR)/third_party/tinyusb/msc_host.o \
	$(OBJ_DIR)/third_party/tinyusb/ehci.o \

# ---- Compatibility / legacy stubs (kept out of the core build graph) ----
# These sources are linked through a static archive so production objects win first,
# and fallback symbols are only pulled in when something in the core build still
# needs them. Broken source-only legacy files are intentionally excluded here.
COMPAT_OBJS = \
	$(OBJ_DIR)/compat/memory_stubs.o \
	$(OBJ_DIR)/compat/malloc_stub.o \
	$(OBJ_DIR)/compat/vga_stub.o \
	$(OBJ_DIR)/compat/serial_stub.o \
	$(OBJ_DIR)/compat/storage_binary_stub.o \
	$(OBJ_DIR)/compat/kernel_stub.o \
	$(OBJ_DIR)/compat/storage_partition_stub.o \


COMPAT_LIB = $(BUILD_DIR)/compat_stubs.a
TINYGL_DIR = $(SRCROOT)/third_party/tinygl
TINYGL_SRC_DIR = $(TINYGL_DIR)/src
TINYGL_SRCS = $(filter-out $(TINYGL_SRC_DIR)/glx.c $(TINYGL_SRC_DIR)/nglx.c,$(wildcard $(TINYGL_SRC_DIR)/*.c))
TINYGL_OBJS = $(patsubst $(TINYGL_SRC_DIR)/%.c,$(OBJ_DIR)/third_party/tinygl/%.o,$(TINYGL_SRCS))
BEARSSL_DIR = $(SRCROOT)/third_party/bearssl
BEARSSL_SRC_DIR = $(BEARSSL_DIR)/src
BEARSSL_SRCS := $(shell find $(BEARSSL_SRC_DIR) -name '*.c' | sort)
BEARSSL_OBJS := $(patsubst $(BEARSSL_SRC_DIR)/%.c,$(OBJ_DIR)/bearssl/%.o,$(BEARSSL_SRCS))
BEARSSL_LIB = $(BUILD_DIR)/libbearssl.a
# Brotli decoder only. Encoder sources are intentionally excluded from the
# freestanding kernel image; HTTP needs Content-Encoding: br decompression.
BROTLI_DIR = $(SRCROOT)/third_party/brotli
BROTLI_COMMON_SRCS := $(wildcard $(BROTLI_DIR)/c/common/*.c)
BROTLI_DEC_SRCS := $(wildcard $(BROTLI_DIR)/c/dec/*.c)
BROTLI_SRCS := $(BROTLI_COMMON_SRCS) $(BROTLI_DEC_SRCS)
BROTLI_OBJS := $(patsubst $(BROTLI_DIR)/c/%.c,$(OBJ_DIR)/brotli/%.o,$(BROTLI_SRCS))
BROTLI_INCLUDES = -I$(BROTLI_DIR)/c/include -I$(BROTLI_DIR)/c/common -I$(BROTLI_DIR)/c/dec
# nghttp2 lib-only vendor build. Applications, command-line tools and server
# code are deliberately excluded; this archive provides RFC-compliant framing
# and HPACK for the C-OS HTTPS transport adapter.
NGHTTP2_DIR = $(SRCROOT)/third_party/nghttp2
NGHTTP2_SRC_DIR = $(NGHTTP2_DIR)/lib
NGHTTP2_SRCS := $(addprefix $(NGHTTP2_SRC_DIR)/,nghttp2_pq.c nghttp2_map.c nghttp2_queue.c nghttp2_frame.c nghttp2_buf.c nghttp2_stream.c nghttp2_outbound_item.c nghttp2_session.c nghttp2_submit.c nghttp2_helper.c nghttp2_alpn.c nghttp2_hd.c nghttp2_hd_huffman.c nghttp2_hd_huffman_data.c nghttp2_version.c nghttp2_priority_spec.c nghttp2_option.c nghttp2_callbacks.c nghttp2_mem.c nghttp2_http.c nghttp2_rcbuf.c nghttp2_extpri.c nghttp2_ratelim.c nghttp2_time.c nghttp2_debug.c sfparse.c)
NGHTTP2_OBJS := $(patsubst $(NGHTTP2_SRC_DIR)/%.c,$(OBJ_DIR)/nghttp2/%.o,$(NGHTTP2_SRCS))
NGHTTP2_LIB = $(BUILD_DIR)/libnghttp2.a
NGHTTP2_INCLUDES = -I$(NGHTTP2_SRC_DIR)/includes -I$(NGHTTP2_SRC_DIR)

# ---- NetSurf core libraries (ported one at a time - see project notes
# for which ones are done). Built straight out of the trimmed
# third_party/netsurf-all-3.11/ tree rather than copied elsewhere, so a
# future upstream re-sync only has to touch this Makefile section. ----
NSALL_DIR = $(SRCROOT)/third_party/netsurf-all-3.11
NS_DIR = $(NSALL_DIR)/netsurf

LWAPCAPLET_DIR = $(NSALL_DIR)/libwapcaplet
LWAPCAPLET_SRCS := $(wildcard $(LWAPCAPLET_DIR)/src/*.c)
LWAPCAPLET_OBJS := $(patsubst $(LWAPCAPLET_DIR)/src/%.c,$(OBJ_DIR)/libwapcaplet/%.o,$(LWAPCAPLET_SRCS))
LWAPCAPLET_LIB = $(BUILD_DIR)/liblwapcaplet.a
LWAPCAPLET_INCLUDES = -I$(LWAPCAPLET_DIR)/include

# input/filter.c depends on iconv, which hasn't been ported (nothing
# else in this library or its dependents calls into it - see project
# notes) - excluded from the glob rather than deleted, so a future
# iconv port can just stop filtering it back in.
LPARSERUTILS_DIR = $(NSALL_DIR)/libparserutils
LPARSERUTILS_SRCS := $(filter-out $(LPARSERUTILS_DIR)/src/input/filter.c,$(shell find $(LPARSERUTILS_DIR)/src -name '*.c'))
LPARSERUTILS_OBJS := $(patsubst $(LPARSERUTILS_DIR)/src/%.c,$(OBJ_DIR)/libparserutils/%.o,$(LPARSERUTILS_SRCS))
LPARSERUTILS_LIB = $(BUILD_DIR)/liblparserutils.a
LPARSERUTILS_INCLUDES = -I$(LPARSERUTILS_DIR)/include -I$(LPARSERUTILS_DIR)/src

# libutf8proc: standalone (no dependency on the other netsurf-all
# libs, and nothing here depends on it yet either - it's staged ahead
# of libcss/libdom, which both need Unicode case-folding). Just two
# source files upstream: utf8proc.c and the ~1.6MB generated table
# utf8proc_data.c; both compile clean against the freestanding
# toolchain as-is, no codegen step needed.
LUTF8PROC_DIR = $(NSALL_DIR)/libutf8proc
# utf8proc_data.c (the ~1.6MB generated table) is #included directly
# by utf8proc.c (see its line 53) rather than being its own
# translation unit - upstream's own src/Makefile confirms this with
# `DIR_SOURCES := utf8proc.c` alone. Compiling it separately fails
# (it uses utf8proc.h's typedefs without including that header) and
# cascades into thousands of spurious errors, so it's filtered out
# here rather than globbed in.
LUTF8PROC_SRCS := $(filter-out $(LUTF8PROC_DIR)/src/utf8proc_data.c,$(wildcard $(LUTF8PROC_DIR)/src/*.c))
LUTF8PROC_OBJS := $(patsubst $(LUTF8PROC_DIR)/src/%.c,$(OBJ_DIR)/libutf8proc/%.o,$(LUTF8PROC_SRCS))
LUTF8PROC_LIB = $(BUILD_DIR)/liblutf8proc.a
LUTF8PROC_INCLUDES = -I$(LUTF8PROC_DIR)/include/libutf8proc

# libhubbub (HTML5 tokeniser + tree builder): depends on libwapcaplet
# + libparserutils, both already ported above. Two files are
# generated ahead of time and vendored as regular source rather than
# regenerated at C-OS build time (same spirit as utf8proc_data.c):
#   - src/tokeniser/entities.inc, from build/make-entities.pl acting
#     on build/Entities (needs perl, host-only, run once)
#   - src/treebuilder/autogenerated-element-type.c, from gperf acting
#     on src/treebuilder/element-type.gperf (needs gperf, host-only,
#     run once): `gperf --output-file=X.tmp element-type.gperf`, then
#     `sed -e 's/^\(const struct element_type_map\)/static \1/' X.tmp
#     > autogenerated-element-type.c` (the sed step matches upstream's
#     own recipe in src/treebuilder/Makefile - it's what makes the
#     table static so it's safe for element-type.c to #include it).
# entities.inc is #included by tokeniser/entities.c;
# autogenerated-element-type.c is #included by treebuilder/
# element-type.c - like utf8proc_data.c, neither is its own
# translation unit, so both are filtered out of the source glob.
# The debug-only element_stack_dump()/formatting_list_dump()
# (#ifndef NDEBUG, using fprintf/FILE*) are compiled out via
# LHUBBUB_CFLAGS so this doesn't need a real hosted stdio.
LHUBBUB_DIR = $(NSALL_DIR)/libhubbub
LHUBBUB_SRCS := $(filter-out $(LHUBBUB_DIR)/src/treebuilder/autogenerated-element-type.c,$(wildcard $(LHUBBUB_DIR)/src/*.c $(LHUBBUB_DIR)/src/*/*.c))
LHUBBUB_OBJS := $(patsubst $(LHUBBUB_DIR)/src/%.c,$(OBJ_DIR)/libhubbub/%.o,$(LHUBBUB_SRCS))
LHUBBUB_LIB = $(BUILD_DIR)/liblhubbub.a
LHUBBUB_INCLUDES = -I$(LHUBBUB_DIR)/include -I$(LHUBBUB_DIR)/src -I$(LPARSERUTILS_DIR)/include
LHUBBUB_CFLAGS = -DNDEBUG

# libcss (CSS parsing + selection/cascade): depends on libwapcaplet +
# libparserutils, both already ported. Needed two rounds of one-time
# code generation, run in this sandbox and vendored as regular source
# (same "generate once" spirit as libhubbub above), covered in detail
# in PORTING_NOTES.md:
#   - src/select/autogenerated_{computed,propget,propset}.h, from
#     `python3 src/select/select_generator.py` (pure stdlib, run from
#     the libcss root)
#   - src/parse/properties/autogenerated_<property>.c (119 files),
#     one per CSS property, from a small host-native generator tool
#     (src/parse/properties/css_property_parser_gen.c, plain C, no
#     special deps) built with the *host* compiler and run once per
#     property against that property's spec line(s) extracted from
#     src/parse/properties/properties.gen.
# css_property_parser_gen.c itself is the host-side generator tool,
# not kernel code - excluded from the source glob below, same idea as
# excluding the two #include-only generated files elsewhere.
# libcss/src has sources directly under src/, plus in charset/, lex/,
# parse/, parse/properties/, select/, select/properties/, and utils/
# (but not recursively beyond that), hence the two-level glob.
LCSS_DIR = $(NSALL_DIR)/libcss
LCSS_SRCS := $(filter-out $(LCSS_DIR)/src/parse/properties/css_property_parser_gen.c,$(wildcard $(LCSS_DIR)/src/*.c $(LCSS_DIR)/src/*/*.c $(LCSS_DIR)/src/*/*/*.c))
LCSS_OBJS := $(patsubst $(LCSS_DIR)/src/%.c,$(OBJ_DIR)/libcss/%.o,$(LCSS_SRCS))
LCSS_LIB = $(BUILD_DIR)/liblcss.a
LCSS_INCLUDES = -I$(LCSS_DIR)/include -I$(LCSS_DIR)/src -I$(LWAPCAPLET_DIR)/include -I$(LPARSERUTILS_DIR)/include \
	-D_ALIGNED=
# -D_ALIGNED= (defines it as empty, NOT as 1 - that's what the
# trailing "=" with nothing after it means to GCC) works around a
# genuine pre-existing gap in this libcss vendoring: src/stylesheet.h
# uses "} _ALIGNED;" as a trailing GCC attribute on `struct css_rule`
# (the usual "struct S { ... } ATTR;" position), expecting some
# upstream build config header to have already #defined _ALIGNED as
# an alignment attribute - nothing in this tree ever does. Without
# this, "_ALIGNED" isn't a macro at all, so the compiler reads
# "} _ALIGNED;" as declaring an actual global variable named
# _ALIGNED of that struct type - once compiled into more than one of
# libcss's ~300 translation units (which all include this header),
# every one of them defines its own global _ALIGNED, and the linker
# rejects it as a multiple definition the first time enough of
# liblcss.a's objects actually get linked together into one binary.
# That first time was 2026-08-08, once html_init()/nscss_init() (see
# cos_netsurf.c) started actually being called - see
# PORTING_NOTES.md. _ALIGNED is used in exactly this one place in the
# whole libcss tree (grepped to confirm), and struct css_rule's
# members are already naturally pointer-aligned without any explicit
# attribute, so defining it as empty rather than guessing at a
# specific __attribute__((aligned(N))) value is the safer fix - it
# removes the erroneous variable declaration without asserting an
# alignment requirement upstream never actually documented anywhere
# in this vendored copy.

# libdom (the actual DOM implementation): depends on libhubbub +
# libcss + libwapcaplet + libparserutils, all ready above. Turned out
# to need no code generation at all (unlike hubbub/css) - src/core,
# src/events, src/html, src/utils, and bindings/hubbub/parser.c (the
# glue that builds a DOM tree from libhubbub's parser output) are all
# plain hand-written C. bindings/xml/ is deliberately excluded
# entirely: it offers a choice of expat or libxml2 for parsing XML
# documents (as opposed to HTML, which goes through the hubbub
# binding instead), and neither expat nor libxml2 is part of this
# build - not needed for ordinary HTML browsing, and skipping it
# avoids taking on a whole new external dependency. If XML document
# support (as opposed to HTML) ever matters, that's the place to
# start, and expat would need porting first.
LDOM_DIR = $(NSALL_DIR)/libdom
LDOM_SRCS := $(wildcard $(LDOM_DIR)/src/*.c $(LDOM_DIR)/src/*/*.c) $(LDOM_DIR)/bindings/hubbub/parser.c
LDOM_OBJS := $(patsubst $(LDOM_DIR)/%.c,$(OBJ_DIR)/libdom/%.o,$(LDOM_SRCS))
LDOM_LIB = $(BUILD_DIR)/libldom.a
LDOM_INCLUDES = -I$(LDOM_DIR)/include -I$(LDOM_DIR)/src -I$(LDOM_DIR)/bindings/hubbub -I$(LHUBBUB_DIR)/include -I$(LWAPCAPLET_DIR)/include -I$(LPARSERUTILS_DIR)/include

# libnsutils: just base64.c (RFC4648, fully self-contained). Its
# siblings time.c/unistd.c are themselves thin wrappers *around* real
# <unistd.h>/<time.h> (not portable replacements for them, despite
# the library's name), so they're skipped here rather than pulled in
# for functionality nothing needs yet.
LNSUTILS_DIR = $(NSALL_DIR)/libnsutils
LNSUTILS_SRCS = $(LNSUTILS_DIR)/src/base64.c $(LNSUTILS_DIR)/src/time_cos.c
LNSUTILS_OBJS := $(patsubst $(LNSUTILS_DIR)/src/%.c,$(OBJ_DIR)/libnsutils/%.o,$(LNSUTILS_SRCS))
LNSUTILS_LIB = $(BUILD_DIR)/liblnsutils.a
LNSUTILS_INCLUDES = -I$(LNSUTILS_DIR)/include

# NetSurf's own utils/ subsystem (not a third-party dependency - this
# is the first piece of NetSurf's actual source, as opposed to a
# library it depends on). Deliberately an explicit file list rather
# than a wildcard: several files in this directory are excluded on
# purpose because they need things this build doesn't have -
# documented in full in PORTING_NOTES.md, summarized here:
#   - file.c, filename.c, filepath.c, utils.c: real POSIX filesystem
#     calls (stat/opendir/fopen) for resolving local resource/cache
#     paths - fundamentally tied to a hosted filesystem layout this
#     kernel doesn't have. A C-OS frontend will need its own take on
#     "where do resources live" against fs_api instead.
#   - time.c: needs curl (curl_getdate, for parsing HTTP date
#     headers) - a small hand-written date parser could replace this
#     cheaply later without needing curl at all.
#   - utf8.c: needs iconv pervasively (legacy non-UTF-8 charset
#     conversion) - not required for modern (UTF-8) content; the
#     already-ported libparserutils/libutf8proc cover the common case.
#   - hashtable.c: needs zlib for one optional feature (loading a
#     gzip-compressed hash table from disk) that's separable but not
#     yet trimmed out.
#   - log.c: wants real hosted FILE*/fprintf/stderr for NSLOG - this
#     kernel logs via serial_puts instead (see the quickjs.c backend's
#     comments for the same call made there).
#   - nsoption.c: entangled with desktop/options.h and per-frontend
#     option tables (riscos/gtk/...) that don't exist in this tree.
# messages.c *is* included despite loading translations from a file
# upstream - the parts compiled here don't pull in real file I/O
# (nothing accesses a filesystem path yet), and content/handlers/html
# will want its localised-string lookups.
NS_UTILS_FILES = bloom.c corestrings.c hashtable.c idna.c libdom.c nscolour.c \
	punycode.c ssl_certs.c talloc.c url.c useragent.c messages.c \
	http/parameter.c http/challenge.c http/strict-transport-security.c \
	http/primitives.c http/cache-control.c http/generics.c \
	http/www-authenticate.c http/content-type.c http/content-disposition.c \
	nsurl/nsurl.c nsurl/parse.c
NS_UTILS_SRCS := $(addprefix $(NS_DIR)/utils/,$(NS_UTILS_FILES))
NS_UTILS_OBJS := $(patsubst $(NS_DIR)/utils/%.c,$(OBJ_DIR)/ns_utils/%.o,$(NS_UTILS_SRCS))
NS_UTILS_LIB = $(BUILD_DIR)/libns_utils.a
NS_UTILS_INCLUDES = -I$(NS_DIR) -I$(NS_DIR)/include -I$(LDOM_DIR)/include -I$(LHUBBUB_DIR)/include -I$(LCSS_DIR)/include -I$(LWAPCAPLET_DIR)/include -I$(LPARSERUTILS_DIR)/include -I$(LUTF8PROC_DIR)/include/libutf8proc -I$(LNSUTILS_DIR)/include

# NetSurf's content/ core, plus the html and css content handlers -
# this is "Tier A" of content/: the pieces that parse a page into the
# real DOM (libdom+libhubbub), associate styles (libcss), and are
# what will call js_exec() on <script> tags - i.e. what actually
# makes the QuickJS backend reachable from a real page. Deliberately
# NOT included here ("Tier B", future work): box_construct/
# box_normalise/box_special/box_textarea/box_inspect/box_manipulate/
# layout/layout_flex/table/redraw/redraw_border/font/imagemap/
# interaction/textselection/form/forms/object - all of that is the
# visual box-model layout + rendering pipeline, which fundamentally
# needs a working plotter/frontend (PORTING_NOTES.md item 2) to make
# sense of at all, and would separately need triage of its own
# dependencies the way this tier did. content/fetchers/curl.c and
# content/handlers/image/* are excluded outright: the former needs
# curl (a C-OS-native fetcher belongs in the frontend instead, same
# spirit as excluding utils/time.c's curl_getdate use), the latter
# needs external image codec libraries (libjpeg/libpng/giflib/...)
# not part of this build.
#
# Two source-level changes were needed (not just exclusions), both
# guarded with #ifndef COS_KERNEL rather than deleted, so upstream's
# code and this build's reasoning both stay visible in the tree:
#   - utils/inet.h: skips real BSD socket headers entirely for
#     COS_KERNEL (this kernel has its own networking stack, not BSD
#     sockets - kernel/api/net_api.c) and defines a minimal stand-in
#     `fd_set` so the couple of header-only signatures that still
#     mention fd_set* (content/fetch.h's fetch_fdset(), the unused
#     `fdset` fetcher-table entry) can still be *parsed* without
#     pulling in the host sandbox's real <sys/socket.h>.
#   - content/handlers/html/html.c: html_drop_file_at_point() (real
#     fopen/fread for reading a dropped file) is excluded - it's
#     drag-and-drop, which needs a real GUI frontend to ever be
#     reached anyway, and should read via fs_api instead of fopen()
#     once it is.
NS_CONTENT_FILES = content.c content_factory.c hlcache.c llcache.c mimesniff.c no_backing_store.c
NS_CONTENT_SRCS := $(addprefix $(NS_DIR)/content/,$(NS_CONTENT_FILES))
NS_FETCHER_FILES = data.c resource.c
NS_FETCHER_SRCS := $(addprefix $(NS_DIR)/content/fetchers/,$(NS_FETCHER_FILES))
NS_HTML_FILES = html.c script.c dom_event.c css.c css_fetcher.c box_construct.c box_inspect.c box_manipulate.c box_normalise.c box_special.c forms.c form.c box_textarea.c font.c layout.c layout_flex.c table.c redraw.c redraw_border.c object.c interaction.c imagemap.c
NS_HTML_SRCS := $(addprefix $(NS_DIR)/content/handlers/html/,$(NS_HTML_FILES))
NS_CSSHANDLER_FILES = css.c dump.c hints.c internal.c select.c
NS_CSSHANDLER_SRCS := $(addprefix $(NS_DIR)/content/handlers/css/,$(NS_CSSHANDLER_FILES))
# Register the JavaScript MIME content handler so HTML <script> elements
# map text/javascript and application/javascript to CONTENT_JS, which in
# turn dispatches to the separately linked QuickJS backend.
NS_JAVASCRIPT_HANDLER_SRCS = $(NS_DIR)/content/handlers/javascript/content.c
# libnsbmp and libsvgtiny are fully vendored and require no hosted image
# codec library. Keep their standard NetSurf content handlers in the same
# lifecycle as PNG/JPEG/BMP so SVG/ICO resources are parsed, sized, cached and
# redrawn rather than being page-specific transparent fallbacks.
NS_IMAGE_HANDLER_SRCS = $(NS_DIR)/content/handlers/image/image.c \
	$(NS_DIR)/content/handlers/image/bmp.c \
	$(NS_DIR)/content/handlers/image/svg.c \
	$(NS_DIR)/content/handlers/image/ico.c
NS_CONTENT_ALL_SRCS := $(NS_CONTENT_SRCS) $(NS_FETCHER_SRCS) $(NS_HTML_SRCS) $(NS_CSSHANDLER_SRCS) $(NS_JAVASCRIPT_HANDLER_SRCS) $(NS_IMAGE_HANDLER_SRCS)
# bitmap.c supplies bitmap_format_convert(), used by the upstream BMP
# handler when libnsbmp pixels are normalised for the C-OS plotter.
NS_DESKTOP_FILES = scrollbar.c textarea.c textinput.c plot_style.c bitmap.c browser_window.c browser_history.c frames.c knockout.c download.c
NS_DESKTOP_SRCS := $(addprefix $(NS_DIR)/desktop/,$(NS_DESKTOP_FILES))
NS_CONTENT_OBJS := $(patsubst $(NS_DIR)/content/%.c,$(OBJ_DIR)/ns_content/%.o,$(NS_CONTENT_ALL_SRCS))
NS_HTML_OBJS := $(patsubst $(NS_DIR)/content/handlers/html/%.c,$(OBJ_DIR)/ns_content/handlers/html/%.o,$(NS_HTML_SRCS))
# Transitional ABI guard: private.h owns struct html_content.  Until every
# existing object has emitted its first .d file, make this critical layout
# dependency explicit as well.  It limits rebuilds to the real HTML cluster.
$(NS_HTML_OBJS): $(NS_DIR)/content/handlers/html/private.h
NS_DESKTOP_OBJS := $(patsubst $(NS_DIR)/desktop/%.c,$(OBJ_DIR)/ns_desktop/%.o,$(NS_DESKTOP_SRCS))
NS_CONTENT_LIB = $(BUILD_DIR)/libns_content.a
NS_CONTENT_INCLUDES = -I$(NS_DIR) -I$(NS_DIR)/include -I$(NS_DIR)/content -I$(NS_DIR)/content/handlers -I$(LDOM_DIR)/include -I$(LHUBBUB_DIR)/include -I$(LCSS_DIR)/include -I$(LWAPCAPLET_DIR)/include -I$(LPARSERUTILS_DIR)/include -I$(LUTF8PROC_DIR)/include/libutf8proc -I$(LNSUTILS_DIR)/include -I$(NSALL_DIR)/libnsbmp/include -I$(SVGTINY_DIR)/include

# NetSurf's BMP parser/decoder is vendored C code.  Keep it as a dedicated
# archive, matching the other NetSurf support libraries, so the final grouped
# link resolves bitmap/content cross-references deterministically.
NSBMP_DIR = $(NSALL_DIR)/libnsbmp
NSBMP_SRC = $(NSBMP_DIR)/src/libnsbmp.c
NSBMP_OBJ = $(OBJ_DIR)/nsbmp/libnsbmp.o
NSBMP_LIB = $(BUILD_DIR)/libnsbmp.a

# The upstream SVG content handler uses libsvgtiny, which itself consumes the
# already linked libdom XML parser. Build it as a dedicated archive so image
# handler symbols resolve inside the final grouped kernel link just like BMP.
SVGTINY_DIR = $(NSALL_DIR)/libsvgtiny
SVGTINY_SRCS := $(wildcard $(SVGTINY_DIR)/src/*.c)
SVGTINY_OBJS := $(patsubst $(SVGTINY_DIR)/src/%.c,$(OBJ_DIR)/svgtiny/%.o,$(SVGTINY_SRCS))
SVGTINY_LIB = $(BUILD_DIR)/libsvgtiny.a

# The native (not vendored/ported - hand-written for C-OS) driver
# code living on top of everything above: the fetcher registry/
# dispatch (cos_fetch.c) and engine init + synchronous page-load
# driver (cos_netsurf.c). See both files' header comments. Needs the
# same include set as the content library above, plus its own
# directory (for cos_netsurf.h) and third_party/quickjs (for
# javascript/js.h's jsheap/jsthread types via quickjs_port.h's
# JSRuntime/JSContext).
COS_NETSURF_DIR = $(SRCROOT)/netsurf
# C-OS 4.0.8 second alpha: cos_fetch_http.c (real http:/https: fetcher)
# and cos_netsurf_render.c (real-DOM-to-display-lines) added - see
# those files' headers. This list is explicit, not a wildcard, so any
# new file under src/netsurf/ must be added here by hand or it will
# silently never be compiled.
COS_NETSURF_SRCS = $(COS_NETSURF_DIR)/cos_fetch.c $(COS_NETSURF_DIR)/cos_fetch_file.c $(COS_NETSURF_DIR)/cos_fetch_http.c $(COS_NETSURF_DIR)/cos_netsurf.c $(COS_NETSURF_DIR)/cos_netsurf_render.c $(COS_NETSURF_DIR)/cos_gui_table.c $(COS_NETSURF_DIR)/cos_netsurf_adapter.c $(COS_NETSURF_DIR)/cos_netsurf_browser.c $(COS_NETSURF_DIR)/cos_netsurf_png.c $(COS_NETSURF_DIR)/cos_netsurf_jpeg.c $(COS_NETSURF_DIR)/cos_urldb_stub.c $(COS_NETSURF_DIR)/cos_nsoptions.c $(COS_NETSURF_DIR)/cos_netsurf_tierb_stubs.c $(COS_NETSURF_DIR)/cos_parserutils_filter_stub.c
COS_NETSURF_OBJS := $(patsubst $(COS_NETSURF_DIR)/%.c,$(OBJ_DIR)/netsurf/%.o,$(COS_NETSURF_SRCS))
# NOTE: COS_NETSURF_OBJS above is NOT what the final kernel.elf link
# uses - it's currently unreferenced elsewhere in this file. The real
# link-time object list is the hardcoded OBJS variable near the top of
# this Makefile (search for "cos_fetch.o"). Confirmed the hard way:
# adding a new src/netsurf/*.c file to COS_NETSURF_SRCS alone gets it
# compiled but NOT linked - it must also be added to OBJS by hand.
COS_NETSURF_INCLUDES = $(NS_CONTENT_INCLUDES) -I$(COS_NETSURF_DIR) -I$(QUICKJS_DIR) -I$(NS_DIR)/frontends \
	-I$(LPARSERUTILS_DIR)/src \
	-DPLOT_FONT_FAMILY_SANS_SERIF=1 \
	-DNETSURF_BUILTIN_LOG_FILTER=\"warning\" \
	-DNETSURF_BUILTIN_VERBOSE_FILTER=\"deepdebug\"

# ---- QuickJS (embeddable JS engine for the browser's script support) ----
# Only the embeddable core (quickjs.c/libregexp.c/libunicode.c/dtoa.c +
# quickjs_port.c, the C-OS glue in third_party/quickjs/quickjs_port.c) is
# built here - qjs.c/qjsc.c (the CLI/bytecode-compiler tools),
# quickjs-libc.c (the CLI's os/std module implementations, which assume a
# real POSIX filesystem), and all test/fuzz drivers were intentionally
# left out of third_party/quickjs/ when this was ported; see quickjs_port.c
# for what's actually wired up so far.
QUICKJS_DIR = $(SRCROOT)/third_party/quickjs
QUICKJS_SRCS := $(wildcard $(QUICKJS_DIR)/*.c)
QUICKJS_OBJS := $(patsubst $(QUICKJS_DIR)/%.c,$(OBJ_DIR)/quickjs/%.o,$(QUICKJS_SRCS))
QUICKJS_LIB = $(BUILD_DIR)/libquickjs.a
QUICKJS_CFLAGS = -DCONFIG_VERSION=\"cos-4.0.8-alpha\" -DCONFIG_BIGNUM=0 \
	-D__STDC_NO_ATOMICS__=1 \
	-DCONFIG_WORKER=0 \
	-include alloca.h -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
	-Wno-implicit-function-declaration -Wno-error=implicit-function-declaration \
	-I$(NSALL_DIR)/libdom/include -I$(NSALL_DIR)/libwapcaplet/include

# ---- NetSurf's own javascript-engine backend, implemented against
# content/handlers/javascript/js.h using QuickJS (see that file's
# header comment for exactly what is and isn't wired up yet). This is
# real NetSurf-tree code, built the same one-component-at-a-time way
# as the libraries above - it is NOT part of QUICKJS_SRCS itself,
# since it lives under netsurf/content/handlers/javascript/quickjs/,
# not third_party/quickjs/. ----
NSJS_QUICKJS_DIR = $(NSALL_DIR)/netsurf/content/handlers/javascript/quickjs
NSJS_QUICKJS_SRCS := $(wildcard $(NSJS_QUICKJS_DIR)/*.c)
NSJS_QUICKJS_OBJS := $(patsubst $(NSJS_QUICKJS_DIR)/%.c,$(OBJ_DIR)/ns_js_quickjs/%.o,$(NSJS_QUICKJS_SRCS))
NSJS_QUICKJS_LIB = $(BUILD_DIR)/libns_js_quickjs.a
NSJS_QUICKJS_INCLUDES = -I$(QUICKJS_DIR) -I$(NSALL_DIR)/netsurf -I$(NSALL_DIR)/netsurf/include -I$(NSALL_DIR)/netsurf/content/handlers -I$(LDOM_DIR)/include -I$(LWAPCAPLET_DIR)/include -I$(NSALL_DIR)/libhubbub/include

# ---- ACPICA component object files ----
ACPICA_SRC_DIR = $(SRCROOT)/third_party/acpica/source/components
ACPICA_SRCS := $(shell find $(ACPICA_SRC_DIR) -name '*.c' | sort)
ACPICA_OBJS := $(patsubst $(ACPICA_SRC_DIR)/%.c,$(OBJ_DIR)/acpica/%.o,$(ACPICA_SRCS))
ACPICA_LIB = $(BUILD_DIR)/libacpica.a

# Optional TLS backend selection.
# Default = BearSSL (kernel-friendly). Set ENABLE_OPENSSL_BACKEND=1 for hosted builds.
ENABLE_OPENSSL_BACKEND ?= 0
TLS_BACKEND_OBJ = $(OBJ_DIR)/kernel/drivers/tls_backend.o
TLS_BACKEND_EXTRA_LIBS =
ifeq ($(ENABLE_OPENSSL_BACKEND),1)
TLS_BACKEND_OBJ = $(OBJ_DIR)/kernel/drivers/tls_backend_openssl.o
TLS_BACKEND_EXTRA_LIBS = -lssl -lcrypto
endif


.PHONY: all kernel iso clean dirs cosnet net-sync text-browser browser

all: iso $(STORAGE_IMG)

kernel: $(KERNEL)

# ---- Create output directories ----
$(OBJ_DIR) $(BUILD_DIR):
	mkdir -p $(OBJ_DIR) $(BUILD_DIR)
	mkdir -p $(OBJ_DIR)/drivers/input $(OBJ_DIR)/drivers/video $(OBJ_DIR)/drivers/disk $(OBJ_DIR)/kernel/drivers
	mkdir -p $(OBJ_DIR)/fs $(OBJ_DIR)/gui $(OBJ_DIR)/apps $(OBJ_DIR)/lib $(OBJ_DIR)/compat $(OBJ_DIR)/cpp $(OBJ_DIR)/kernel/api
	mkdir -p $(OBJ_DIR)/third_party/tinygl
	mkdir -p $(OBJ_DIR)/acpica $(OBJ_DIR)/kernel/power
	mkdir -p $(OBJ_DIR)/quickjs
	mkdir -p $(OBJ_DIR)/libwapcaplet
	mkdir -p $(OBJ_DIR)/libparserutils

# ---- Assembly files (NASM 64-bit ELF) ----

$(OBJ_DIR)/kernel/context_switch.o: $(SRCROOT)/kernel/context_switch.asm | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/boot/boot.o: $(SRCROOT)/boot/boot.asm | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/boot/irq.o: $(SRCROOT)/boot/irq.asm | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/boot/isr.o: $(SRCROOT)/boot/isr.asm | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/boot/storage_blob.o: $(SRCROOT)/boot/storage_blob.asm | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/boot/smp_trampoline.o: $(SRCROOT)/boot/smp_trampoline.asm | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# ---- C kernel files ----
$(OBJ_DIR)/third_party/tinygl/%.o: $(TINYGL_SRC_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -DNDEBUG -DM_PI=3.14159265358979323846 -c $< -o $@

$(OBJ_DIR)/kernel/kernel.o: $(SRCROOT)/kernel/kernel.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/io.o: $(SRCROOT)/kernel/io.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/serial.o: $(SRCROOT)/kernel/serial.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/timer.o: $(SRCROOT)/kernel/timer.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/scheduler.o: $(SRCROOT)/kernel/scheduler.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/smp.o: $(SRCROOT)/kernel/smp.c $(SRCROOT)/include/smp.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/task.o: $(SRCROOT)/kernel/task.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/sync.o: $(SRCROOT)/kernel/sync.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/ipc.o: $(SRCROOT)/kernel/ipc.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/mm/paging.o: $(SRCROOT)/kernel/mm/paging.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/rtc.o: $(SRCROOT)/kernel/rtc.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/page_fault.o: $(SRCROOT)/kernel/page_fault.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/idt.o: $(SRCROOT)/kernel/idt.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/syscall.o: $(SRCROOT)/kernel/syscall.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/userspace_demo.o: $(SRCROOT)/kernel/userspace_demo.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/arch/gdt.o: $(SRCROOT)/kernel/arch/gdt.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/memory.o: $(SRCROOT)/kernel/memory.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/memory_virtual.o: $(SRCROOT)/kernel/memory_virtual.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


$(OBJ_DIR)/kernel/memory_physical.o: $(SRCROOT)/kernel/memory_physical.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


$(OBJ_DIR)/kernel/irq.o: $(SRCROOT)/kernel/irq.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/irq_handlers.o: $(SRCROOT)/kernel/irq_handlers.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/shell.o: $(SRCROOT)/kernel/shell.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/system/mk_mp3_backend.o: $(SRCROOT)/kernel/system/mk_mp3_backend.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/minimp3/minimp3.o: $(SRCROOT)/third_party/minimp3/minimp3.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/bios/bios.o: $(SRCROOT)/bios/bios.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/settings_manager.o: $(SRCROOT)/kernel/settings_manager.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/disk/ide.o: $(SRCROOT)/drivers/disk/ide.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/ai/mphalport.o: $(SRCROOT)/kernel/ai/mphalport.c | $(OBJ_DIR) $(MICROPY_GENHDR_DIR)/qstrdefs.generated.h $(MICROPY_GENHDR_DIR)/mpversion.h $(MICROPY_GENHDR_DIR)/moduledefs.h $(MICROPY_GENHDR_DIR)/root_pointers.h

	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/ai/micropython_repl.o: $(SRCROOT)/kernel/ai/micropython_repl.c | $(OBJ_DIR) $(MICROPY_GENHDR_DIR)/qstrdefs.generated.h $(MICROPY_GENHDR_DIR)/mpversion.h $(MICROPY_GENHDR_DIR)/moduledefs.h $(MICROPY_GENHDR_DIR)/root_pointers.h

	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/ai/micropython_vfs.o: $(SRCROOT)/kernel/ai/micropython_vfs.c | $(OBJ_DIR) $(MICROPY_GENHDR_DIR)/qstrdefs.generated.h $(MICROPY_GENHDR_DIR)/mpversion.h $(MICROPY_GENHDR_DIR)/moduledefs.h $(MICROPY_GENHDR_DIR)/root_pointers.h

	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/ai/micropython_bridge.o: $(SRCROOT)/kernel/ai/micropython_bridge.c | $(OBJ_DIR) $(MICROPY_GENHDR_DIR)/qstrdefs.generated.h $(MICROPY_GENHDR_DIR)/mpversion.h $(MICROPY_GENHDR_DIR)/moduledefs.h $(MICROPY_GENHDR_DIR)/root_pointers.h

	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


$(OBJ_DIR)/kernel/ai/micropython_port.o: $(SRCROOT)/kernel/ai/micropython_port.c | $(OBJ_DIR) $(MICROPY_GENHDR_DIR)/qstrdefs.generated.h $(MICROPY_GENHDR_DIR)/mpversion.h $(MICROPY_GENHDR_DIR)/moduledefs.h $(MICROPY_GENHDR_DIR)/root_pointers.h

	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@



# Generic rule for kernel AI sources
$(OBJ_DIR)/kernel/ai/%.o: $(SRCROOT)/kernel/ai/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
# MicroPython Core
#
# NOTE: genhdr generation (mpversion.h, qstrdefs.generated.h, moduledefs.h,
# root_pointers.h, moduledefs.collected, the build/genhdr directory rule,
# and the `clean` target) is provided by the upstream
# $(MP_SRC_PY)/py.mk / mkrules.mk included later in this file. This file
# used to also define its own recipes for the same targets, but GNU Make
# only keeps the last-parsed recipe for a given target, so those duplicate
# rules were dead code (never executed) and only produced "overriding
# recipe" warnings. They have been removed; py.mk/mkrules.mk's versions
# are authoritative.

FORCE:
.PHONY: FORCE

MICROPY_SRCS = $(shell find $(MP_SRC_DIR)/py -name "*.c")
MICROPY_OBJS = $(patsubst $(MP_SRC_DIR)/%.c,$(OBJ_DIR)/micropython/%.o,$(MICROPY_SRCS))
$(MICROPY_OBJS): | $(MICROPY_GENHDR_DIR)/qstrdefs.generated.h $(MICROPY_GENHDR_DIR)/mpversion.h $(MICROPY_GENHDR_DIR)/moduledefs.h $(MICROPY_GENHDR_DIR)/root_pointers.h


$(OBJ_DIR)/micropython/%.o: $(MP_SRC_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/api/cos_api.o: $(SRCROOT)/kernel/api/cos_api.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/api/fs_api.o: $(SRCROOT)/kernel/api/fs_api.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/api/net_api.o: $(SRCROOT)/kernel/api/net_api.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Removed legacy duplicate kernel files; canonical implementations live in keyboard.c, timer.c, rtc.c, serial.c, page_fault.c, memory_physical.c, and memory_virtual.c

$(OBJ_DIR)/drivers/video/vga.o: $(SRCROOT)/drivers/video/vga.c | $(OBJ_DIR)
		mkdir -p $(dir $@)
		$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/video/gfx_blit.o: $(SRCROOT)/drivers/video/gfx_blit.c | $(OBJ_DIR)
		mkdir -p $(dir $@)
		$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/video/gfx_blit_avx2.o: $(SRCROOT)/drivers/video/gfx_blit_avx2.c | $(OBJ_DIR)
		mkdir -p $(dir $@)
		$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/video/font24x24_data.o: $(SRCROOT)/drivers/video/font24x24_data.c | $(OBJ_DIR)
		mkdir -p $(dir $@)
		$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/video/jp_font16.o: $(SRCROOT)/drivers/video/jp_font16.c | $(OBJ_DIR)
		mkdir -p $(dir $@)
		$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/output/gui_render_engine.o: $(SRCROOT)/gui/core/output/gui_render_engine.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/gui_utils.o: $(SRCROOT)/gui/core/gui_utils.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/calc_engine.o: $(SRCROOT)/kernel/calc_engine.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/input/keyboard.o: $(SRCROOT)/drivers/input/keyboard.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/gui_lifecycle.o: $(SRCROOT)/gui/core/gui_lifecycle.c $(SRCROOT)/gui/core/gui_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/window/gui_settings.o: $(SRCROOT)/gui/core/window/gui_settings.c $(SRCROOT)/gui/core/gui_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/output/gui_desktop_render.o: $(SRCROOT)/gui/core/output/gui_desktop_render.c $(SRCROOT)/gui/core/gui_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/output/gui_taskbar_frame.o: $(SRCROOT)/gui/core/output/gui_taskbar_frame.c $(SRCROOT)/gui/core/gui_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/window/gui_menus.o: $(SRCROOT)/gui/core/window/gui_menus.c $(SRCROOT)/gui/core/gui_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/input/gui_input.o: $(SRCROOT)/gui/core/input/gui_input.c $(SRCROOT)/gui/core/gui_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/gui_render_loop.o: $(SRCROOT)/gui/core/gui_render_loop.c $(SRCROOT)/gui/core/gui_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


$(OBJ_DIR)/gui/components/boot_animation.o: $(SRCROOT)/gui/components/boot_animation.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/ui/modern_ui.o: $(SRCROOT)/gui/apps/ui/modern_ui.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/common/gui_apps_common.o: $(SRCROOT)/gui/apps/common/gui_apps_common.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/core/gui_apps_core.o: $(SRCROOT)/gui/apps/core/gui_apps_core.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/terminal/gui_apps_terminal.o: $(SRCROOT)/gui/apps/terminal/gui_apps_terminal.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/calculator/gui_apps_calculator.o: $(SRCROOT)/gui/apps/calculator/gui_apps_calculator.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/settings/gui_apps_settings.o: $(SRCROOT)/gui/apps/settings/gui_apps_settings.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/about/gui_apps_about.o: $(SRCROOT)/gui/apps/about/gui_apps_about.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/browser/gui_apps_browser.o: $(SRCROOT)/gui/apps/browser/gui_apps_browser.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/taskmanager/gui_apps_taskmanager.o: $(SRCROOT)/gui/apps/taskmanager/gui_apps_taskmanager.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/paint/gui_apps_paint.o: $(SRCROOT)/gui/apps/paint/gui_apps_paint.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/clock/gui_apps_clock.o: $(SRCROOT)/gui/apps/clock/gui_apps_clock.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/storage/gui_apps_storage.o: $(SRCROOT)/gui/apps/storage/gui_apps_storage.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/memory/gui_apps_memory.o: $(SRCROOT)/gui/apps/memory/gui_apps_memory.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/birthday/gui_apps_birthday.o: $(SRCROOT)/gui/apps/birthday/gui_apps_birthday.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/music/gui_apps_music.o: $(SRCROOT)/gui/apps/music/gui_apps_music.c $(SRCROOT)/gui/apps/common/gui_apps_common.h $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


$(OBJ_DIR)/gui/apps/games/voxel_games_advanced.o: $(SRCROOT)/gui/apps/games/voxel_games_advanced.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/games/tinygl_viewer.o: $(SRCROOT)/gui/apps/games/tinygl_viewer.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/games/games2d.o: $(SRCROOT)/gui/apps/games/games2d.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/tools/calc_school_math.o: $(SRCROOT)/gui/apps/tools/calc_school_math.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/window/gui_windows.o: $(SRCROOT)/gui/core/window/gui_windows.c $(COSNET_STATE) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/window/gui_create_window.o: $(SRCROOT)/gui/core/window/gui_create_window.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/development/python_ide_gui.o: $(SRCROOT)/gui/apps/development/python_ide_gui.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/tools/http_downloader_gui.o: $(SRCROOT)/gui/apps/tools/http_downloader_gui.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/system/password_screen.o: $(SRCROOT)/gui/apps/system/password_screen.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/browser/modern_browser.o: $(SRCROOT)/gui/browser/modern_browser.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/browser/cos_browser.o: $(SRCROOT)/gui/browser/cos_browser.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Additional GUI build rules for new folder structure
$(OBJ_DIR)/gui/components/enhanced_context_menu.o: $(SRCROOT)/gui/components/enhanced_context_menu.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/efm_util.o: $(SRCROOT)/gui/file_manager/efm_util.c $(SRCROOT)/gui/file_manager/efm_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/efm_core.o: $(SRCROOT)/gui/file_manager/efm_core.c $(SRCROOT)/gui/file_manager/efm_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/efm_fileops.o: $(SRCROOT)/gui/file_manager/efm_fileops.c $(SRCROOT)/gui/file_manager/efm_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/efm_search_bookmark.o: $(SRCROOT)/gui/file_manager/efm_search_bookmark.c $(SRCROOT)/gui/file_manager/efm_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/efm_thumbnail_preview.o: $(SRCROOT)/gui/file_manager/efm_thumbnail_preview.c $(SRCROOT)/gui/file_manager/efm_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/efm_render.o: $(SRCROOT)/gui/file_manager/efm_render.c $(SRCROOT)/gui/file_manager/efm_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/efm_input.o: $(SRCROOT)/gui/file_manager/efm_input.c $(SRCROOT)/gui/file_manager/efm_internal.h | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


$(OBJ_DIR)/gui/core/input/input_manager.o: $(SRCROOT)/gui/core/input/input_manager.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/output/gui_renderer.o: $(SRCROOT)/gui/core/output/gui_renderer.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/file_manager/file_manager_explorer.o: $(SRCROOT)/gui/file_manager/file_manager_explorer.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/core/gui_file_manager_enhanced_bridge.o: $(SRCROOT)/gui/core/gui_file_manager_enhanced_bridge.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/apps/system/password_screen_enhanced.o: $(SRCROOT)/gui/apps/system/password_screen_enhanced.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Generic rule for additional GUI sources
$(OBJ_DIR)/gui/%.o: $(SRCROOT)/gui/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/apps/config_manager.o: $(SRCROOT)/apps/config_manager.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/apps/jpeg_viewer.o: $(SRCROOT)/apps/jpeg_viewer.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/apps/png_decoder.o: $(SRCROOT)/apps/png_decoder.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/assets/desktop_featured_png.o: $(SRCROOT)/assets/desktop_featured_png.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/assets/sample_icon_bmp.o: $(SRCROOT)/assets/sample_icon_bmp.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/assets/sample_beep_mp3.o: $(SRCROOT)/assets/sample_beep_mp3.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/assets/sample_photo_jpg.o: $(SRCROOT)/assets/sample_photo_jpg.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/assets/sample_beep_wav.o: $(SRCROOT)/assets/sample_beep_wav.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/input/mouse.o: $(SRCROOT)/drivers/input/mouse.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/fs/fs.o: $(SRCROOT)/fs/fs.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/fatfs/ff.o: $(SRCROOT)/third_party/fatfs/ff.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/third_party/fatfs/ffunicode.o: $(SRCROOT)/third_party/fatfs/ffunicode.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/third_party/fatfs/diskio.o: $(SRCROOT)/third_party/fatfs/diskio.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/fatfs/ffsystem.o: $(SRCROOT)/third_party/fatfs/ffsystem.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/fs/fs_unified.o: $(SRCROOT)/fs/fs_unified.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/fs/text_editor.o: $(SRCROOT)/fs/text_editor.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/disk/storage.o: $(SRCROOT)/drivers/disk/storage.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/drivers/disk/crc32_common.o: $(SRCROOT)/drivers/disk/crc32_common.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/filemanager/mk_advanced_filemanager.o: $(SRCROOT)/kernel/filemanager/mk_advanced_filemanager.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/string.o: $(SRCROOT)/lib/string.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/cos_itoa.o: $(SRCROOT)/lib/cos_itoa.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/ctype.o: $(SRCROOT)/lib/ctype.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/cos_assert.o: $(SRCROOT)/lib/cos_assert.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/cos_pthread_stub.o: $(SRCROOT)/lib/cos_pthread_stub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/math.o: $(SRCROOT)/lib/math.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -DCOS_SKIP_TINYGL_DUPS -c $< -o $@

$(OBJ_DIR)/lib/time_impl.o: $(SRCROOT)/lib/time_impl.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/cos_nslog.o: $(SRCROOT)/lib/cos_nslog.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lib/cos_http_date.o: $(SRCROOT)/lib/cos_http_date.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(NS_DIR) -c $< -o $@

$(OBJ_DIR)/lib/cos_string_util.o: $(SRCROOT)/lib/cos_string_util.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/arp.o: $(SRCROOT)/kernel/drivers/arp.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/dhcp.o: $(SRCROOT)/kernel/drivers/dhcp.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/dns.o: $(SRCROOT)/kernel/drivers/dns.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/udp.o: $(SRCROOT)/kernel/drivers/udp.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/tcp.o: $(SRCROOT)/kernel/drivers/tcp.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/http.o: $(SRCROOT)/kernel/drivers/http.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
$(OBJ_DIR)/kernel/drivers/http2_client.o: $(SRCROOT)/kernel/drivers/http2_client.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NGHTTP2_INCLUDES) -DNGHTTP2_STATICLIB -DNDEBUG -c $< -o $@
$(OBJ_DIR)/kernel/drivers/brotli_decoder.o: $(SRCROOT)/kernel/drivers/brotli_decoder.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(BROTLI_INCLUDES) -DNDEBUG -c $< -o $@
$(OBJ_DIR)/brotli/%.o: $(BROTLI_DIR)/c/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(BROTLI_INCLUDES) -DNDEBUG -c $< -o $@
$(OBJ_DIR)/kernel/drivers/tls_backend.o: $(SRCROOT)/kernel/drivers/tls_backend.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/tls_backend_openssl.o: $(SRCROOT)/kernel/drivers/tls_backend_openssl.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -DCOS_ENABLE_OPENSSL_BACKEND=1 -c $< -o $@

$(OBJ_DIR)/kernel/drivers/pci.o: $(SRCROOT)/kernel/drivers/pci.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/e1000.o: $(SRCROOT)/kernel/drivers/e1000.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/ac97.o: $(SRCROOT)/kernel/drivers/ac97.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/net.o: $(SRCROOT)/kernel/drivers/net.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


# ---- ACPICA compile rules ----
$(OBJ_DIR)/acpica/%.o: $(ACPICA_SRC_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -DACPI_DEBUG_OUTPUT=0 -DACPI_NO_ERROR_MESSAGES=0 \
		-Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
		-Wno-implicit-function-declaration -Wno-strict-aliasing -c $< -o $@

$(ACPICA_LIB): $(ACPICA_OBJS)
	@echo "[BUILD] Archiving ACPICA..."
	ar rcs $@ $^

# ---- ACPICA power backend ----
$(OBJ_DIR)/kernel/power/acpica_backend.o: $(SRCROOT)/kernel/power/acpica_backend.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -DCOS_ACPICA_PRESENT=1 \
		-Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
		-Wno-implicit-function-declaration -c $< -o $@

$(OBJ_DIR)/kernel/power/acpi_power.o: $(SRCROOT)/kernel/power/acpi_power.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -DCOS_ACPICA_PRESENT=1 \
		-Wno-unused-parameter -c $< -o $@

# acpi_power_enhanced.o removed (conflicts with acpi_power.c)

$(OBJ_DIR)/bearssl/%.o: $(BEARSSL_SRC_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@echo "[BUILD] Archiving BearSSL..."
	ar rcs $@ $^
$(OBJ_DIR)/nghttp2/%.o: $(NGHTTP2_SRC_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NGHTTP2_INCLUDES) -DNGHTTP2_STATICLIB -DNDEBUG -c $< -o $@
$(NGHTTP2_LIB): $(NGHTTP2_OBJS)
	@echo "[BUILD] Archiving nghttp2 (lib-only)..."
	ar rcs $@ $^
$(OBJ_DIR)/quickjs/%.o: $(QUICKJS_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(QUICKJS_DIR) $(QUICKJS_CFLAGS) -c $< -o $@

$(QUICKJS_LIB): $(QUICKJS_OBJS)
	@echo "[BUILD] Archiving QuickJS..."
	ar rcs $@ $^

$(OBJ_DIR)/libwapcaplet/%.o: $(LWAPCAPLET_DIR)/src/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(LWAPCAPLET_INCLUDES) -c $< -o $@

$(LWAPCAPLET_LIB): $(LWAPCAPLET_OBJS)
	@echo "[BUILD] Archiving libwapcaplet..."
	ar rcs $@ $^

$(OBJ_DIR)/libparserutils/%.o: $(LPARSERUTILS_DIR)/src/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(LPARSERUTILS_INCLUDES) -c $< -o $@

$(LPARSERUTILS_LIB): $(LPARSERUTILS_OBJS)
	@echo "[BUILD] Archiving libparserutils..."
	ar rcs $@ $^

$(OBJ_DIR)/libutf8proc/%.o: $(LUTF8PROC_DIR)/src/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(LUTF8PROC_INCLUDES) -c $< -o $@

$(LUTF8PROC_LIB): $(LUTF8PROC_OBJS)
	@echo "[BUILD] Archiving libutf8proc..."
	ar rcs $@ $^

$(OBJ_DIR)/libhubbub/%.o: $(LHUBBUB_DIR)/src/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(LHUBBUB_INCLUDES) $(LHUBBUB_CFLAGS) -c $< -o $@

$(LHUBBUB_LIB): $(LHUBBUB_OBJS)
	@echo "[BUILD] Archiving libhubbub..."
	ar rcs $@ $^

$(OBJ_DIR)/libcss/%.o: $(LCSS_DIR)/src/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(LCSS_INCLUDES) -c $< -o $@

$(LCSS_LIB): $(LCSS_OBJS)
	@echo "[BUILD] Archiving libcss..."
	ar rcs $@ $^

$(OBJ_DIR)/libdom/%.o: $(LDOM_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDOM_INCLUDES) -c $< -o $@

$(LDOM_LIB): $(LDOM_OBJS)
	@echo "[BUILD] Archiving libdom..."
	ar rcs $@ $^

$(OBJ_DIR)/libnsutils/%.o: $(LNSUTILS_DIR)/src/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(LNSUTILS_INCLUDES) -c $< -o $@

$(LNSUTILS_LIB): $(LNSUTILS_OBJS)
	@echo "[BUILD] Archiving libnsutils..."
	ar rcs $@ $^

$(OBJ_DIR)/ns_utils/%.o: $(NS_DIR)/utils/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NS_UTILS_INCLUDES) -c $< -o $@

$(NS_UTILS_LIB): $(NS_UTILS_OBJS)
	@echo "[BUILD] Archiving NetSurf utils..."
	ar rcs $@ $^

$(OBJ_DIR)/ns_content/%.o: $(NS_DIR)/content/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NS_CONTENT_INCLUDES) -c $< -o $@

$(NSBMP_OBJ): $(NSBMP_SRC) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NS_CONTENT_INCLUDES) -c $< -o $@

$(NSBMP_LIB): $(NSBMP_OBJ)
	@echo "[BUILD] Archiving NetSurf libnsbmp..."
	ar rcs $@ $^

$(OBJ_DIR)/svgtiny/%.o: $(SVGTINY_DIR)/src/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NS_CONTENT_INCLUDES) -I$(SVGTINY_DIR)/src -c $< -o $@

$(SVGTINY_LIB): $(SVGTINY_OBJS)
	@echo "[BUILD] Archiving NetSurf libsvgtiny..."
	ar rcs $@ $^

$(OBJ_DIR)/ns_desktop/%.o: $(NS_DIR)/desktop/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NS_CONTENT_INCLUDES) -c $< -o $@

$(NS_CONTENT_LIB): $(NS_CONTENT_OBJS) $(NS_DESKTOP_OBJS)
	@echo "[BUILD] Archiving NetSurf content core + html/css handlers..."
	ar rcs $@ $^

$(OBJ_DIR)/netsurf/%.o: $(COS_NETSURF_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(COS_NETSURF_INCLUDES) -c $< -o $@

$(OBJ_DIR)/ns_js_quickjs/%.o: $(NSJS_QUICKJS_DIR)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(NSJS_QUICKJS_INCLUDES) -c $< -o $@

$(NSJS_QUICKJS_LIB): $(NSJS_QUICKJS_OBJS)
	@echo "[BUILD] Archiving NetSurf QuickJS backend..."
	ar rcs $@ $^

# ---- Compatibility object files ----
$(OBJ_DIR)/compat/memory_stubs.o: $(SRCROOT)/kernel/memory_stubs.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/compat/malloc_stub.o: $(SRCROOT)/lib/malloc_stub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/compat/vga_stub.o: $(SRCROOT)/lib/vga_stub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/compat/serial_stub.o: $(SRCROOT)/lib/serial_stub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/compat/storage_binary_stub.o: $(SRCROOT)/lib/storage_binary_stub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/compat/kernel_stub.o: $(SRCROOT)/lib/kernel_stub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/compat/storage_partition_stub.o: $(SRCROOT)/apps/storage_partition_stub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(COMPAT_LIB): $(COMPAT_OBJS)
	@echo "[BUILD] Archiving compatibility stubs..."
	ar rcs $@ $^

obj/cpp/%.o: $(SRCROOT)/cpp/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
# ---- Linker flags (optimized 64-bit) ----
LDFLAGS = -m64 -nostdlib -nodefaultlibs -no-pie -T $(SRCROOT)/boot/linker.ld \
	-Wl,--build-id=none

# ---- Link kernel ----
# The static archives below are wrapped in --start-group/--end-group:
# plain `ld` only scans each archive once, in the order given, pulling
# in an object only if something *already* processed needs a symbol
# from it. With this many archives now genuinely cross-referencing
# each other (NS_CONTENT needs symbols from NS_UTILS and
# NSJS_QUICKJS; NSJS_QUICKJS needs symbols from QUICKJS_LIB; etc.) a
# single hand-ordered pass is fragile and breaks again every time a
# new cross-reference is added - --start-group/--end-group tells ld
# to keep re-scanning the whole group until nothing new resolves,
# which is the standard fix for this and doesn't require reasoning
# about the exact dependency graph by hand.
$(KERNEL): $(OBJS) $(BEARSSL_LIB) $(NGHTTP2_LIB) $(ACPICA_LIB) $(COMPAT_LIB) $(QUICKJS_LIB) $(LWAPCAPLET_LIB) $(LPARSERUTILS_LIB) $(LUTF8PROC_LIB) $(LHUBBUB_LIB) $(LCSS_LIB) $(LDOM_LIB) $(LNSUTILS_LIB) $(NS_UTILS_LIB) $(NSBMP_LIB) $(SVGTINY_LIB) $(NS_CONTENT_LIB) $(NSJS_QUICKJS_LIB)
	@echo "[BUILD] Linking kernel..."
	$(CXX) $(LDFLAGS) -o $(BUILD_DIR)/kernel.elf $(OBJS) \
		-Wl,--start-group \
		$(BEARSSL_LIB) $(NGHTTP2_LIB) $(ACPICA_LIB) $(COMPAT_LIB) $(QUICKJS_LIB) $(LWAPCAPLET_LIB) $(LPARSERUTILS_LIB) $(LUTF8PROC_LIB) $(LHUBBUB_LIB) $(LCSS_LIB) $(LDOM_LIB) $(LNSUTILS_LIB) $(NS_UTILS_LIB) $(NSBMP_LIB) $(SVGTINY_LIB) $(NS_CONTENT_LIB) $(NSJS_QUICKJS_LIB) \
		-Wl,--end-group \
		$(TLS_BACKEND_EXTRA_LIBS)
	cp $(BUILD_DIR)/kernel.elf $@
	@echo "[BUILD] Kernel linked (ELF): $(BUILD_DIR)/kernel.elf"
	@ls -la $(BUILD_DIR)/kernel.elf

$(PACK_STORAGE): tools/pack_storage.go | $(BUILD_DIR)
	@echo "[BUILD] Building pack_storage with Go..."
	@if command -v $(GO) >/dev/null 2>&1; then \
		$(GO) build -o $@ tools/pack_storage.go; \
	else \
		echo "[BUILD] $(GO) not found; writing stub (storage image will be raw dd)"; \
		printf '#!/bin/sh\necho "pack_storage stub: Go toolchain unavailable, storage image unsupported" >&2\nexit 1\n' > $@; \
		chmod +x $@; \
	fi

$(COSNET): tools/cosnet.go | $(BUILD_DIR)
	@echo "[BUILD] (host) skipping Go build of cosnet; stub available at $@"
	@mkdir -p $(BUILD_DIR)
	@touch $@

# Provide an offline stub for COSNET_STATE so kernel target builds without Go.
# The header is shipped pre-generated in build/cosnet_state.h so cosnet does
# not have to run. Real builds with Go installed override this by deleting
# the cached file.
$(BUILD_DIR)/cosnet_state.h: | $(BUILD_DIR)
	@echo "[BUILD] Using pre-generated offline cosnet_state header"
	@test -s $@ || echo '#define COSNET_REPO_NAME ""' > $@

# ---- Raw storage image for persistent data ----
$(STORAGE_IMG): $(PACK_STORAGE) | $(BUILD_DIR)
	@echo "[BUILD] Creating storage image: $(STORAGE_IMG) ($(STORAGE_SIZE_MB) MB)"
	@if "$(PACK_STORAGE)" -out $(STORAGE_IMG) -size-mb $(STORAGE_SIZE_MB) 2>/dev/null; then \
		echo "[BUILD] Native pack_storage succeeded"; \
	else \
		echo "[BUILD] pack_storage stub: using Python catalog packer"; \
		python3 tools/pack_storage.py --out $(STORAGE_IMG) --size-mb $(STORAGE_SIZE_MB); \
	fi

# ---- Create bootable ISO ----
iso: $(KERNEL)
	mkdir -p iso/boot
	cp $(BUILD_DIR)/kernel.elf iso/boot/kernel.elf
	mkdir -p iso/boot/desktop
	cp $(SRCROOT)/assets/desktop_featured.png iso/boot/desktop/featured.png
	mkdir -p $(GRUB_DIR)
	cp $(SRCROOT)/boot/grub.cfg $(GRUB_CFG)
	@if [ -d /usr/lib/grub/x86_64-efi ] || [ -d /usr/lib/grub/i386-efi ]; then \
		echo "[BUILD] EFI GRUB platform files found -> building hybrid BIOS+UEFI ISO"; \
	else \
		echo "[BUILD] WARNING: no /usr/lib/grub/x86_64-efi (grub-efi-amd64-bin) found."; \
		echo "[BUILD]          grub-mkrescue will only produce a BIOS-bootable ISO."; \
		echo "[BUILD]          Install grub-efi-amd64-bin grub-pc-bin mtools to get UEFI boot."; \
	fi
	grub-mkrescue -o $(ISO) iso
	@echo "[BUILD] ISO created: $(ISO)"
	@if command -v xorriso >/dev/null 2>&1; then \
		if xorriso -indev $(ISO) -report_el_torito plain 2>/dev/null | grep -qi "EFI"; then \
			echo "[BUILD] VERIFIED: ISO contains a UEFI El Torito boot entry."; \
		else \
			echo "[BUILD] VERIFIED: ISO is BIOS-only (no UEFI El Torito entry found)."; \
			echo "[BUILD]           See the WARNING above if this is unexpected."; \
		fi; \
	fi

# ---- UEFI Secure Boot artifacts ----
# The signed standalone GRUB embeds both this immutable configuration and the
# exact kernel ELF. Firmware verification of BOOTX64.EFI therefore covers the
# only kernel payload the Secure-Boot configuration can launch.
secureboot-dev-keys:
	chmod 700 tools/generate_secureboot_dev_keys.sh
	./tools/generate_secureboot_dev_keys.sh

secureboot-esp: $(KERNEL)
	@test -n "$(SECUREBOOT_KEY)" || { echo "Set SECUREBOOT_KEY to an external PEM private key."; exit 2; }
	@test -n "$(SECUREBOOT_CERT)" || { echo "Set SECUREBOOT_CERT to its external PEM certificate."; exit 2; }
	chmod 700 tools/build_secureboot_uefi.sh
	./tools/build_secureboot_uefi.sh --key "$(SECUREBOOT_KEY)" --cert "$(SECUREBOOT_CERT)" --kernel "$(BUILD_DIR)/kernel.elf" --config "$(SRCROOT)/boot/grub_secure.cfg" --out-dir "$(SECUREBOOT_DIR)"

# Alias retained for CI/manual workflows: the build script emits both the FAT
# ESP and the UEFI-first El Torito ISO from one signed BOOTX64.EFI.
secureboot-iso: secureboot-esp
	@test -s "$(SECUREBOOT_DIR)/C-OS_4.0.8_alpha_secure.iso"
	@echo "[SECUREBOOT] UEFI-first ISO ready: $(SECUREBOOT_DIR)/C-OS_4.0.8_alpha_secure.iso"

.PHONY: secureboot-dev-keys secureboot-esp secureboot-iso

# ---- Clean ----
# NOTE: this is intentionally named clean-cos rather than "clean" because
# py.mk/mkrules.mk (included below) also define a "clean" target, and GNU
# Make only keeps the last-parsed recipe for a duplicate target name. A
# combined "clean" target that runs both is defined after those includes.
clean-cos:
	rm -rf $(OBJ_DIR) $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/compat_stubs.a $(BUILD_DIR)/libbearssl.a $(BUILD_DIR)/isofiles $(ISO) iso/boot/kernel.elf $(GRUB_DIR)
	@echo "[CLEAN] Done"

run: iso $(STORAGE_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -drive file=$(STORAGE_IMG),format=raw,if=ide,index=0,media=disk -m 512M -serial stdio -audiodev pa,id=snd0 -device AC97,audiodev=snd0

cosnet: $(COSNET)

net-sync: $(COSNET) $(COSNET_STATE)
	@echo "[BUILD] cosnet ready: $(COSNET)"

$(TEXT_BROWSER): tools/text_browser.go tools/dom_css.go | $(BUILD_DIR)
	$(GO) build -o $@ tools/text_browser.go tools/dom_css.go

text-browser: $(TEXT_BROWSER)
	@echo "[BUILD] text browser ready: $(TEXT_BROWSER)"

open-browser: $(TEXT_BROWSER)
	@echo "[BUILD] browser launcher ready. Run: tools/open-browser.sh"

run-browser: $(TEXT_BROWSER)
	$(TEXT_BROWSER)


# acpi_stub.o removed - replaced by full ACPICA integration

$(OBJ_DIR)/kernel/drivers/usb.o: $(SRCROOT)/kernel/drivers/usb.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/tusb_port_pc.o: $(SRCROOT)/kernel/drivers/tusb_port_pc.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/tusb_hid_bridge.o: $(SRCROOT)/kernel/drivers/tusb_hid_bridge.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/tinyusb/tusb.o: $(TINYUSB_DIR)/tusb.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/tinyusb/tusb_fifo.o: $(TINYUSB_DIR)/common/tusb_fifo.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/tinyusb/usbh.o: $(TINYUSB_DIR)/host/usbh.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/tinyusb/hub.o: $(TINYUSB_DIR)/host/hub.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/drivers/tusb_msc_bridge.o: $(SRCROOT)/kernel/drivers/tusb_msc_bridge.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/tinyusb/hid_host.o: $(TINYUSB_DIR)/class/hid/hid_host.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/tinyusb/msc_host.o: $(TINYUSB_DIR)/class/msc/msc_host.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/third_party/tinyusb/ehci.o: $(TINYUSB_DIR)/portable/ehci/ehci.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TINYUSB_INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/system/mk_storage_compat.o: $(SRCROOT)/kernel/system/mk_storage_compat.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---- MicroPython upstream build ----
TOP := $(MP_SRC_DIR)
BUILD := build
HEADER_BUILD := $(BUILD)/genhdr
PY_SRC := $(MP_SRC_PY)

include $(MP_SRC_PY)/py.mk
include $(MP_SRC_PY)/mkrules.mk

# Combined clean: runs both the C-OS clean and MicroPython's own clean.
# Defined after both includes so it is the final (winning) recipe for
# "clean", instead of mkrules.mk's clean silently shadowing clean-cos.
.PHONY: clean
clean: clean-cos
	$(RM) -rf $(BUILD) $(CLEAN_EXTRA)

override AS := nasm
override ASFLAGS := -f elf64


$(OBJ_DIR)/kernel/ai/micropython_debug.o: $(SRCROOT)/kernel/ai/micropython_debug.c | $(OBJ_DIR) $(MICROPY_GENHDR_DIR)/qstrdefs.generated.h
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MICROPY_INC) -c $< -o $@

# ---- Additional Build Rules for C-OS 5.0.0 Features ----
$(OBJ_DIR)/kernel/security/permission_manager.o: $(SRCROOT)/kernel/security/permission_manager.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/kernel/system/storage_manager.o: $(SRCROOT)/kernel/system/storage_manager.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/notification_center.o: $(SRCROOT)/gui/notification_center.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/gui/theme_system.o: $(SRCROOT)/gui/theme_system.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Generated C/C++ header dependencies.  Every compilation rule already uses
# CFLAGS or CXXFLAGS, so -MMD -MP above writes a sibling .d file for each
# object. Missing files are intentionally ignored on the first incremental
# build; later structural-header edits rebuild all affected objects instead of
# retaining incompatible layouts.
-include $(OBJS:.o=.d)

# ---- Build reproducibility helper (課題整理.md 2.4対応) ----
# `make check` はビルドを実行し、ログを保存した上で
# エラー・警告の有無を機械的に判定する。CIが無い状態でも、
# 最低限「ビルドが壊れていないか」を毎回同じ手順で確認できるようにする。
BUILD_LOG = build_check.log

.PHONY: check
check:
	@echo "[check] Running full build, logging to $(BUILD_LOG) ..."
	@$(MAKE) clean >/dev/null 2>&1 || true
	@( $(MAKE) 2>&1 | tee $(BUILD_LOG) ) ; status=$$? ; \
	if grep -qE "error:|Error [0-9]+" $(BUILD_LOG); then \
		echo "[check] FAILED: errors found in $(BUILD_LOG)"; \
		exit 1; \
	elif [ $$status -ne 0 ]; then \
		echo "[check] FAILED: make exited with status $$status (see $(BUILD_LOG))"; \
		exit 1; \
	else \
		warn_count=$$(grep -cE "warning:" $(BUILD_LOG) || true); \
		echo "[check] OK: build succeeded ($$warn_count warnings, see $(BUILD_LOG))"; \
	fi
