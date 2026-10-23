Single-header libraries built on C-OS with the on-device TinyCC (tcc.c-os)
==========================================================================
Everything here builds in the Terminal, with the SDK in /system/sdk, and the project also builds from C-OS Studio.

  cd /system/sdk/examples/singleheader
  tcc stb_image_demo.c -o /tmp/stb_image_demo.c-os     (stb_image.h : PNG / JPEG / BMP ... decoder)
  tcc minimp3_demo.c -o /tmp/minimp3_demo.c-os         (minimp3.h   : MP3 decoder)
  tcc stb_truetype_demo.c -o /tmp/stb_truetype_demo.c-os (stb_truetype.h : TrueType rasteriser)
  tcc vec_main.c vec_util.c -o /tmp/vec_demo.c-os      (mini_vec.h  : a small stb-style header, the implementation in one file)

Programs must end in .c-os to be started from the Terminal. Copy the folder (File Manager) to change it; /system is read-only for programs.
In C-OS Studio: open this folder as a project (Open Folder) and press Build / F5.

Py2C's own single header
------------------------
/apps/Py2C/include/python_code_to_c_single.h is the whole Python-to-C converter and its runtime in one header (about 1.8 MB).
Define P2C_SINGLE_HEADER_IMPLEMENTATION in exactly one .c file, then:

  tcc -U__linux__ -I/apps/Py2C/include -DPYTHON_CODE_TO_C_NO_TLS /apps/Py2C/tests/test_single_header.c -o /tmp/tsh.c-os

-U__linux__ keeps the header from including <pthread.h> (C-OS has none), PYTHON_CODE_TO_C_NO_TLS is needed because programs on C-OS have no thread pointer.
The freestanding configuration (-DPYTHON_CODE_TO_C_NO_STDLIB) compiles too.
