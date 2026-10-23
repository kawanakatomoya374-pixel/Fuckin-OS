#!/usr/bin/env bash
# ffmpeg_min_build.sh [dir]: builds a small static-pie ffmpeg 6.1.1 (wav/mp3/flac/raw/image2, lavfi sources, pthreads) for C-OS's Linux layer.
# Run it with LINUX_ROOTFS="<dir>/ffmpeg/ffmpeg:/linux/bin/ffmpeg" (docs/LINUX_COMPAT.md).
set -euo pipefail
D=${1:-/tmp/ff}; mkdir -p "$D"; cd "$D"
[ -d ffmpeg ] || git clone --depth 1 --branch n6.1.1 https://github.com/FFmpeg/FFmpeg ffmpeg
cd ffmpeg
./configure --disable-everything --enable-ffmpeg --disable-autodetect --disable-doc --disable-debug --disable-network \
  --disable-x86asm --enable-pthreads --enable-pic --enable-static --disable-shared --extra-cflags="-fPIE -Os" --extra-ldflags="-static-pie" \
  --enable-protocol=file,pipe --enable-demuxer=wav,mp3,pcm_s16le,rawvideo,image2,flac --enable-muxer=wav,pcm_s16le,rawvideo,null,image2,mp3 \
  --enable-decoder=pcm_s16le,pcm_u8,mp3float,flac,rawvideo,pcm_f32le --enable-encoder=pcm_s16le,pcm_u8,rawvideo,pcm_f32le,ppm,bmp \
  --enable-parser=mpegaudio,flac --enable-filter=aresample,anull,null,scale,format,aformat,volume,anullsrc,color,testsrc,sine \
  --enable-swscale --enable-swresample --enable-avfilter --enable-indev=lavfi
make -j"$(nproc)"
ls -la ffmpeg
