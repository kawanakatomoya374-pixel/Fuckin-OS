/* Media.c-os - small MP4 player for C-OS.
 * Supported first profile: ISO-BMFF MP4 containing Motion-JPEG video and
 * an MP3/AAC audio track. It streams samples from a <=100 MiB file, decodes JPEG
 * frames with stb_image and sends MP3/AAC PCM through the C-OS audio API.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdarg.h>
#include "cos.h"
#include "cos_ui.h"
#include "../../third_party/edge264/edge264.h"
#include "../../third_party/faad2/include/neaacdec.h"
#include "../../third_party/ffmpeg_vp9/cos_vp9.h"
#include "../../third_party/ffmpeg_hevc/cos_hevc.h"
#include "../../third_party/dav1d_src/include/dav1d/dav1d.h"
#include "../../third_party/dav1d_src/include/dav1d/data.h"
#include "../../third_party/dav1d_src/include/dav1d/picture.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "../../third_party/minimp3.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ASSERT(x) ((void)0)
#include "../../third_party/stb_image.h"

#ifndef COS_VALIDATION_FRAME_CRC
#define COS_VALIDATION_FRAME_CRC 0
#endif
#define MAX_BYTES (512ull * 1024ull * 1024ull)
/* 4K: frame_prepare()/media_push_yuv420() already allocate dynamically
 * with an overflow check (pixels > SIZE_MAX/4), so this cap is just a
 * sanity ceiling against malformed dimensions, not a real buffer limit -
 * raised from 1080p since 4K is common for modern phone/camera MP4s. */
#define MAX_VIDEO_W 3840
#define MAX_VIDEO_H 2160
#define MAX_SAMPLES 65536
#define MAX_TABLE_ENTRIES 4096
#define W 820
#define H 560
#define VIDEO 1
#define AUDIO 2

typedef struct {
    uint64_t off, size, dts, dur;
    int64_t pts_offset;
    uint32_t flags;
    uint8_t sync;
    uint32_t decode_index;
    uint32_t display_index;
} sample_t;
typedef struct { uint32_t first_chunk, samples_per_chunk, sample_desc; } stsc_entry_t;
typedef struct { uint32_t count, delta; } stts_entry_t;
typedef struct { uint32_t count; int64_t offset; } ctts_entry_t;

/* ISO-BMFF sample-entry tags. The parser recognizes all common modern video
 * tags so unsupported codecs are reported accurately instead of being fed to
 * the H.264 decoder by accident. */
#define CODEC_JPEG 1
#define CODEC_MP3 2
#define CODEC_H264 3
#define CODEC_AAC 4
#define CODEC_HEVC 5
#define CODEC_AV1 6
#define CODEC_VP9 7
typedef struct { int kind, codec; sample_t s[MAX_SAMPLES]; int n; uint32_t timescale;
    uint32_t track_id, default_sample_duration, default_sample_size, default_sample_flags;
    bool edit_valid; int64_t edit_media_time; uint64_t edit_empty_duration;
    uint64_t chunk_offsets[MAX_SAMPLES]; uint32_t chunk_count;
    stsc_entry_t stsc[MAX_TABLE_ENTRIES]; uint32_t stsc_count;
    stts_entry_t stts[MAX_TABLE_ENTRIES]; uint32_t stts_count;
    uint32_t sync_samples[MAX_SAMPLES]; uint32_t sync_count;
    ctts_entry_t ctts[MAX_TABLE_ENTRIES]; uint32_t ctts_count; } track_t;
static uint8_t *file_data; static uint64_t file_size; static uint32_t movie_timescale=1000;
static track_t tracks[8]; static int ntracks;
static char path_buf[256], status_buf[160], stats_buf[120];       /* stats_buf: what the player measures about itself while it plays (shown under the status text) */
static uint64_t playback_start_ms, audio_start_ms, audio_written_frames;
static bool audio_clock_valid;
static int64_t win; static cos_win_info_t wi; static cui_canvas cv;
static bool playing, paused, have_video, have_audio, audio_opened;
static int video_track=-1, audio_track=-1, video_pos; static uint64_t next_frame_ms; static bool video_clock_valid;
static uint8_t *audio_data; static size_t audio_len, audio_pos; static mp3dec_t mp3; static int audio_rate=44100, audio_channels=2;
static unsigned char *frame_rgba; static int frame_w, frame_h;
#define MEDIA_FRAME_POOL_SLOTS 3
static struct { uint8_t *data; size_t capacity; } frame_pool[MEDIA_FRAME_POOL_SLOTS];
static unsigned frame_pool_next;
static uint32_t frame_generation, presented_generation;
static uint32_t packet_decode_pos, frame_output_pos, frame_present_pos, dropped_frames, decode_errors;
static uint64_t pts_first_ms, pts_last_ms, decode_total_ms, decode_max_ms;
static bool have_pts_sample;
static uint64_t bytes_read;
static Edge264Decoder *h264_dec;
static uint8_t *nal_buf;
static uint8_t *h264_cfg; static size_t h264_cfg_len; static uint8_t h264_nal_length_size=4; static uint8_t h264_profile_idc, h264_level_idc;
static NeAACDecHandle aac_dec;
static bool h264_flushed, h264_drained; /* set once the last sample is fed / the reorder queue is empty */
static uint8_t *aac_cfg; static unsigned long aac_cfg_len;
static void *vp9_dec; static bool vp9_dec_open;
/* HEVC: hvcC configuration record, decoder handle, and the display-order -> sample map used to time
 * frames (the decoder returns frames in display order, delayed relative to the samples fed in). */
static void *hevc_dec; static bool hevc_dec_open, hevc_drained; static uint8_t *hevc_cfg; static size_t hevc_cfg_len;
static int *hevc_disp2dec; static int hevc_out_count;
static Dav1dContext *av1_dec; static bool av1_dec_open, av1_drained, av1_in_pending; static Dav1dData av1_in; static int av1_out_count;
extern size_t cos_pthread_arena_heap;
static void av1_log_cb(void *ck,const char *fmt,va_list ap){(void)ck;if(cos_in_worker())return;char b[200];vsnprintf(b,sizeof b,fmt,ap);cos_puts("[dav1d] ");cos_puts(b);}

static uint32_t be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t be64(const uint8_t *p){return ((uint64_t)be32(p)<<32)|be32(p+4);}
static bool typ(const uint8_t *p,const char *s){return !memcmp(p,s,4);}
static void msg(const char *s){snprintf(status_buf,sizeof status_buf,"%s",s);}
static bool in_file(uint64_t p,uint64_t n){return p<=file_size && n<=file_size-p;}
static bool h264_cfg_append(const uint8_t *p,size_t n){
    uint8_t *q=realloc(h264_cfg,h264_cfg_len+4+n);if(!q)return false;h264_cfg=q;
    h264_cfg[h264_cfg_len]=0;h264_cfg[h264_cfg_len+1]=0;h264_cfg[h264_cfg_len+2]=0;h264_cfg[h264_cfg_len+3]=1;
    memcpy(h264_cfg+h264_cfg_len+4,p,n);h264_cfg_len+=4+n;return true;
}
static void expand_timestamps(track_t *t);
static uint64_t sample_pts_ms(const track_t *t, int index);
static void assign_display_order(track_t *t);
static void expand_sample_offsets(track_t *t){
    if(!t||!t->chunk_count||!t->stsc_count)return;
    uint32_t sample=0;
    for(uint32_t chunk=1;chunk<=t->chunk_count&&sample<(uint32_t)t->n;chunk++){
        uint32_t ei=0;for(uint32_t i=1;i<t->stsc_count;i++){if(t->stsc[i].first_chunk>chunk)break;ei=i;}
        uint32_t count=t->stsc[ei].samples_per_chunk;uint64_t off=t->chunk_offsets[chunk-1];
        for(uint32_t j=0;j<count&&sample<(uint32_t)t->n;j++,sample++){t->s[sample].off=off;off+=t->s[sample].size;}
    }
    expand_timestamps(t);
    assign_display_order(t);
}
static void expand_timestamps(track_t *t){
    if(!t) return;
    uint32_t k=0; uint64_t dts=0;
    for(uint32_t i=0;i<t->stts_count && k<(uint32_t)t->n;i++){
        for(uint32_t j=0;j<t->stts[i].count && k<(uint32_t)t->n;j++,k++){
            t->s[k].dts=dts; t->s[k].dur=t->stts[i].delta; dts+=t->stts[i].delta;
        }
    }
    k=0;
    for(uint32_t i=0;i<t->ctts_count && k<(uint32_t)t->n;i++)
        for(uint32_t j=0;j<t->ctts[i].count && k<(uint32_t)t->n;j++,k++)
            t->s[k].pts_offset=t->ctts[i].offset;
    for(;k<(uint32_t)t->n;k++) t->s[k].pts_offset=0;
}
static uint64_t sample_pts_ms(const track_t *t, int index){
    if(!t || index<0 || index>=t->n || !t->timescale) return 0;
    /* DTS is unsigned in BMFF, while CTTS may be signed.  Do not cast the
     * DTS to int64_t before applying the offset: valid long-running files
     * can cross INT64_MAX ticks and must not wrap to an early timestamp. */
    uint64_t ticks=t->s[index].dts;
    int64_t off=t->s[index].pts_offset;
    if(off<0){ uint64_t neg=(uint64_t)(-(off+1))+1u; ticks = ticks>neg ? ticks-neg : 0; }
    else { uint64_t add=(uint64_t)off; ticks = UINT64_MAX-ticks<add ? UINT64_MAX : ticks+add; }
    if(t->edit_valid && t->edit_media_time>=0){ uint64_t mt=(uint64_t)t->edit_media_time; ticks=ticks>mt?ticks-mt:0; }
    __uint128_t scaled=(__uint128_t)ticks*1000u + t->timescale/2u;
    scaled/=t->timescale;
    uint64_t media_ms=scaled>(__uint128_t)UINT64_MAX?UINT64_MAX:(uint64_t)scaled;
    if(t->edit_valid && t->edit_empty_duration && movie_timescale){ __uint128_t delay=(__uint128_t)t->edit_empty_duration*1000u+movie_timescale/2u; delay/=movie_timescale; if(delay>(__uint128_t)UINT64_MAX-media_ms)return UINT64_MAX; media_ms+=(uint64_t)delay; }
    return media_ms;
}
static void assign_display_order(track_t *t){
    if(!t) return;
    for(int i=0;i<t->n;i++){
        uint32_t rank=0;
        uint64_t pts_i=sample_pts_ms(t,i);
        for(int j=0;j<t->n;j++){
            uint64_t pts_j=sample_pts_ms(t,j);
            if(pts_j<pts_i || (pts_j==pts_i && j<i)) rank++;
        }
        t->s[i].decode_index=(uint32_t)i;
        t->s[i].display_index=rank;
    }
}
static bool frame_prepare(int w, int h){
    if(w<=0 || h<=0 || w>MAX_VIDEO_W || h>MAX_VIDEO_H) return false;
    size_t pixels=(size_t)w*(size_t)h;
    if(pixels > (SIZE_MAX/4u)) return false;
    unsigned slot=frame_pool_next++ % MEDIA_FRAME_POOL_SLOTS;
    size_t need=pixels*4u;
    if(frame_pool[slot].capacity<need){
        uint8_t *q=(uint8_t*)realloc(frame_pool[slot].data,need);
        if(!q) return false;
        frame_pool[slot].data=q; frame_pool[slot].capacity=need;
    }
    frame_rgba=frame_pool[slot].data; frame_w=w; frame_h=h; frame_generation++;
    return true;
}
/* Branch-free conversion: chroma terms and the clamp come from small tables (Q10 BT.601, identical results
 * to the kernel's arithmetic version); every table lookup replaces several translated compare/branch ops. */
static int cv_rv[256], cv_bu[256], cv_gu[256], cv_gv[256]; static uint8_t cv_clp[1024]; static bool cv_ready;
static void conv_init(void){
    if(cv_ready) return;
    for(int i=0;i<256;i++){int c=i-128;cv_rv[i]=(c*1436)>>10;cv_bu[i]=(c*1814)>>10;cv_gu[i]=c*352;cv_gv[i]=c*731;}
    for(int i=0;i<1024;i++){int v=i-256;cv_clp[i]=(uint8_t)(v<0?0:v>255?255:v);}
    cv_ready=true;
}
typedef struct { uint32_t *dst; const uint8_t *py,*pu,*pv; int w,sy,sc,y0,y1,bd; volatile uint32_t *done; } conv_band_t;
#define CVPIX(d,Y,rv,gd,bu) (d)=0xFF000000u|((uint32_t)cv_clp[256+(Y)+(rv)]<<16)|((uint32_t)cv_clp[256+(Y)-(gd)]<<8)|cv_clp[256+(Y)+(bu)]
static void conv_band(void *a){
    const conv_band_t *t=(const conv_band_t*)a; const int w=t->w, sh=t->bd-8;
    for(int y=t->y0;y<t->y1;y++){
        uint32_t *d=t->dst+(size_t)y*w; int x=0;
        if(sh>0){
            const uint16_t *yr=(const uint16_t*)(t->py+(size_t)y*t->sy),*ur=(const uint16_t*)(t->pu+(size_t)(y>>1)*t->sc),*vr=(const uint16_t*)(t->pv+(size_t)(y>>1)*t->sc);
            for(;x<w;x+=2){
                int U=ur[x>>1]>>sh,V=vr[x>>1]>>sh,rv=cv_rv[V],gd=(cv_gu[U]+cv_gv[V])>>10,bu=cv_bu[U];
                int Y0=yr[x]>>sh; CVPIX(d[x],Y0,rv,gd,bu);
                if(x+1<w){int Y1=yr[x+1]>>sh; CVPIX(d[x+1],Y1,rv,gd,bu);}
            }
        }else{
            const uint8_t *yr=t->py+(size_t)y*t->sy,*ur=t->pu+(size_t)(y>>1)*t->sc,*vr=t->pv+(size_t)(y>>1)*t->sc;
            for(;x<w;x+=2){
                int U=ur[x>>1],V=vr[x>>1],rv=cv_rv[V],gd=(cv_gu[U]+cv_gv[V])>>10,bu=cv_bu[U];
                int Y0=yr[x]; CVPIX(d[x],Y0,rv,gd,bu);
                if(x+1<w){int Y1=yr[x+1]; CVPIX(d[x+1],Y1,rv,gd,bu);}
            }
        }
    }
    __atomic_fetch_add(t->done,1,__ATOMIC_RELEASE);
}
/* Display-resolution conversion: the compositor scales the picture to the window with nearest-neighbour sampling
 * anyway, so when the window is smaller than the video only the pixels that will be shown are converted. */
typedef struct { uint32_t *dst; const uint8_t *py,*pu,*pv; int ow,oh,sw,sh_,sy,sc,y0,y1,bd; const int *colmap; volatile uint32_t *done; } scale_band_t;
static void scale_band(void *a){
    const scale_band_t *t=(const scale_band_t*)a; const int sh=t->bd-8, ow=t->ow; const int *cm=t->colmap;
    for(int y=t->y0;y<t->y1;y++){
        int srow=(int)(((int64_t)y*t->sh_)/t->oh); uint32_t *d=t->dst+(size_t)y*ow;
        if(sh>0){
            const uint16_t *yr=(const uint16_t*)(t->py+(size_t)srow*t->sy),*ur=(const uint16_t*)(t->pu+(size_t)(srow>>1)*t->sc),*vr=(const uint16_t*)(t->pv+(size_t)(srow>>1)*t->sc);
            for(int x=0;x<ow;x++){int sx=cm[x],U=ur[sx>>1]>>sh,V=vr[sx>>1]>>sh,Y=yr[sx]>>sh;CVPIX(d[x],Y,cv_rv[V],(cv_gu[U]+cv_gv[V])>>10,cv_bu[U]);}
        }else{
            const uint8_t *yr=t->py+(size_t)srow*t->sy,*ur=t->pu+(size_t)(srow>>1)*t->sc,*vr=t->pv+(size_t)(srow>>1)*t->sc;
            for(int x=0;x<ow;x++){int sx=cm[x],U=ur[sx>>1],V=vr[sx>>1],Y=yr[sx];CVPIX(d[x],Y,cv_rv[V],(cv_gu[U]+cv_gv[V])>>10,cv_bu[U]);}
        }
    }
    __atomic_fetch_add(t->done,1,__ATOMIC_RELEASE);
}
static int cv_colmap[3840] __attribute__((aligned(4096))); static int cv_colmap_ow,cv_colmap_w;
#define CONV_BANDS 8
static int vid_idx; static uint32_t *vid_buf[2]; static int vid_w,vid_h; static bool vid_zc_failed;
/* Zero-copy path: convert straight into the kernel-mapped RGBA buffer in bands that idle worker cores pick up. */
static bool media_push_zerocopy(const uint8_t *py,const uint8_t *pu,const uint8_t *pv,int w,int h,int sy,int sc,int bd,int x,int y,int dw,int dh){
    if(vid_zc_failed) return false;
    conv_init();
    int ow=w,oh=h; bool down=(int64_t)dw*dh*4<(int64_t)w*h*3&&dw>=16&&dh>=16&&dw<=3840;
    if(down){ow=dw;oh=dh;}
    if(!vid_buf[0]||vid_w!=ow||vid_h!=oh){
        uint32_t *b1=NULL; uint32_t *b0=cos_win2_video_map(win,ow,oh,&b1);
        if(!b0||!b1){vid_zc_failed=true;vid_buf[0]=NULL;return false;}
        vid_buf[0]=b0;vid_buf[1]=b1;vid_w=ow;vid_h=oh;vid_idx=0;
    }
    uint32_t *dst=vid_buf[vid_idx];
    volatile uint32_t done=0; int n=0;
    if(down){
        if(cv_colmap_ow!=ow||cv_colmap_w!=w){for(int xx=0;xx<ow;xx++) cv_colmap[xx]=(int)(((int64_t)xx*w)/ow);cv_colmap_ow=ow;cv_colmap_w=w;}
        scale_band_t bands[CONV_BANDS]; int per=(oh+CONV_BANDS-1)/CONV_BANDS;
        for(int y0=0;y0<oh;y0+=per,n++){
            bands[n]=(scale_band_t){dst,py,pu,pv,ow,oh,w,h,sy,sc,y0,y0+per<oh?y0+per:oh,bd,cv_colmap,&done};
            if(n>0 && !cos_job_push(scale_band,&bands[n])) scale_band(&bands[n]);
        }
        scale_band(&bands[0]);
        while(__atomic_load_n(&done,__ATOMIC_ACQUIRE)<(uint32_t)n){ if(!cos_idle_work())__builtin_ia32_pause(); }
    }else{
        conv_band_t bands[CONV_BANDS]; int per=((h+CONV_BANDS-1)/CONV_BANDS+1)&~1;
        for(int y0=0;y0<h;y0+=per,n++){
            bands[n]=(conv_band_t){dst,py,pu,pv,w,sy,sc,y0,y0+per<h?y0+per:h,bd,&done};
            if(n>0 && !cos_job_push(conv_band,&bands[n])) conv_band(&bands[n]);
        }
        conv_band(&bands[0]);
        while(__atomic_load_n(&done,__ATOMIC_ACQUIRE)<(uint32_t)n){ if(!cos_idle_work())__builtin_ia32_pause(); }
    }
    if(cos_win2_video_commit(win,vid_idx,ow,oh,x,y,dw,dh)!=0){vid_zc_failed=true;vid_buf[0]=NULL;return false;}
    vid_idx^=1;
    return true;
}
/* Pushes one decoded I420 frame straight to the kernel compositor instead
 * of this process converting it to RGBA itself first (see
 * cos_win2_video_frame_yuv420()'s comment in cos.h). The kernel copies
 * the three planes and does the YUV->RGBA conversion on whichever CPUs
 * are free - including AP cores this single-threaded ring-3 process can
 * never reach - so this is strictly less work here, not more. Geometry
 * mirrors draw()'s aspect-fit math exactly (same bx/by/bw/bh), since the
 * decoder's output planes are only valid for the scope of the codec call
 * that produced them (edge264_return_frame()/dav1d_picture_unref() free
 * them right after), so the push has to happen now rather than from the
 * next draw() tick. */
static bool media_push_yuv420(const uint8_t *py, const uint8_t *pu, const uint8_t *pv,
                              int w, int h, int stride_y, int stride_c, int bit_depth) {
    if (w <= 0 || h <= 0 || w > MAX_VIDEO_W || h > MAX_VIDEO_H) return false;
    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) return false;
#if COS_VALIDATION_FRAME_TIMES
    /* Performance runs: a time stamp every 8 frames, without the CRC cost. */
    {
        static unsigned ft_n; static uint64_t ft_t0;
        if (ft_n == 0) ft_t0 = cos_time_ms();
        if ((++ft_n & 7u) == 0) { char b[96]; snprintf(b, sizeof b, "[FT] n=%u ms=%llu %dx%d\n", ft_n, (unsigned long long)(cos_time_ms() - ft_t0), w, h); cos_puts(b); }
    }
#endif
#if COS_VALIDATION_FRAME_CRC
    /* Validation only: CRC32 of each decoded plane (cropped to the visible
     * size, little-endian samples for 10-bit), printed to the serial console
     * so every frame can be compared against a reference decode (ffmpeg). */
    {
        static unsigned frame_no;
        const int bps = bit_depth > 8 ? 2 : 1, cw = (w + 1) / 2, ch = (h + 1) / 2;
        const uint8_t *pl[3] = {py, pu, pv};
        const int pw[3] = {w, cw, cw}, ph[3] = {h, ch, ch}, ps[3] = {stride_y, stride_c, stride_c};
        unsigned crc[3];
        static uint32_t tab[256];
        if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u)); tab[i] = c; }
        for (int p = 0; p < 3; p++) {
            uint32_t c = 0xFFFFFFFFu;
            for (int y = 0; y < ph[p]; y++) {
                const uint8_t *r = pl[p] + (long)y * ps[p];
                for (int x = 0; x < pw[p] * bps; x++) c = tab[(c ^ r[x]) & 0xFF] ^ (c >> 8);
            }
            crc[p] = ~c;
        }
        char b[96];
        snprintf(b, sizeof b, "[FRAMECRC] %u %dx%d %d y=%08x u=%08x v=%08x\n", frame_no++, w, h, bit_depth, crc[0], crc[1], crc[2]);
        cos_puts(b);
    }
#endif
    int bx=18, by=72, bw=cv.w-36, bh=cv.h-154;
    float sx=(float)(bw-24)/w, sy=(float)(bh-24)/h, sc=sx<sy?sx:sy;
    int dw=(int)(w*sc), dh=(int)(h*sc), x=bx+(bw-dw)/2, y=by+(bh-dh)/2;
    if(media_push_zerocopy(py,pu,pv,w,h,stride_y,stride_c,bit_depth,x,y,dw,dh)){frame_w=w;frame_h=h;frame_generation++;return true;}
    cos_video_yuv420_t f;
    f.y=py; f.u=pu; f.v=pv; f.w=w; f.h=h; f.stride_y=stride_y; f.stride_c=stride_c;
    f.x=x; f.y_pos=y; f.dst_w=dw; f.dst_h=dh; f.bit_depth=bit_depth;
    if(cos_win2_video_frame_yuv420(win, &f) != 0) return false;
    /* frame_rgba stays NULL for this path (the kernel already has the
     * presented plane, in its own video_pixels buffer) - frame_w/h and
     * frame_generation are still tracked here so draw()/present() can
     * tell "a frame exists" apart from "no file open yet" and keep
     * counting presented frames the same way the RGBA path does. */
    frame_w=w; frame_h=h; frame_generation++;
    return true;
}
static void media_log_stats(const char *codec){
    char b[192];
    uint64_t avg=frame_output_pos ? decode_total_ms/(uint64_t)frame_output_pos : 0;
    snprintf(b,sizeof b,"[VALIDATION] codec=%s packets=%u decoded=%u presented=%u dropped=%u errors=%u pts=%llu..%llu decode_avg_ms=%llu decode_max_ms=%llu\n",
        codec,(unsigned)packet_decode_pos,(unsigned)frame_output_pos,(unsigned)frame_present_pos,
        (unsigned)dropped_frames,(unsigned)decode_errors,
        (unsigned long long)(have_pts_sample?pts_first_ms:0),(unsigned long long)(have_pts_sample?pts_last_ms:0),
        (unsigned long long)avg,(unsigned long long)decode_max_ms);
    cos_puts(b);
}
static uint64_t sample_duration_ms(const track_t *t, int index){
    if(!t || index<0 || index>=t->n || !t->timescale) return 40;
    uint64_t d=t->s[index].dur;
    /* Broken or fragmented files occasionally omit a usable stts duration.
     * For VFR the next presentation timestamp is the only correct fallback;
     * never invent a fixed frame rate when the BMFF carries timing data. */
    if(!d && index+1<t->n){
        uint64_t a=sample_pts_ms(t,index), b=sample_pts_ms(t,index+1);
        if(b>a) return b-a;
    }
    __uint128_t scaled=(__uint128_t)d*1000u + t->timescale/2u;
    scaled/=t->timescale;
    if(scaled==0) return 1;
    return scaled>(__uint128_t)UINT64_MAX ? UINT64_MAX : (uint64_t)scaled;
}
static void mark_sync_samples(track_t *t){
    if(!t) return;
    for(int i=0;i<t->n;i++) t->s[i].sync = t->sync_count ? 0 : 1;
    for(uint32_t i=0;i<t->sync_count;i++){
        uint32_t n=t->sync_samples[i];
        if(n>=1 && n<=(uint32_t)t->n) t->s[n-1].sync=1;
    }
}
static uint64_t media_clock_ms(void){
    if(audio_clock_valid && audio_written_frames>0){ /* no audio actually queued yet (decode failure/startup): fall through to the wall clock so video never stalls */
        cos_audio_status_t st;
        if(cos_audio_status(&st)==0 && audio_rate>0){
            uint64_t pending_frames=st.pending_samples/2u;
            uint64_t played=audio_written_frames>pending_frames?audio_written_frames-pending_frames:0;
            uint64_t aclk=played*1000ULL/(uint64_t)audio_rate;
            /* If the audio device stops consuming (no/slow backend, driver
             * stall) its clock freezes and video would freeze with it.
             * Track when the clock last moved and, after 750 ms without
             * progress, let it keep advancing on the wall clock from the
             * last known audio position. */
            static uint64_t last_aclk=(uint64_t)-1, last_move_wall;
            uint64_t wall=cos_time_ms();
            if(aclk!=last_aclk){last_aclk=aclk;last_move_wall=wall;}
            else if(wall-last_move_wall>750) return aclk+(wall-last_move_wall-750);
            return aclk;
        }
    }
    return cos_time_ms()>=playback_start_ms?cos_time_ms()-playback_start_ms:0;
}

/* Set by h264_feed_annexb() from edge264_decode_NAL()'s own per-NAL return
 * code - nonzero there means edge264 itself could not handle something in
 * the bitstream (see edge264_headers.c's parse_seq_parameter_set: ENOTSUP
 * for non-4:2:0 chroma, >8-bit depth, etc). Checked only right after the
 * initial avcC config-NAL feed (SPS/PPS) in load_path() below, where it
 * reflects a real stream-capability mismatch worth stopping for; a later
 * slice-NAL error during playback is left to edge264's own frame-level
 * recovery rather than aborting the whole video over one bad access unit. */
static int h264_fatal_error;
static const char *h264_error_name(int err){
    switch(err){
        case ENOTSUP: return "unsupported bit depth, chroma format, or feature";
        case EINVAL: return "invalid bitstream";
        case ENOMEM: return "out of memory";
        default: return "decoder error";
    }
}
/* With decoder threads edge264 reads each slice from its NAL buffer after decode_NAL() has returned, and tells us
 * through the unref callback when it is done; the buffer therefore carries a reference count (one for the feeder
 * and one per NAL handed over) and is freed by whoever drops the last reference - possibly a worker. */
static void h264_nal_unref(int ret,void *arg){(void)ret;volatile int *r=(volatile int*)arg;if(__atomic_sub_fetch(r,1,__ATOMIC_ACQ_REL)==0)free((void*)r);}
static int h264_out_count;
#ifndef ENOBUFS
#define ENOBUFS 105
#endif
static bool h264_take_frame_adapter(Edge264Frame *fr,uint64_t *pts,uint64_t *dur,bool *stored);
static bool (*h264_take_frame_cb)(Edge264Frame *fr,uint64_t *pts,uint64_t *dur,bool *stored);
static void h264_feed_annexb_ref(const uint8_t *buf,size_t len,volatile int *refs){
    if(!h264_dec||!buf)return;size_t p=0;
    while(p+4<=len){while(p+3<len && !(buf[p]==0&&buf[p+1]==0&&buf[p+2]==1) && !(p+4<=len&&buf[p]==0&&buf[p+1]==0&&buf[p+2]==0&&buf[p+3]==1))p++;
        if(p+3>=len)break;size_t start=p+(buf[p+2]==1?3:4),end=start;
        while(end+3<len && !(buf[end]==0&&buf[end+1]==0&&buf[end+2]==1) && !(end+4<=len&&buf[end]==0&&buf[end+1]==0&&buf[end+2]==0&&buf[end+3]==1))end++;
        if(end+3>=len)end=len;
        if(refs)__atomic_add_fetch(refs,1,__ATOMIC_ACQ_REL);
        int rc=edge264_decode_NAL(h264_dec,buf+start,buf+end,refs?h264_nal_unref:NULL,(void*)refs);
        /* ENOBUFS: decoding runs ahead on other cores and edge264 needs a finished picture taken before it can
         * accept more. Hand finished pictures to the display (or wait for the cores) and offer the NAL again. */
        for(int tries=0;rc==ENOBUFS&&tries<20000;tries++){
            Edge264Frame ofr;uint64_t op=pts_last_ms,od=0;bool ost=false;
            if(h264_take_frame_cb&&h264_take_frame_cb(&ofr,&op,&od,&ost)){if(ost){frame_output_pos++;video_clock_valid=true;}}
            else cos_yield();
            rc=edge264_decode_NAL(h264_dec,buf+start,buf+end,refs?h264_nal_unref:NULL,(void*)refs);
        }
        if(rc&&refs)h264_nal_unref(rc,(void*)refs);            /* refused: the callback will not come */
        if(rc && rc!=ENODATA)h264_fatal_error=rc;
        p=end;
    }
}
static void h264_feed_annexb(const uint8_t *buf,size_t len){h264_feed_annexb_ref(buf,len,NULL);}
static bool parse_avcc(const uint8_t *box,size_t len){
    if(len<7||box[0]!=1)return false;
    h264_profile_idc=box[1]; h264_level_idc=box[3];
    h264_nal_length_size=(uint8_t)((box[4]&3)+1);if(h264_nal_length_size==3)h264_nal_length_size=4;
    size_t p=5;unsigned ns=box[p++]&31;for(unsigned i=0;i<ns;i++){if(p+2>len)return false;size_t n=((size_t)box[p]<<8)|box[p+1];p+=2;if(p+n>len)return false;if(!h264_cfg_append(box+p,n))return false;p+=n;}
    if(p>=len)return false;unsigned np=box[p++];for(unsigned i=0;i<np;i++){if(p+2>len)return false;size_t n=((size_t)box[p]<<8)|box[p+1];p+=2;if(p+n>len)return false;if(!h264_cfg_append(box+p,n))return false;p+=n;}return h264_cfg_len>0;
}

/* Copy an HEVC hvcC configuration record (box payload) for the decoder. */
static void parse_hvcc_payload(const uint8_t *p,size_t n){
    if(hevc_cfg||n<23)return;
    hevc_cfg=malloc(n);if(hevc_cfg){memcpy(hevc_cfg,p,n);hevc_cfg_len=n;}
}
/* Extract the AudioSpecificConfig (DecoderSpecificInfo, tag 0x05) from an
 * mp4a sample entry (e, size es). Descriptor lengths are MPEG-4 "expandable"
 * (7 bits per byte, MSB = more), which many muxers emit as 80 80 80 NN. */
static void parse_mp4a_esds(const uint8_t *e,uint32_t es){
    if(aac_cfg)return;
    for(uint32_t z=28;z+2<es&&z<256;z++)if(e[z]==5){
        uint32_t l=0,q=z+1,nb=0;
        while(q<es&&nb<4){uint8_t bb=e[q++];nb++;l=(l<<7)|(bb&0x7f);if(!(bb&0x80))break;}
        if(l&&l<64&&q+l<=es){aac_cfg=malloc(l);if(aac_cfg){memcpy(aac_cfg,e+q,l);aac_cfg_len=l;}break;}
    }
}
static void parse_box(uint64_t pos,uint64_t end,track_t *t,int depth){
    if(depth>12) return;
    while(pos+8<=end && pos+8<=file_size){
        uint64_t sz=be32(file_data+pos), head=8;
        if(sz==1){if(pos+16>end) return; sz=be64(file_data+pos+8); head=16;}
        else if(sz==0) sz=end-pos;
        if(sz<head || !in_file(pos,sz) || pos+sz>end) return;
        const uint8_t *b=file_data+pos+head; uint64_t n=sz-head;
        if(typ(file_data+pos+4,"hdlr") && n>=12){
            if(typ(b+8,"vide")) t->kind=VIDEO; else if(typ(b+8,"soun")) t->kind=AUDIO;
        } else if(typ(file_data+pos+4,"mdhd") && n>=16){
            uint8_t ver=b[0]; uint64_t q=ver?20:12; if(n>=q+4)t->timescale=be32(b+q);
        } else if(typ(file_data+pos+4,"tkhd") && n>=16){
            uint8_t ver=b[0]; uint64_t q=ver?20:12; if(n>=q+4)t->track_id=be32(b+q);
        } else if(typ(file_data+pos+4,"elst") && n>=8){
            uint8_t ver=b[0]; uint32_t count=be32(b+4);
            /* Walk the edit list: leading entries with media_time == -1 are "empty edits" (a
             * presentation delay of segment_duration, in movie timescale); the first real entry's
             * media_time is where the track's media starts. Its own segment_duration is how long
             * that segment plays - NOT a delay - so it must not be added to the timestamps. */
            uint64_t esz=ver?20:12; uint64_t q=8; uint64_t empty=0; int64_t mtime=0; bool have_real=false;
            for(uint32_t i=0;i<count && q+esz<=n;i++,q+=esz){
                uint64_t dur=ver?be64(b+q):be32(b+q); int64_t mt=ver?(int64_t)be64(b+q+8):(int64_t)(int32_t)be32(b+q+4);
                if(mt==-1){ if(!have_real) empty+=dur; }
                else { mtime=mt; have_real=true; break; }
            }
            if(count){ t->edit_empty_duration=empty; t->edit_media_time=have_real?mtime:0; t->edit_valid=true; }
        } else if(typ(file_data+pos+4,"stsd") && n>=8){
            uint32_t count=be32(b+4); if(count && n>=16){
                const uint8_t *e=b+8; uint32_t es=be32(e); if(es>=8 && es<=n-8){
                    if(typ(e+4,"jpeg")) t->codec=1;
                    else if(typ(e+4,"avc1") || typ(e+4,"avc3")){t->codec=CODEC_H264;/* VisualSampleEntry header is 86 bytes before child boxes such as avcC. */for(uint32_t z=86;z+8<=es;){uint32_t bs=be32(e+z);if(bs<8||z+bs>es)break;if(typ(e+z+4,"avcC")){(void)parse_avcc(e+z+8,bs-8);break;}z+=bs;}}
                    else if(typ(e+4,".mp3") || typ(e+4,"mp3 ")) t->codec=CODEC_MP3;
                    else if(typ(e+4,"mp4a")){t->codec=CODEC_AAC;parse_mp4a_esds(e,es);}
                    else if(typ(e+4,"hvc1") || typ(e+4,"hev1")){t->codec=CODEC_HEVC;for(uint32_t z=86;z+8<=es;){uint32_t bs=be32(e+z);if(bs<8||z+bs>es)break;if(typ(e+z+4,"hvcC")){parse_hvcc_payload(e+z+8,bs-8);break;}z+=bs;}}
                    else if(typ(e+4,"av01")) t->codec=CODEC_AV1;
                    else if(typ(e+4,"vp09")) t->codec=CODEC_VP9;
                    if((t->codec==CODEC_MP3||t->codec==CODEC_AAC) && es>=28){audio_channels=(e[14]<<8)|e[15]; if(!audio_channels)audio_channels=2;}
                }
            }
        } else if(typ(file_data+pos+4,"stsz") && n>=12){
            uint32_t fixed=be32(b+4), count=be32(b+8); if(count>MAX_SAMPLES)count=MAX_SAMPLES;
            t->n=(int)count; for(uint32_t i=0;i<count;i++) t->s[i].size=fixed?fixed:be32(b+12+i*4);
        } else if(typ(file_data+pos+4,"stsc") && n>=8){
            uint32_t count=be32(b+4);if(count>MAX_TABLE_ENTRIES)count=MAX_TABLE_ENTRIES;t->stsc_count=count;
            for(uint32_t i=0;i<count;i++){uint64_t q=8+i*12;if(q+12>n+head)break;t->stsc[i].first_chunk=be32(b+q);t->stsc[i].samples_per_chunk=be32(b+q+4);t->stsc[i].sample_desc=be32(b+q+8);}
        } else if(typ(file_data+pos+4,"stco") && n>=8){
            uint32_t count=be32(b+4);if(count>MAX_SAMPLES)count=MAX_SAMPLES;t->chunk_count=count;
            for(uint32_t i=0;i<count;i++)t->chunk_offsets[i]=be32(b+8+i*4);
        } else if(typ(file_data+pos+4,"co64") && n>=8){
            uint32_t count=be32(b+4);if(count>MAX_SAMPLES)count=MAX_SAMPLES;t->chunk_count=count;
            for(uint32_t i=0;i<count;i++)t->chunk_offsets[i]=be64(b+8+i*8);
        } else if(typ(file_data+pos+4,"stts") && n>=8){
            uint32_t count=be32(b+4); if(count>MAX_TABLE_ENTRIES)count=MAX_TABLE_ENTRIES; t->stts_count=count;
            for(uint32_t i=0;i<count;i++){uint64_t q=8+i*8;if(q+8>n+head)break;t->stts[i].count=be32(b+q);t->stts[i].delta=be32(b+q+4);}
        } else if(typ(file_data+pos+4,"stss") && n>=8){
            uint32_t count=be32(b+4); if(count>MAX_SAMPLES) count=MAX_SAMPLES;
            t->sync_count=count;
            for(uint32_t i=0;i<count;i++){ uint64_t q=8+i*4; if(q+4>n+head) break; t->sync_samples[i]=be32(b+q); }
            mark_sync_samples(t);
        } else if(typ(file_data+pos+4,"ctts") && n>=8){
            uint32_t count=be32(b+4); if(count>MAX_TABLE_ENTRIES)count=MAX_TABLE_ENTRIES; t->ctts_count=count;
            bool v1=(b[0]!=0);
            for(uint32_t i=0;i<count;i++){uint64_t q=8+i*8;if(q+8>n+head)break;t->ctts[i].count=be32(b+q);uint32_t raw=be32(b+q+4);t->ctts[i].offset=v1?(int64_t)(int32_t)raw:(int64_t)raw;}
        }
        if(typ(file_data+pos+4,"trak")){ if(ntracks<8){track_t *nt=&tracks[ntracks++];memset(nt,0,sizeof(*nt));nt->timescale=1000;parse_box(pos+head,pos+sz,nt,depth+1);expand_sample_offsets(nt);} }
        else if(typ(file_data+pos+4,"moov")||typ(file_data+pos+4,"mdia")||typ(file_data+pos+4,"minf")||typ(file_data+pos+4,"stbl")||typ(file_data+pos+4,"edts")) parse_box(pos+head,pos+sz,t,depth+1);
        pos+=sz;
    }
}
static void salvage_track(track_t *t, uint64_t start, uint64_t end){
    /* Some generated files place valid leaf boxes behind optional sample-entry
     * fields. Scan the bounded trak region independently so a malformed
     * recursive walk cannot hide an otherwise standard avc1 track. */
    for(uint64_t x=start; x+8<=end; x++){
        if(typ(file_data+x+4,"hdlr") && x+24<=end){
            if(typ(file_data+x+16,"vide")) t->kind=VIDEO;
            if(typ(file_data+x+16,"soun")) t->kind=AUDIO;
        }
        if(typ(file_data+x+4,"avc1") || typ(file_data+x+4,"avc3")) t->codec=CODEC_H264;
        if(typ(file_data+x+4,"mp4a")){ t->codec=CODEC_AAC; uint32_t esz=be32(file_data+x); if(esz>=36 && x+esz<=end) parse_mp4a_esds(file_data+x,esz); }
        if(typ(file_data+x+4,"hvc1") || typ(file_data+x+4,"hev1")) t->codec=CODEC_HEVC;
        if(typ(file_data+x+4,"av01")) t->codec=CODEC_AV1;
        if(typ(file_data+x+4,"vp09")) t->codec=CODEC_VP9;
        if(typ(file_data+x+4,"hvcC")){
            uint32_t bs=be32(file_data+x); if(bs>=8 && x+bs<=end) parse_hvcc_payload(file_data+x+8,bs-8);
        }
        if(typ(file_data+x+4,"avcC")){
            uint32_t bs=be32(file_data+x); if(bs>=8 && x+bs<=end) (void)parse_avcc(file_data+x+8,bs-8);
        }
        if(typ(file_data+x+4,"stsz") && x+20<=end){
            uint32_t bs=be32(file_data+x), fixed=be32(file_data+x+12), count=be32(file_data+x+16);
            if(bs>=20 && x+bs<=end){if(count>MAX_SAMPLES)count=MAX_SAMPLES;t->n=(int)count;for(uint32_t i=0;i<count;i++)t->s[i].size=fixed?fixed:be32(file_data+x+20+i*4);}
        }
        if(typ(file_data+x+4,"stsc") && x+16<=end){
            uint32_t bs=be32(file_data+x),count=be32(file_data+x+12);if(bs>=16&&x+bs<=end){if(count>MAX_TABLE_ENTRIES)count=MAX_TABLE_ENTRIES;t->stsc_count=count;for(uint32_t i=0;i<count;i++){uint64_t q=x+16+i*12;if(q+12>x+bs)break;t->stsc[i].first_chunk=be32(file_data+q);t->stsc[i].samples_per_chunk=be32(file_data+q+4);t->stsc[i].sample_desc=be32(file_data+q+8);}}
        }
        if(typ(file_data+x+4,"stco") && x+16<=end){
            uint32_t bs=be32(file_data+x),count=be32(file_data+x+12);if(bs>=16&&x+bs<=end){if(count>MAX_SAMPLES)count=MAX_SAMPLES;t->chunk_count=count;for(uint32_t i=0;i<count;i++)t->chunk_offsets[i]=be32(file_data+x+16+i*4);}
        }
        if(typ(file_data+x+4,"stts") && x+16<=end){
            uint32_t bs=be32(file_data+x),count=be32(file_data+x+12);if(bs>=16&&x+bs<=end){if(count>MAX_TABLE_ENTRIES)count=MAX_TABLE_ENTRIES;t->stts_count=count;for(uint32_t i=0;i<count;i++){uint64_t q=x+16+i*8;if(q+8>x+bs)break;t->stts[i].count=be32(file_data+q);t->stts[i].delta=be32(file_data+q+4);}}
        }
        if(typ(file_data+x+4,"stss") && x+16<=end){
            uint32_t bs=be32(file_data+x),count=be32(file_data+x+12);
            if(bs>=16&&x+bs<=end){ if(count>MAX_SAMPLES) count=MAX_SAMPLES; t->sync_count=count; for(uint32_t i=0;i<count;i++){uint64_t q=x+16+i*4;if(q+4>x+bs)break;t->sync_samples[i]=be32(file_data+q);} mark_sync_samples(t); }
        }
        if(typ(file_data+x+4,"ctts") && x+16<=end){
            uint32_t bs=be32(file_data+x),count=be32(file_data+x+12);if(bs>=16&&x+bs<=end){if(count>MAX_TABLE_ENTRIES)count=MAX_TABLE_ENTRIES;t->ctts_count=count;bool v1=file_data[x+8]!=0;for(uint32_t i=0;i<count;i++){uint64_t q=x+16+i*8;if(q+8>x+bs)break;t->ctts[i].count=be32(file_data+q);uint32_t raw=be32(file_data+q+4);t->ctts[i].offset=v1?(int64_t)(int32_t)raw:(int64_t)raw;}}
        }
    }
    expand_sample_offsets(t);
}


static track_t *track_by_id(uint32_t id){
    for(int i=0;i<ntracks;i++) if(tracks[i].track_id==id) return &tracks[i];
    return NULL;
}
static void parse_trex_box(const uint8_t *b, uint64_t n){
    if(n<24) return;
    track_t *t=track_by_id(be32(b+4));
    if(!t) return;
    t->default_sample_duration=be32(b+12);
    t->default_sample_size=be32(b+16);
    t->default_sample_flags=be32(b+20);
}
static void scan_trex(uint64_t pos,uint64_t end,int depth){
    if(depth>8) return;
    while(pos+8<=end && pos+8<=file_size){
        uint64_t sz=be32(file_data+pos), head=8;
        if(sz==1){if(!in_file(pos,16))return;sz=be64(file_data+pos+8);head=16;}
        else if(sz==0) sz=end-pos;
        if(sz<head||!in_file(pos,sz)||pos+sz>end)return;
        if(typ(file_data+pos+4,"trex")) parse_trex_box(file_data+pos+head,sz-head);
        else if(typ(file_data+pos+4,"mvex")||typ(file_data+pos+4,"moov")||typ(file_data+pos+4,"trak")||typ(file_data+pos+4,"mdia")||typ(file_data+pos+4,"minf"))
            scan_trex(pos+head,pos+sz,depth+1);
        pos+=sz;
    }
}
static void parse_traf_fragment(uint64_t traf_pos,uint64_t traf_end,uint64_t moof_pos){
    uint32_t track_id=0, tfhd_flags=0, trun_flags=0, sample_count=0;
    uint64_t base_offset=moof_pos, decode_time=0;
    bool have_tfdt=false, have_base=false;
    uint32_t def_dur=0, def_size=0, def_flags=0;
    uint64_t trun_pos=0, trun_end=0;
    int depth=0;
    for(uint64_t q=traf_pos+8;q+8<=traf_end;){
        uint64_t sz=be32(file_data+q), head=8;
        if(sz==1){if(!in_file(q,16))return;sz=be64(file_data+q+8);head=16;}
        else if(sz==0)sz=traf_end-q;
        if(sz<head||!in_file(q,sz)||q+sz>traf_end)return;
        const uint8_t *b=file_data+q+head; uint64_t n=sz-head;
        if(typ(file_data+q+4,"tfhd")&&n>=8){
            tfhd_flags=((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3]; track_id=be32(b+4); uint64_t o=8;
            if(tfhd_flags&1u){if(n<o+8)return;base_offset=be64(b+o);o+=8;have_base=true;}
            if(tfhd_flags&2u){if(n<o+4)return;o+=4;}
            if(tfhd_flags&8u){if(n<o+4)return;def_dur=be32(b+o);o+=4;}
            if(tfhd_flags&16u){if(n<o+4)return;def_size=be32(b+o);o+=4;}
            if(tfhd_flags&32u){if(n<o+4)return;def_flags=be32(b+o);o+=4;}
        } else if(typ(file_data+q+4,"tfdt")&&n>=8){
            uint8_t ver=b[0]; if(ver==1){if(n<12)return;decode_time=be64(b+4);}else decode_time=be32(b+4); have_tfdt=true;
        } else if(typ(file_data+q+4,"trun")&&n>=8){trun_pos=q+head;trun_end=q+sz;trun_flags=((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];sample_count=be32(b+4);}
        q+=sz;
    }
    track_t *t=track_by_id(track_id); if(!t||!sample_count||!trun_pos||t->n>=MAX_SAMPLES)return;
    if(!def_dur)def_dur=t->default_sample_duration; if(!def_size)def_size=t->default_sample_size; if(!def_flags)def_flags=t->default_sample_flags;
    const uint8_t *b=file_data+trun_pos; uint64_t n=trun_end-trun_pos, o=8;
    int32_t data_offset=0; uint32_t first_flags=0;
    if(trun_flags&1u){if(n<o+4)return;data_offset=(int32_t)be32(b+o);o+=4;}
    if(trun_flags&4u){if(n<o+4)return;first_flags=be32(b+o);o+=4;}
    uint64_t data_pos=have_base?base_offset:moof_pos; data_pos=(data_offset>=0)?data_pos+(uint32_t)data_offset:(data_pos>(uint32_t)(-data_offset)?data_pos-(uint32_t)(-data_offset):0);
    uint64_t dts=have_tfdt?decode_time:0;
    for(uint32_t i=0;i<sample_count&&t->n<MAX_SAMPLES;i++){
        uint32_t dur=def_dur,size=def_size,flags=(i==0&&first_flags)?first_flags:def_flags; int64_t cto=0;
        if(trun_flags&0x100u){if(n<o+4)return;dur=be32(b+o);o+=4;}
        if(trun_flags&0x200u){if(n<o+4)return;size=be32(b+o);o+=4;}
        if(trun_flags&0x400u){if(n<o+4)return;flags=be32(b+o);o+=4;}
        if(trun_flags&0x800u){if(n<o+4)return;uint32_t raw=be32(b+o);cto=(b[0]==1)?(int64_t)(int32_t)raw:(int64_t)raw;o+=4;}
        if(!size||!in_file(data_pos,size))return;
        sample_t *dst=&t->s[t->n]; memset(dst,0,sizeof(*dst)); dst->off=data_pos;dst->size=size;dst->dts=dts;dst->dur=dur;dst->pts_offset=cto;dst->flags=flags;dst->sync=(flags&0x10000u)?0:1;dst->decode_index=(uint32_t)t->n;dst->display_index=(uint32_t)t->n;
        t->n++; data_pos+=size; dts+=dur;
    }
}
static void parse_fragments(uint64_t pos,uint64_t end){
    while(pos+8<=end&&pos+8<=file_size){
        uint64_t sz=be32(file_data+pos),head=8;if(sz==1){if(!in_file(pos,16))return;sz=be64(file_data+pos+8);head=16;}else if(sz==0)sz=end-pos;
        if(sz<head||!in_file(pos,sz)||pos+sz>end)return;
        if(typ(file_data+pos+4,"moof")){
            uint64_t q=pos+head;
            while(q+8<=pos+sz){uint64_t bs=be32(file_data+q),bh=8;if(bs==1){if(!in_file(q,16))break;bs=be64(file_data+q+8);bh=16;}else if(bs==0)bs=pos+sz-q;if(bs<bh||!in_file(q,bs)||q+bs>pos+sz)break;if(typ(file_data+q+4,"traf"))parse_traf_fragment(q,q+bs,pos);q+=bs;}
        }
        pos+=sz;
    }
    for(int i=0;i<ntracks;i++) if(tracks[i].n) assign_display_order(&tracks[i]);
}


/* ---- HEVC decoding on other cores -------------------------------------------
 * The decoder runs as compute jobs on application processors (SYS_PAR_*): up to HW_MAXT workers, each with its
 * own arena, decode different frames at the same time (frame threading, see cos_hevc_entry.c). Packet `seq` -
 * every sample, then a run of drain calls - is decoded by worker seq % N and its result is published in a small
 * window. The main thread takes results strictly in seq order at exactly the points where it used to call
 * cos_hevc_decode()/cos_hevc_drain(), so pacing, timestamps and the display path are unchanged, and a result's
 * frame is displayed straight from the decoder's own buffer (no copy). Everything a worker touches is mapped
 * before it starts: it cannot make system calls or take page faults. */
enum { HW_SAMPLE_KIND=0, HW_DRAIN_KIND=1 };
#define HW_MAXT 3
#define HW_RES 16
#define HW_DRAIN_PKTS 24
typedef struct { volatile int64_t ready; int rc; void *frame; } hw_res_t;
typedef struct {
    int codec;                                   /* CODEC_HEVC or CODEC_VP9 */
    cos_hevc_group_t *group; cos_vp9_group_t *vgroup; int nthreads;
    /* VP9: one packet per VP9 frame; a sample can hold several (superframe) */
    uint64_t *pk_off; uint32_t *pk_len; int *smp_first, *smp_cnt;
    hw_res_t res[HW_RES];
    volatile int64_t consumed;                   /* results below this seq have been released */
    volatile uint32_t quit, done, open_failed;
    int64_t total; int nsamples;
    const uint8_t *file; const sample_t *samples; const uint8_t *cfg; uint32_t cfg_len;
    uint64_t tsc_decode[HW_MAXT], tsc_wait[HW_MAXT];
} hw_shared_t;
typedef struct { hw_shared_t *sh; int idx; } hw_warg_t;
static hw_shared_t *hw; static hw_warg_t *hw_args; static cos_arena_t *hw_arena[HW_MAXT]; static int hw_job[HW_MAXT];
static bool hevc_async; static int hw_codec; static int64_t hw_next_seq; static void *hw_prev_frame; static bool hw_have_prev;
static int hw_dim_w, hw_dim_h, hw_dim_d;
static void hw_worker(void *arg){
    hw_warg_t *wa=(hw_warg_t*)arg; hw_shared_t *sh=wa->sh; int idx=wa->idx,T=sh->nthreads;
    const bool vp9=sh->codec==CODEC_VP9;
    void *dec=vp9?cos_vp9_mt_open(sh->vgroup,idx):cos_hevc_mt_open(sh->group,idx,sh->cfg,sh->cfg_len);
    if(!dec){sh->open_failed=1;if(vp9)cos_vp9_group_abort(sh->vgroup);else cos_hevc_group_abort(sh->group);return;}
    for(int64_t seq=idx;seq<sh->total&&!sh->quit;seq+=T){
        uint64_t t0=__builtin_ia32_rdtsc();
        while(seq-sh->consumed>=HW_RES){if(sh->quit)goto out;if(!cos_idle_work())__builtin_ia32_pause();}
        uint64_t t1=__builtin_ia32_rdtsc();
        const uint8_t *d=NULL;uint32_t sz=0;void *fr=NULL;int rc;
        if(vp9){rc=cos_vp9_mt_decode(dec,seq,sh->file+sh->pk_off[seq],sh->pk_len[seq],&fr);}
        else{
            if(seq<sh->nsamples){d=sh->file+sh->samples[seq].off;sz=(uint32_t)sh->samples[seq].size;}
            rc=cos_hevc_mt_decode(dec,seq,d,sz,&fr);
        }
        sh->tsc_decode[idx]+=__builtin_ia32_rdtsc()-t1;sh->tsc_wait[idx]+=t1-t0;
        hw_res_t *r=&sh->res[seq%HW_RES];r->rc=rc;r->frame=fr;
        __atomic_store_n(&r->ready,seq+1,__ATOMIC_RELEASE);
    }
out:
    /* every context may still be read by a successor copying its state: close only when all are finished */
    __atomic_fetch_add(&sh->done,1,__ATOMIC_ACQ_REL);
    while(sh->done<(uint32_t)T&&!sh->quit){if(!cos_idle_work())__builtin_ia32_pause();}
    if(vp9)cos_vp9_close(dec);else cos_hevc_close(dec);
}
static void hw_stop(void){
    if(hw){
        hw->quit=1;if(hw->group)cos_hevc_group_abort(hw->group);if(hw->vgroup)cos_vp9_group_abort(hw->vgroup);
        for(int i=0;i<hw->nthreads;i++)if(hw_job[i]>=0){
            for(int k=0;k<3000;k++){if(cos_par_poll(hw_job[i],NULL)>=COS_PAR_DONE)break;cos_sleep_ms(1);}
            cos_par_stop(hw_job[i]);
            for(int k=0;k<500;k++){if(cos_par_poll(hw_job[i],NULL)>=COS_PAR_DONE)break;cos_sleep_ms(1);}
            cos_par_free(hw_job[i]);hw_job[i]=-1;
        }
        /* release results nobody collected */
        for(int i=0;i<HW_RES;i++)if(hw->res[i].frame&&hw->res[i].ready>hw->consumed){if(hw->codec==CODEC_VP9)cos_vp9_release(hw->res[i].frame);else cos_hevc_release(hw->res[i].frame);hw->res[i].frame=NULL;}
        if(hw->group)cos_hevc_group_destroy(hw->group);
        if(hw->vgroup)cos_vp9_group_destroy(hw->vgroup);
        free(hw->pk_off);free(hw->pk_len);free(hw->smp_first);free(hw->smp_cnt);
        free(hw);hw=NULL;
    }
    if(hw_prev_frame){if(hw_codec==CODEC_VP9)cos_vp9_release(hw_prev_frame);else cos_hevc_release(hw_prev_frame);hw_prev_frame=NULL;}
    free(hw_args);hw_args=NULL;
    for(int i=0;i<HW_MAXT;i++){hw_job[i]=-1;if(hw_arena[i]){cos_arena_destroy(hw_arena[i]);hw_arena[i]=NULL;}}
    hevc_async=false;hw_next_seq=0;hw_have_prev=false;
}
static bool hw_start(const track_t *t,int w,int h,int depth,int codec){
    int cpus=cos_par_cpus();if(cpus<1||w<=0||h<=0)return false;
    hw_codec=codec;
    hw_dim_w=w;hw_dim_h=h;hw_dim_d=depth;
    int bps=depth>8?2:1;
    size_t frame=((size_t)w*h*3/2*bps+4096);
    int T=cpus<HW_MAXT?cpus:HW_MAXT;
    /* memory budget: each worker's arena holds its share of the live pictures (DPB + results) plus codec state */
    while(T>1&&(frame*((HW_RES+16)/T+4)+(48u<<20))*(size_t)T>(size_t)640<<20)T--;
    size_t heap=frame*((HW_RES+16)/T+4)+(48u<<20);
    for(int i=0;i<HW_MAXT;i++)hw_job[i]=-1;
    hw=calloc(1,sizeof *hw);if(!hw)return false;
    hw_args=calloc(HW_MAXT,sizeof *hw_args);if(!hw_args){free(hw);hw=NULL;return false;}
    hw->codec=codec;
    hw->nthreads=T;hw->nsamples=t->n;hw->total=(int64_t)t->n+HW_DRAIN_PKTS;hw->file=file_data;hw->samples=t->s;hw->cfg=hevc_cfg;hw->cfg_len=(uint32_t)hevc_cfg_len;
    if(codec==CODEC_VP9){
        /* number the VP9 frames: one packet each, in decode order, a sample's packets consecutively */
        hw->smp_first=malloc((size_t)(t->n>0?t->n:1)*sizeof(int));hw->smp_cnt=malloc((size_t)(t->n>0?t->n:1)*sizeof(int));
        size_t cap=(size_t)t->n*2+16,np=0;hw->pk_off=malloc(cap*sizeof(uint64_t));hw->pk_len=malloc(cap*sizeof(uint32_t));
        if(!hw->smp_first||!hw->smp_cnt||!hw->pk_off||!hw->pk_len){hw_stop();return false;}
        for(int i=0;i<t->n;i++){
            uint32_t offs[8],lens[8];int k=cos_vp9_split(file_data+t->s[i].off,(uint32_t)t->s[i].size,offs,lens);
            if(k<0)k=0;
            if(np+(size_t)k>cap){cap=(np+(size_t)k)*2;hw->pk_off=realloc(hw->pk_off,cap*sizeof(uint64_t));hw->pk_len=realloc(hw->pk_len,cap*sizeof(uint32_t));if(!hw->pk_off||!hw->pk_len){hw_stop();return false;}}
            hw->smp_first[i]=(int)np;hw->smp_cnt[i]=k;
            for(int j=0;j<k;j++){hw->pk_off[np]=t->s[i].off+offs[j];hw->pk_len[np]=lens[j];np++;}
        }
        hw->total=(int64_t)np;
        hw->vgroup=cos_vp9_group_create(T);if(!hw->vgroup){hw_stop();return false;}
    }else{
        hw->group=cos_hevc_group_create(T);if(!hw->group){hw_stop();return false;}
    }
    for(int i=0;i<T;i++){hw_arena[i]=cos_arena_create(heap,2u<<20);if(!hw_arena[i]){hw_stop();return false;}}
    for(int i=0;i<T;i++){
        void *stk;size_t ss;cos_arena_stack(hw_arena[i],&stk,&ss);
        hw_args[i].sh=hw;hw_args[i].idx=i;
        hw_job[i]=cos_par_start(hw_worker,&hw_args[i],stk,ss);
        if(hw_job[i]<0){hw_stop();return false;}
    }
    hevc_async=true;hw_next_seq=0;hw_have_prev=false;
    return true;
}
#if COS_VALIDATION_FRAME_CRC
static bool hw_reported;
static void hw_report(void){
    if(hw_reported||!hw||hw_next_seq<hw->nsamples)return;hw_reported=true;
    for(int i=0;i<hw->nthreads;i++){char db[160];snprintf(db,sizeof db,"[HWTSC] worker=%d decode=%llu wait=%llu (Mcycles)\n",i,(unsigned long long)(hw->tsc_decode[i]>>20),(unsigned long long)(hw->tsc_wait[i]>>20));cos_puts(db);}
}
#else
static void hw_report(void){}
#endif
static unsigned hw_spin;
static int hw_pop(int kind,int sample_index,cos_hevc_frame_t *out){
    hw_report();
    memset(out,0,sizeof *out);
    if(hw_have_prev){if(hw_prev_frame){cos_hevc_release(hw_prev_frame);hw_prev_frame=NULL;}hw->consumed=hw_next_seq;hw_have_prev=false;}
    if(kind==HW_SAMPLE_KIND&&sample_index==0&&hw_next_seq>0){
        /* playback restarted from the beginning: decode the stream again */
        track_t *t=&tracks[video_track];int w=hw_dim_w,h=hw_dim_h,d=hw_dim_d;
        hw_stop();if(!hw_start(t,w,h,d,CODEC_HEVC))return -1;
    }
    int64_t seq=hw_next_seq;
    if(seq>=hw->total)return 0;
    hw_res_t *r=&hw->res[seq%HW_RES];
    for(;;){
        if(__atomic_load_n(&r->ready,__ATOMIC_ACQUIRE)==seq+1)break;
        if(hw->open_failed)return -1;
        bool dead=false;
        if(((hw_spin+1)&0x3FFu)==0)for(int i=0;i<hw->nthreads;i++){
            int st=cos_par_poll(hw_job[i],NULL);
            if(st>COS_PAR_DONE){
                cos_par_fault_t f;cos_par_poll(hw_job[i],&f);
                snprintf(status_buf,sizeof status_buf,"HEVC worker %d stopped: vec=%llu rip=%llx cr2=%llx",i,(unsigned long long)f.vec,(unsigned long long)f.rip,(unsigned long long)f.cr2);
                cos_puts(status_buf);cos_puts("\n");dead=true;
            }
        }
        if(dead)return -1;
        if(!cos_idle_work()){__builtin_ia32_pause();if(((++hw_spin)&0x3FFu)==0)cos_sleep_ms(1);}
    }
    hw_next_seq=seq+1;hw_have_prev=true;
    int rc=r->rc;
    if(rc==1){
        hw_prev_frame=r->frame;r->frame=NULL;
        int d=cos_hevc_describe(hw_prev_frame,out);
        if(d!=1)rc=d<0?-1:d;
    }
    return rc;
}
/* VP9: the results of a sample's frames (usually one; a superframe holds a hidden frame and a visible one) are
 * taken together; the frame the sample shows is the one that decoded to a picture. */
static int vp9_hw_pop(int sample_index,cos_vp9_frame_t *out){
    hw_report();
    memset(out,0,sizeof *out);
    if(hw_have_prev){if(hw_prev_frame){cos_vp9_release(hw_prev_frame);hw_prev_frame=NULL;}hw->consumed=hw_next_seq;hw_have_prev=false;}
    if(sample_index==0&&hw_next_seq>0){
        track_t *t=&tracks[video_track];int w=hw_dim_w,h=hw_dim_h,d=hw_dim_d;
        hw_stop();if(!hw_start(t,w,h,d,CODEC_VP9))return -1;
    }
    if(sample_index<0||sample_index>=hw->nsamples)return 0;
    int first=hw->smp_first[sample_index],cnt=hw->smp_cnt[sample_index],rc=0;
    void *shown=NULL;
    for(int k=0;k<cnt;k++){
        int64_t seq=first+k;
        hw_res_t *r=&hw->res[seq%HW_RES];
        for(;;){
            if(__atomic_load_n(&r->ready,__ATOMIC_ACQUIRE)==seq+1)break;
            if(hw->open_failed){if(shown)cos_vp9_release(shown);return -1;}
            bool dead=false;
            if(((hw_spin+1)&0x3FFu)==0)for(int i=0;i<hw->nthreads;i++){
                int st=cos_par_poll(hw_job[i],NULL);
                if(st>COS_PAR_DONE){cos_par_fault_t f;cos_par_poll(hw_job[i],&f);snprintf(status_buf,sizeof status_buf,"VP9 worker %d stopped: vec=%llu rip=%llx cr2=%llx",i,(unsigned long long)f.vec,(unsigned long long)f.rip,(unsigned long long)f.cr2);cos_puts(status_buf);cos_puts("\n");dead=true;}
            }
            if(dead){if(shown)cos_vp9_release(shown);return -1;}
            if(!cos_idle_work()){__builtin_ia32_pause();if(((++hw_spin)&0x3FFu)==0)cos_sleep_ms(1);}
        }
        hw_next_seq=seq+1;
        if(r->rc<0)rc=-1;
        else if(r->rc==1){if(shown)cos_vp9_release(shown);shown=r->frame;r->frame=NULL;}
    }
    if(cnt==0)hw_next_seq=hw_next_seq; 
    hw_have_prev=true;
    if(rc<0){if(shown)cos_vp9_release(shown);return -1;}
    if(!shown)return 0;
    hw_prev_frame=shown;
    return cos_vp9_describe(shown,out)==1?1:0;
}
static bool parse_mp4(void){
    if(file_size<12 || !typ(file_data+4,"ftyp")){msg("Not an MP4 file");return false;}
    /* Locate moov and enumerate trak boxes explicitly. This avoids relying on
     * recursive traversal state while still parsing each track's mdia/stbl
     * hierarchy with parse_box(). It handles both fast-start and tail moov. */
    uint64_t p=0;
    while(p+8<=file_size){
        uint64_t s=be32(file_data+p); if(s==1){if(!in_file(p,16))break;s=be64(file_data+p+8);} else if(s==0)s=file_size-p;
        uint64_t head=(be32(file_data+p)==1)?16:8;
        if(s<head||!in_file(p,s))break;
        if(typ(file_data+p+4,"moov")){
            uint64_t q=p+head;
            while(q+8<=p+s){
                uint64_t bs=be32(file_data+q); if(bs==1){if(!in_file(q,16))break;bs=be64(file_data+q+8);} else if(bs==0)bs=(p+s)-q;
                uint64_t bh=(be32(file_data+q)==1)?16:8;
                if(bs<bh||!in_file(q,bs)||q+bs>p+s)break;
                if(typ(file_data+q+4,"mvhd")){ const uint8_t *mb=file_data+q+bh; uint64_t mn=bs-bh, mq=mb[0]?20:12; if(mn>=mq+4&&be32(mb+mq))movie_timescale=be32(mb+mq); }
                if(typ(file_data+q+4,"trak") && ntracks<8){
                    track_t *nt=&tracks[ntracks++]; memset(nt,0,sizeof(*nt)); nt->timescale=1000;
                    parse_box(q+bh,q+bs,nt,0); salvage_track(nt,q+bh,q+bs);
                }
                q+=bs;
            }
        }
        p+=s;
    }
    /* Fragmented files carry defaults in mvex/trex and samples in moof/trun.
     * Parse them after moov so track ids, timescales, and codec config are known. */
    scan_trex(0,file_size,0);
    parse_fragments(0,file_size);
    for(int i=0;i<ntracks;i++){
        track_t *t=&tracks[i]; if(t->kind==VIDEO&&(t->codec==CODEC_JPEG||t->codec==CODEC_H264||t->codec==CODEC_VP9||t->codec==CODEC_AV1||t->codec==CODEC_HEVC)&&t->n)video_track=i; if(t->kind==AUDIO&&(t->codec==CODEC_MP3||t->codec==CODEC_AAC)&&t->n)audio_track=i;
    }
    have_video=video_track>=0; have_audio=audio_track>=0;
#if COS_VALIDATION_FRAME_CRC
    for(int i=0;i<ntracks;i++){track_t *t=&tracks[i]; char tb[200]; snprintf(tb,sizeof tb,"[TRACK] %d kind=%d codec=%d n=%d timescale=%u movie_ts=%u edit=%d empty=%llu mtime=%lld pts0=%llu pts1=%llu pts2=%llu\n",i,t->kind,t->codec,t->n,(unsigned)t->timescale,(unsigned)movie_timescale,(int)t->edit_valid,(unsigned long long)t->edit_empty_duration,(long long)t->edit_media_time,(unsigned long long)sample_pts_ms(t,0),(unsigned long long)sample_pts_ms(t,1),(unsigned long long)sample_pts_ms(t,2)); cos_puts(tb);}
#endif
    if(!have_video){
        for(int i=0;i<ntracks;i++) if(tracks[i].kind==VIDEO && tracks[i].n){
            if(tracks[i].codec==CODEC_HEVC) msg("HEVC/H.265 track detected (unsupported track layout)");
            else if(tracks[i].codec==CODEC_AV1) msg("AV1 track detected (decoder initialization failed)");
            else if(tracks[i].codec==CODEC_VP9) msg("VP9 track detected (decoder initialization failed)");
            else msg("MP4 video track is not supported");
            break;
        }
        return false;
    }
    if(!have_audio)msg(tracks[video_track].codec==CODEC_H264?"H.264 video ready (no audio)":(tracks[video_track].codec==CODEC_VP9?"VP9 video ready (no audio)":(tracks[video_track].codec==CODEC_AV1?"AV1 video ready (no audio)":"Motion-JPEG video ready"))); else msg(tracks[video_track].codec==CODEC_H264?"H.264 video + audio ready":(tracks[video_track].codec==CODEC_VP9?"VP9 video + audio ready":(tracks[video_track].codec==CODEC_AV1?"AV1 video + audio ready":"Motion-JPEG video + audio ready")));
    if(video_track>=0 && tracks[video_track].codec==CODEC_H264){
        /* edge264 itself already parses the High/High10/High422/High444
         * SPS extensions correctly (see edge264_headers.c's
         * parse_seq_parameter_set) and only fails the genuinely
         * unsupported cases - non-4:2:0 chroma, >8-bit depth,
         * qpprime_y_zero_transform_bypass - via its own per-NAL return
         * code. There is therefore no reason to reject a profile number
         * before even trying: High profile 8-bit 4:2:0, by far the most
         * common H.264 encoding for web/phone/camera MP4s, decodes fine.
         * h264_feed_annexb() below surfaces that return code via
         * h264_fatal_error so a genuinely unsupported stream (10-bit,
         * 4:2:2/4:4:4) is still reported accurately instead of either
         * being silently fed garbage or rejected on profile number alone. */
        {int nt=cos_par_cpus();if(nt>3)nt=3;if(nt<2)nt=0;           /* measured (QEMU, 1080p): one worker was slower than none */
#ifdef COS_H264_THREADS
        nt=COS_H264_THREADS;
#endif
        cos_pthread_arena_heap=(size_t)(96u<<20);
        h264_dec=nt>0?edge264_alloc(nt,NULL,NULL,0,NULL,NULL,NULL):NULL;
        if(!h264_dec)h264_dec=edge264_alloc(0,NULL,NULL,0,NULL,NULL,NULL);
#if COS_VALIDATION_FRAME_CRC
        {char db[64];snprintf(db,sizeof db,"[H264] threads=%d dec=%p\n",nt,(void*)h264_dec);cos_puts(db);}
#endif
        }
        if(!h264_dec){msg("H.264 decoder allocation failed");return false;}
        h264_fatal_error=0;h264_take_frame_cb=h264_take_frame_adapter;
        if(h264_cfg_len)h264_feed_annexb(h264_cfg,h264_cfg_len);
        if(h264_fatal_error){
            char prof[128]; snprintf(prof,sizeof prof,"H.264 profile %u level %u not supported (%s)",(unsigned)h264_profile_idc,(unsigned)h264_level_idc,h264_error_name(h264_fatal_error));
            msg(prof); edge264_free(&h264_dec); have_video=false; return false;
        }
        { char prof[80]; snprintf(prof,sizeof prof,"H.264 profile %u level %u backend ready",(unsigned)h264_profile_idc,(unsigned)h264_level_idc); msg(prof); }
    }
    if(video_track>=0 && tracks[video_track].codec==CODEC_VP9){
        vp9_dec=cos_vp9_open();
        if(!vp9_dec){msg("VP9 decoder initialization failed");have_video=false;return false;}
        vp9_dec_open=true; msg("VP9 decoder ready (FFmpeg vp9.c, 8/10/12-bit)");
        {   /* decode on other cores when there are some: learn the picture size from the first sample, then hand over */
            track_t *vt=&tracks[video_track];cos_vp9_frame_t pf;
            if(vt->n>0&&cos_vp9_decode(vp9_dec,file_data+vt->s[0].off,(uint32_t)vt->s[0].size,&pf)==1){
                int pw=pf.width,ph=pf.height,pd=pf.bit_depth;
                cos_vp9_close(vp9_dec);vp9_dec=cos_vp9_open();
                if(vp9_dec&&hw_start(vt,pw,ph,pd,CODEC_VP9)){cos_vp9_close(vp9_dec);vp9_dec=NULL;}
            }
        }
    }
    if(video_track>=0 && tracks[video_track].codec==CODEC_HEVC){
        track_t *ht=&tracks[video_track];
        hevc_dec=cos_hevc_open(hevc_cfg,(uint32_t)hevc_cfg_len);
        if(!hevc_dec){msg("HEVC decoder initialization failed");have_video=false;return false;}
        /* display_index -> sample (decode order), to time each output frame by its presentation order */
        hevc_disp2dec=malloc((size_t)(ht->n>0?ht->n:1)*sizeof(int));
        if(!hevc_disp2dec){cos_hevc_close(hevc_dec);hevc_dec=NULL;msg("HEVC decoder allocation failed");have_video=false;return false;}
        for(int i=0;i<ht->n;i++)hevc_disp2dec[i]=i;
        for(int i=0;i<ht->n;i++){uint32_t di=ht->s[i].display_index;if(di<(uint32_t)ht->n)hevc_disp2dec[di]=i;}
        hevc_dec_open=true;hevc_drained=false;hevc_out_count=0;
        {   /* decode on another core when there is one: learn the picture size, then hand over */
            int hw_w=0,hw_h=0,hw_d=8;
            if(ht->n>0){
                /* the picture size is only known after one sample has been decoded; use a throwaway decoder */
                cos_hevc_frame_t pf;(void)cos_hevc_decode(hevc_dec,file_data+ht->s[0].off,(uint32_t)ht->s[0].size,&pf);
                if(cos_hevc_info(hevc_dec,&hw_w,&hw_h,&hw_d)<0)hw_w=0;
                cos_hevc_close(hevc_dec);hevc_dec=cos_hevc_open(hevc_cfg,(uint32_t)hevc_cfg_len);
            }
            if(hevc_dec&&hw_w>0&&hw_start(ht,hw_w,hw_h,hw_d,CODEC_HEVC)){cos_hevc_close(hevc_dec);hevc_dec=NULL;}
        }
        msg("HEVC decoder ready (FFmpeg hevc, 8/10/12-bit 4:2:0)");
    }
    if(video_track>=0 && tracks[video_track].codec==CODEC_AV1){
        Dav1dSettings settings; dav1d_default_settings(&settings);
        {   /* AVX2 asm is used when the CPU has it and the kernel enabled YMM state (XCR0, saved with xsave since r17; dav1d checks xgetbv itself). AVX-512 stays off: XCR0 has no ZMM state.
             * /linux/etc/cos-nosimd-avx2 (a file) switches AVX2 off again for comparisons and as a fallback. */
            extern void dav1d_set_cpu_flags_mask(unsigned mask);
            cos_stat_t st_noavx; bool noavx2=cos_stat("/linux/etc/cos-noavx2",&st_noavx)==0;
            dav1d_set_cpu_flags_mask(noavx2?~((1u<<3)|(1u<<4)):~(1u<<4));   /* DAV1D_X86_CPU_FLAG_AVX2 = 1<<3, _AVX512ICL = 1<<4 */
        }
        int nt=cos_par_cpus();if(nt>3)nt=3;if(nt<1)nt=1;
#ifdef COS_AV1_THREADS
        nt=COS_AV1_THREADS;
#endif
        settings.n_threads=nt; settings.max_frame_delay=nt;   /* as many frames in flight as threads: 1080p AV1 2.8 / 7.2 / 9.2 fps for 1 / 2 / 3 */
#ifdef COS_AV1_FD
        settings.max_frame_delay=COS_AV1_FD;
#endif settings.logger.callback=av1_log_cb;
        cos_pthread_arena_heap=(size_t)(128u<<20);
        int orc=dav1d_open(&av1_dec,&settings);
#if COS_VALIDATION_FRAME_CRC
        {char db[96];snprintf(db,sizeof db,"[AV1] open rc=%d threads=%d\n",orc,settings.n_threads);cos_puts(db);}
#endif
        if(orc<0&&(settings.n_threads=1,settings.max_frame_delay=1,dav1d_open(&av1_dec,&settings)<0)){msg("AV1 decoder initialization failed");have_video=false;return false;}
        av1_dec_open=true; msg("AV1 decoder ready (dav1d CPU)");
#if COS_VALIDATION_FRAME_CRC
        {char db[96];snprintf(db,sizeof db,"[AV1] threads=%d frame_delay=%d\n",settings.n_threads,settings.max_frame_delay);cos_puts(db);}
#endif
    }
    if(have_audio&&tracks[audio_track].codec==CODEC_AAC&&aac_cfg&&aac_cfg_len){unsigned long rate=0;unsigned char ch=0;aac_dec=NeAACDecOpen();if(aac_dec){NeAACDecConfigurationPtr cfg=NeAACDecGetCurrentConfiguration(aac_dec);if(cfg){cfg->outputFormat=FAAD_FMT_16BIT;NeAACDecSetConfiguration(aac_dec,cfg);}}if(!aac_dec||NeAACDecInit2(aac_dec,aac_cfg,aac_cfg_len,&rate,&ch)<0){if(aac_dec)NeAACDecClose(aac_dec);aac_dec=NULL;have_audio=false;msg("AAC decoder initialization failed");}else{audio_rate=(int)rate;audio_channels=ch?ch:2;}}
    return true;
}
static void clear_media(void){if(audio_opened){cos_audio_stop();cos_audio_close();audio_opened=false;}audio_clock_valid=false;audio_written_frames=0;playback_start_ms=0;audio_start_ms=0;if(aac_dec){NeAACDecClose(aac_dec);aac_dec=NULL;}free(aac_cfg);aac_cfg=NULL;aac_cfg_len=0;hw_stop();if(vp9_dec_open){if(vp9_dec)cos_vp9_close(vp9_dec);vp9_dec=NULL;vp9_dec_open=false;}if(hevc_dec_open){if(hevc_dec)cos_hevc_close(hevc_dec);hevc_dec=NULL;hevc_dec_open=false;}free(hevc_cfg);hevc_cfg=NULL;hevc_cfg_len=0;free(hevc_disp2dec);hevc_disp2dec=NULL;hevc_drained=false;hevc_out_count=0;if(av1_dec_open){if(av1_in_pending){dav1d_data_unref(&av1_in);av1_in_pending=false;}dav1d_close(&av1_dec);av1_dec_open=false;}av1_drained=false;av1_out_count=0;if(h264_dec){edge264_free(&h264_dec);}h264_flushed=h264_drained=false;h264_out_count=0;free(h264_cfg);h264_cfg=NULL;h264_cfg_len=0;h264_nal_length_size=4;h264_profile_idc=0;h264_level_idc=0;for(unsigned i=0;i<MEDIA_FRAME_POOL_SLOTS;i++){free(frame_pool[i].data);frame_pool[i].data=NULL;frame_pool[i].capacity=0;}frame_rgba=NULL;frame_w=frame_h=0;frame_pool_next=0;frame_generation=presented_generation=0;packet_decode_pos=frame_output_pos=frame_present_pos=dropped_frames=decode_errors=0;pts_first_ms=pts_last_ms=decode_total_ms=decode_max_ms=0;have_pts_sample=false;free(audio_data);audio_data=NULL;audio_len=audio_pos=0;free(file_data);file_data=NULL;file_size=0;ntracks=0;video_track=audio_track=-1;have_video=have_audio=false;playing=paused=false;}
static bool load_path(const char *p){
    clear_media();cos_stat_t st; if(!p||cos_stat(p,&st)<0||st.is_dir){msg("Cannot open file");return false;} if(st.size>MAX_BYTES){msg("File is larger than 512 MiB");return false;}
    int fd=cos_open(p,COS_O_RDONLY);if(fd<0){msg("Open failed");return false;}file_data=malloc((size_t)st.size);if(!file_data){cos_close(fd);msg("Not enough memory");return false;}uint64_t got=0;while(got<st.size){ssize_t r=cos_fd_read(fd,(char*)file_data+got,(size_t)(st.size-got));if(r<=0)break;got+=(uint64_t)r;}cos_close(fd);if(got!=st.size){msg("Read failed");clear_media();return false;}file_size=got;snprintf(path_buf,sizeof path_buf,"%s",p);
    if(!parse_mp4()){clear_media();return false;}
    if(have_audio){track_t *a=&tracks[audio_track];for(int i=0;i<a->n;i++)if(in_file(a->s[i].off,a->s[i].size))audio_len+=(size_t)a->s[i].size;audio_data=malloc(audio_len);if(!audio_data){have_audio=false;msg("Video ready (audio buffer unavailable)");}else{size_t o=0;for(int i=0;i<a->n;i++){if(in_file(a->s[i].off,a->s[i].size)){memcpy(audio_data+o,file_data+a->s[i].off,(size_t)a->s[i].size);o+=(size_t)a->s[i].size;}}audio_len=o;mp3dec_init(&mp3);}}
    video_pos=0; packet_decode_pos=0; frame_output_pos=0; frame_present_pos=0; dropped_frames=0; decode_errors=0; return true;
}
static void audio_step(void){
    if(!have_audio||!audio_data||audio_pos>=audio_len)return;
    if(!audio_opened){if(cos_audio_open((uint32_t)audio_rate,2)!=0){msg("Video playing (audio device unavailable)");return;}audio_opened=true;audio_clock_valid=true;audio_start_ms=cos_time_ms();audio_written_frames=0;cos_audio_set_volume(85);}
    cos_audio_status_t st;if(cos_audio_status(&st)!=0||st.free_samples<1024)return;
    track_t *a=&tracks[audio_track];
    if(a->codec==CODEC_AAC){
        /* MP4/AAC samples are raw access units. FAAD2 consumes one sample at a time. */
        NeAACDecFrameInfo info;memset(&info,0,sizeof info);
        int16_t *decoded=(int16_t*)NeAACDecDecode(aac_dec,&info,audio_data+audio_pos,(unsigned long)(audio_len-audio_pos));
        size_t consumed=info.bytesconsumed;
        if(consumed==0)consumed=1;if(audio_pos+consumed>audio_len)consumed=audio_len-audio_pos;audio_pos+=consumed;
        if(info.error||!decoded||!info.samples||!info.channels){if(info.error){char *err=NeAACDecGetErrorMessage(info.error);if(err)msg(err);}return;}
        if(info.samplerate)audio_rate=(int)info.samplerate;audio_channels=info.channels;
#if COS_VALIDATION_FRAME_CRC
        {static int aac_logged; if(!aac_logged){aac_logged=1; char tb[96]; snprintf(tb,sizeof tb,"[AAC] first frame ok: rate=%lu ch=%d samples=%lu\n",(unsigned long)info.samplerate,(int)info.channels,(unsigned long)info.samples); cos_puts(tb);}}
#endif
        unsigned long frames=info.samples/info.channels;if(frames>4096)frames=4096;int16_t pcm[8192];
        for(unsigned long i=0;i<frames;i++){int16_t l=decoded[i*info.channels],r=decoded[i*info.channels+(info.channels>1?1:0)];pcm[i*2]=l;pcm[i*2+1]=r;}
        int64_t accepted=cos_audio_write(pcm,frames*2);if(accepted>0)audio_written_frames+=(uint64_t)accepted/2u;return;
    }
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME*2];mp3dec_frame_info_t inf;int n=mp3dec_decode_frame(&mp3,audio_data+audio_pos,(int)(audio_len-audio_pos),pcm,&inf);if(n<=0){audio_pos+=inf.frame_bytes>0?(size_t)inf.frame_bytes:1;return;}audio_pos+=inf.frame_bytes;if(inf.hz)audio_rate=inf.hz;if(inf.channels>0)audio_channels=inf.channels;int out=0;for(int i=0;i<n;i++){int16_t l=pcm[i*inf.channels],r=pcm[i*inf.channels+(inf.channels>1?1:0)];pcm[out++]=l;pcm[out++]=r;}int64_t accepted=cos_audio_write(pcm,(uint64_t)out);if(accepted>0)audio_written_frames+=(uint64_t)accepted/2u;
}
/* edge264_get_frame() returns 0 when it hands out a frame and ENOMSG when
 * none is ready - the opposite of the usual "positive means data" - and
 * every call that returns 0 consumes one queued output frame. So fetch
 * exactly one frame per call, push it, return it, and leave any others in
 * edge264's queue for the next step instead of draining and dropping them. */
static bool h264_take_frame(Edge264Frame *fr,track_t *t,uint64_t *pts,uint64_t *dur,bool *stored_out){
    memset(fr,0,sizeof *fr);
    if(edge264_get_frame(h264_dec,fr,1)!=0||!fr->samples[0])return false;
    int w=fr->width_Y,h=fr->height_Y;
    if(w>MAX_VIDEO_W||h>MAX_VIDEO_H){msg("Video exceeds 4K limit (3840x2160)");playing=false;edge264_return_frame(h264_dec,fr->return_arg);*stored_out=false;return true;}
    /* edge264's output_queue is already display-order, while FrameId is
     * explicitly assigned in decode order. Map that identifier back to the
     * MP4 sample PTS so B-frames are timed by presentation order rather than
     * by the access unit that happened to produce them. */
    if(fr->FrameId>=0&&fr->FrameId<t->n){*pts=sample_pts_ms(t,fr->FrameId);*dur=sample_duration_ms(t,fr->FrameId);}
    *stored_out=media_push_yuv420(fr->samples[0],fr->samples[1],fr->samples[2],w,h,fr->stride_Y,fr->stride_C,8);
    edge264_return_frame(h264_dec,fr->return_arg);
    h264_out_count++;
    return true;
}
/* True while video_step() still has work: more samples to feed, or (H.264)
 * frames that edge264 is still holding back for reordering. */
static bool h264_take_frame_adapter(Edge264Frame *fr,uint64_t *pts,uint64_t *dur,bool *stored){return h264_take_frame(fr,&tracks[video_track],pts,dur,stored);}
static bool video_more(void){
    if(!have_video||video_track<0)return false;
    if(video_pos<tracks[video_track].n)return true;
    if(tracks[video_track].codec==CODEC_HEVC&&hevc_dec_open&&!hevc_drained)return true;
    if(tracks[video_track].codec==CODEC_AV1&&av1_dec_open&&(av1_in_pending||!av1_drained))return true;
    return tracks[video_track].codec==CODEC_H264&&h264_dec&&!h264_drained;
}
/* Takes one finished AV1 picture if dav1d has one (it decodes ahead on other cores, so a picture rarely
 * belongs to the sample just sent). Returns 1 if a picture was handed to the display, 0 if none was ready,
 * -1 on error. */
static int av1_take_picture(void){
    Dav1dPicture pic; memset(&pic,0,sizeof pic);
    int rc=dav1d_get_picture(av1_dec,&pic);
    if(rc==DAV1D_ERR(EAGAIN))return 0;
    if(rc<0){msg("AV1 picture output failed");return -1;}
    if(!pic.data[0])return 0;
    int w=pic.p.w,h=pic.p.h;
    if(w>MAX_VIDEO_W||h>MAX_VIDEO_H){dav1d_picture_unref(&pic);msg("Video exceeds 4K limit (3840x2160)");return -1;}
    if(pic.p.layout!=DAV1D_PIXEL_LAYOUT_I420||(pic.p.bpc!=8&&pic.p.bpc!=10&&pic.p.bpc!=12)){dav1d_picture_unref(&pic);msg("AV1 requires 8, 10 or 12-bit 4:2:0 output");return -1;}
    track_t *t=&tracks[video_track];
    if(av1_out_count<t->n){uint64_t dpts=sample_pts_ms(t,av1_out_count);if(dpts>pts_last_ms||!have_pts_sample)pts_last_ms=dpts;have_pts_sample=true;next_frame_ms=dpts;}
    av1_out_count++;
    /* dav1d stores >8bpc samples as little-endian uint16_t with stride in bytes; the kernel's YUV->RGBA path handles that layout. */
    bool stored=media_push_yuv420(pic.data[0],pic.data[1],pic.data[2],w,h,(int)pic.stride[0],(int)pic.stride[1],(int)pic.p.bpc);
    dav1d_picture_unref(&pic);
    if(stored){frame_output_pos++;video_clock_valid=true;}
    return 1;
}
static void video_step(void){
    if(have_video&&video_track>=0&&tracks[video_track].codec==CODEC_AV1&&av1_dec_open&&(av1_in_pending||video_pos<tracks[video_track].n)){
        track_t *t=&tracks[video_track]; uint64_t av1_t0=cos_time_ms();
        if(!av1_in_pending){
            sample_t *s=&t->s[video_pos];
            if(!in_file(s->off,s->size)||s->size>32*1024*1024){msg("Invalid video sample");playing=false;return;}
            memset(&av1_in,0,sizeof av1_in);
            uint8_t *copy=dav1d_data_create(&av1_in,(size_t)s->size);
            if(!copy){msg("AV1 input allocation failed");playing=false;return;}
            memcpy(copy,file_data+s->off,(size_t)s->size);
            av1_in_pending=true;video_pos++;packet_decode_pos++;
        }
        int rc=dav1d_send_data(av1_dec,&av1_in);
        if(rc==0||av1_in.sz==0)av1_in_pending=false;
        else if(rc!=DAV1D_ERR(EAGAIN)){dav1d_data_unref(&av1_in);av1_in_pending=false;msg("AV1 bitstream decode failed");playing=false;return;}
        if(av1_take_picture()<0){playing=false;return;}
        {uint64_t e=cos_time_ms()-av1_t0;decode_total_ms+=e;if(e>decode_max_ms)decode_max_ms=e;}      /* the stats line: time spent in dav1d per sample */
        if(video_pos>=t->n&&!av1_in_pending)av1_drained=false;
        return;
    }
    if(have_video&&video_track>=0&&tracks[video_track].codec==CODEC_AV1&&av1_dec_open&&!av1_drained){
        /* every sample sent: collect the pictures dav1d still holds */
        int r=av1_take_picture();
        if(r<0){playing=false;return;}
        if(r==0){static int idle;if(av1_out_count>=tracks[video_track].n||++idle>=2000){av1_drained=true;idle=0;}else cos_sleep_ms(1);}
        return;
    }
    if(have_video&&video_track>=0&&video_pos>=tracks[video_track].n&&tracks[video_track].codec==CODEC_H264&&h264_dec&&!h264_drained){
        /* All samples fed: flush once, then hand out the frames edge264 was
         * holding for reordering, one per step, before declaring EOF. */
        /* An empty NAL makes edge264 bump every pending frame into its output
         * queue. (edge264_flush() would instead discard them and reset.) */
        if(!h264_flushed){static const uint8_t eos=0;edge264_decode_NAL(h264_dec,&eos,&eos,NULL,NULL);h264_flushed=true;}
        Edge264Frame dfr; uint64_t dpts=pts_last_ms,ddur=0; bool dstored=false;
        if(h264_take_frame(&dfr,&tracks[video_track],&dpts,&ddur,&dstored)){
            if(dstored){frame_output_pos++;video_clock_valid=true;if(dpts>pts_last_ms||!have_pts_sample)pts_last_ms=dpts;have_pts_sample=true;next_frame_ms=dpts;}
            return;
        }
        /* frames still being decoded on other cores are not ready yet: wait for all of them */
        {static int idle;if(h264_out_count<tracks[video_track].n&&++idle<3000){cos_sleep_ms(1);return;}idle=0;}
        h264_drained=true;
    }
    if(have_video&&video_track>=0&&video_pos>=tracks[video_track].n&&tracks[video_track].codec==CODEC_HEVC&&hevc_dec_open&&!hevc_drained){
        /* All samples fed: collect the frames the decoder still holds back for reordering. */
        cos_hevc_frame_t df; int dr=hevc_async?hw_pop(HW_DRAIN_KIND,0,&df):cos_hevc_drain(hevc_dec,&df);
        if(dr==1){
            if(hevc_out_count<tracks[video_track].n){uint64_t dpts=sample_pts_ms(&tracks[video_track],hevc_disp2dec[hevc_out_count]);if(dpts>pts_last_ms||!have_pts_sample)pts_last_ms=dpts;have_pts_sample=true;next_frame_ms=dpts;}
            hevc_out_count++;
            if(media_push_yuv420(df.y,df.u,df.v,df.width,df.height,df.stride_y,df.stride_c,df.bit_depth)){frame_output_pos++;video_clock_valid=true;}
            return;
        }
        hevc_drained=true;
    }
    if(!have_video||video_pos>=tracks[video_track].n){playing=false;msg("Playback finished");media_log_stats("eof");return;}uint64_t decode_begin=cos_time_ms();int sample_index = video_pos++; packet_decode_pos++;
    /* 32 MiB: generous even for a 4K I-frame at a high bitrate (raised
     * alongside MAX_VIDEO_W/H - the old 8 MiB cap was sized for 1080p). */
    sample_t *s=&tracks[video_track].s[sample_index];if(!in_file(s->off,s->size)||s->size>32*1024*1024){msg("Invalid video sample");playing=false;return;}track_t *t=&tracks[video_track];
    uint64_t output_pts_ms = sample_pts_ms(t, sample_index); uint64_t output_duration_ms = sample_duration_ms(t, sample_index); bool frame_emitted=false;
    if(t->codec==CODEC_JPEG){int w=0,h=0,c=0;uint8_t *px=stbi_load_from_memory(file_data+s->off,(int)s->size,&w,&h,&c,4);if(!px){msg("JPEG frame decode failed");playing=false;return;}if(w>MAX_VIDEO_W||h>MAX_VIDEO_H){stbi_image_free(px);msg("Video exceeds 4K limit (3840x2160)");playing=false;return;}if(!frame_prepare(w,h)){stbi_image_free(px);msg("Frame pool allocation failed");playing=false;return;}memcpy(frame_rgba,px,(size_t)w*(size_t)h*4u);stbi_image_free(px);frame_output_pos++; frame_emitted=true; video_clock_valid=true;}
    else if(t->codec==CODEC_H264 && h264_dec){
        uint8_t *blk=malloc(16+(size_t)s->size*2+16); if(!blk){msg("H.264 sample buffer unavailable");playing=false;return;} *(volatile int*)blk=1; uint8_t *nal_buf=blk+16;
        uint32_t at=0, out=0; int nal_count=0; while(at+h264_nal_length_size<=s->size){uint32_t n=0;for(uint8_t z=0;z<h264_nal_length_size;z++)n=(n<<8)|file_data[s->off+at+z];at+=h264_nal_length_size;if(!n||n>s->size-at||out+4+n>(uint32_t)s->size*2+16)break;nal_buf[out++]=0;nal_buf[out++]=0;nal_buf[out++]=0;nal_buf[out++]=1;memcpy(nal_buf+out,file_data+s->off+at,n);out+=n;at+=n;nal_count++;}
        h264_feed_annexb_ref(nal_buf,out,(volatile int*)blk);
        h264_nal_unref(0,blk);                                   /* the feeder's own reference */
        Edge264Frame fr; bool stored=false;
        if(h264_take_frame(&fr,t,&output_pts_ms,&output_duration_ms,&stored)){
            if(!playing)return;
            if(stored){frame_output_pos++; frame_emitted=true; video_clock_valid=true;}
        } else if(video_pos==1){snprintf(status_buf,sizeof status_buf,"H.264 decode pending nals=%d bytes=%u",nal_count,(unsigned)s->size);}
    } else if(t->codec==CODEC_HEVC && hevc_dec_open){
        cos_hevc_frame_t hf;
#if COS_VALIDATION_FRAME_CRC
        uint64_t dec_t0=cos_time_ms();
#endif
        int rc=hevc_async?hw_pop(HW_SAMPLE_KIND,sample_index,&hf):cos_hevc_decode(hevc_dec,file_data+s->off,(uint32_t)s->size,&hf);
#if COS_VALIDATION_FRAME_CRC
        { static uint64_t dec_total_ms; static int dec_n; dec_total_ms+=cos_time_ms()-dec_t0; if(++dec_n%16==0){char db[96]; snprintf(db,sizeof db,"[DECMS] samples=%d total_ms=%llu\n",dec_n,(unsigned long long)dec_total_ms); cos_puts(db);} }
#endif
        if(rc<0){
#if COS_VALIDATION_FRAME_CRC
            {char db[200];snprintf(db,sizeof db,"[HEVCFAIL] sample=%d off=%llu size=%llu file=%llu bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",sample_index,(unsigned long long)s->off,(unsigned long long)s->size,(unsigned long long)file_size,file_data[s->off],file_data[s->off+1],file_data[s->off+2],file_data[s->off+3],file_data[s->off+4],file_data[s->off+5],file_data[s->off+6],file_data[s->off+7]);cos_puts(db);}
#endif
            msg("HEVC decode failed");playing=false;return;}
        if(rc==2){msg("HEVC 4:2:2 / 4:4:4 / monochrome / 9-bit not supported");playing=false;return;}
        if(rc==1){
            if(hf.width>MAX_VIDEO_W||hf.height>MAX_VIDEO_H){msg("Video exceeds 4K limit (3840x2160)");playing=false;return;}
            if(hf.bit_depth!=8&&hf.bit_depth!=10&&hf.bit_depth!=12){msg("HEVC bit depth not supported");playing=false;return;}
            /* frames come out in display order: the k-th output frame is the sample with display_index k */
            if(hevc_out_count<t->n){int di=hevc_disp2dec[hevc_out_count];output_pts_ms=sample_pts_ms(t,di);output_duration_ms=sample_duration_ms(t,di);}
            hevc_out_count++;
            bool stored=media_push_yuv420(hf.y,hf.u,hf.v,hf.width,hf.height,hf.stride_y,hf.stride_c,hf.bit_depth);
            if(stored){ frame_output_pos++; frame_emitted=true; video_clock_valid=true; }
        }
    } else if(t->codec==CODEC_VP9 && vp9_dec_open){
        cos_vp9_frame_t vf;
        int rc=(hevc_async&&hw_codec==CODEC_VP9)?vp9_hw_pop(sample_index,&vf):cos_vp9_decode(vp9_dec,file_data+s->off,(uint32_t)s->size,&vf);
        if(rc<0){msg("VP9 decode failed");playing=false;return;}
        if(rc>0){
            int w=vf.width,h=vf.height;
            if(w>MAX_VIDEO_W||h>MAX_VIDEO_H){msg("Video exceeds 4K limit (3840x2160)");playing=false;return;}
            if(vf.bit_depth!=8&&vf.bit_depth!=10&&vf.bit_depth!=12){msg("VP9 bit depth not supported");playing=false;return;}
            bool stored=media_push_yuv420(vf.y,vf.u,vf.v,w,h,vf.stride_y,vf.stride_c,vf.bit_depth);
            if(stored){ frame_output_pos++; frame_emitted=true; video_clock_valid=true; }
        }
    }
    uint64_t decode_elapsed=cos_time_ms()-decode_begin; decode_total_ms+=decode_elapsed; if(decode_elapsed>decode_max_ms)decode_max_ms=decode_elapsed;
    if(frame_emitted){ if(!have_pts_sample||output_pts_ms<pts_first_ms)pts_first_ms=output_pts_ms; pts_last_ms=output_pts_ms; have_pts_sample=true; if(frame_output_pos>0u && (frame_output_pos%30u)==0u)media_log_stats(t->codec==CODEC_H264?"h264":(t->codec==CODEC_VP9?"vp9":(t->codec==CODEC_AV1?"av1":"jpeg"))); }
    uint64_t dur=output_duration_ms; next_frame_ms=output_pts_ms>UINT64_MAX-dur?UINT64_MAX:output_pts_ms+dur; if(video_clock_valid) next_frame_ms=output_pts_ms;}
static void draw(void){cui_fill(&cv,0,0,cv.w,cv.h,0x0D1119);cui_fill(&cv,0,0,cv.w,54,0x1A2333);cui_text(&cv,cui_font_bold(16),20,16,"Media",0xF3F6FF);cui_text(&cv,cui_font_default(11),92,20,"MP4 video player",0x9BA9C2);cui_fill(&cv,0,54,cv.w,1,0x33445E);int bx=18,by=72,bw=cv.w-36,bh=cv.h-154;cui_round_rect(&cv,bx,by,bw,bh,8,0x151C28);if(frame_rgba){float sx=(float)(bw-24)/frame_w,sy=(float)(bh-24)/frame_h,sc=sx<sy?sx:sy;int dw=(int)(frame_w*sc),dh=(int)(frame_h*sc),x=bx+(bw-dw)/2,y=by+(bh-dh)/2;cos_win2_video_frame(win, (const uint32_t*)frame_rgba, frame_w, frame_h, frame_w, x, y, dw, dh);}else if(!frame_output_pos){cui_text(&cv,cui_font_default(14),bx+24,by+bh/2-10,"Open an MP4 file from Files or drag it here",0xAEBBD0);}
    /* else: a video codec already pushed this frame straight to the kernel
     * (media_push_yuv420() - see its comment), so there is nothing left
     * to upload here; the compositor keeps presenting it every tick on
     * its own. */
    int y=cv.h-86;cui_round_rect(&cv,18,y,92,34,6,playing&&!paused?0x4D83E6:0x29364A);cui_text(&cv,cui_font_bold(13),40,y+10,playing&&!paused?"Pause":"Play",0xFFFFFF);cui_round_rect(&cv,120,y,92,34,6,0x29364A);cui_text(&cv,cui_font_default(13),145,y+10,"Stop",0xD6DEED);cui_text_fit(&cv,cui_font_default(11),230,y+11,cv.w-250,status_buf,0xAAB7CC);if(stats_buf[0])cui_text_fit(&cv,cui_font_default(10),230,y+26,cv.w-250,stats_buf,0x8794AB);cui_text_fit(&cv,cui_font_default(10),20,cv.h-34,cv.w-40,path_buf[0]?path_buf:"No file selected",0x77859C);}
/* once a second while playing: frame size, decoded frames per second, average and worst decode time, dropped frames (so a slow machine shows it without a log) */
static void update_stats(void){
    static uint64_t t0; static uint32_t n0; uint64_t now=cos_time_ms();
    if(!playing||!have_video||!frame_output_pos){stats_buf[0]=0;t0=0;return;}
    if(!t0||frame_output_pos<n0){t0=now;n0=frame_output_pos;return;}
    if(now-t0<1000)return;
    unsigned fps10=(unsigned)((uint64_t)(frame_output_pos-n0)*10000u/(now-t0)); uint64_t avg=decode_total_ms/(uint64_t)frame_output_pos;
    snprintf(stats_buf,sizeof stats_buf,"%dx%d  %u.%u fps  decode %llu ms avg, %llu ms worst  dropped %u",frame_w,frame_h,fps10/10,fps10%10,(unsigned long long)avg,(unsigned long long)decode_max_ms,(unsigned)dropped_frames);
    t0=now;n0=frame_output_pos;
}
static void present(void){update_stats();cui_canvas_init(&cv,wi.pixels,wi.width,wi.height,wi.stride);draw();cos_win2_present(win);if(frame_output_pos&&presented_generation!=frame_generation){presented_generation=frame_generation;frame_present_pos++;}}
#if COS_THREAD_BENCH
static volatile uint64_t bench_sink[8];
static volatile uint32_t bench_flag;
static void bench_fn(void *a){uint64_t id=(uint64_t)(uintptr_t)a,x=88172645463325252ull+id;for(uint32_t i=0;i<30000000u;i++){x^=x<<13;x^=x>>7;x^=x<<17;}bench_sink[id&7]=x;}
static void bench_syscall(void *a){(void)a;cos_puts("must not run on an AP\n");}
static void bench_wild(void *a){(void)a;*(volatile uint32_t*)0x30000000u=1;}
static void bench_spin(void *a){(void)a;bench_flag=1;for(;;)__asm__ volatile("pause");}
static int bench_run(void (*fn)(void*),void *arg,cos_par_fault_t *f,int wait_stop){
    void *stk=malloc(65536);memset(stk,0,65536);
    int id=cos_par_start(fn,arg,stk,65536);if(id<0)return id-1000;
    int st;
    if(wait_stop){while(!bench_flag)cos_sleep_ms(1);cos_par_stop(id);}
    while((st=cos_par_poll(id,f))<COS_PAR_DONE)cos_sleep_ms(1);
    cos_par_free(id);free(stk);return st;
}
static void thread_bench(void){
    char b[200];cos_par_fault_t f;memset(&f,0,sizeof f);
    snprintf(b,sizeof b,"[PARBENCH] cpus=%d\n",cos_par_cpus());cos_puts(b);
    for(int i=0;i<8;i++)bench_sink[i]=0;
    for(int n=1;n<=3;n++){
        int id[4];void *stk[4];uint64_t t0=cos_time_ms();
        for(int i=0;i<n;i++){stk[i]=malloc(65536);memset(stk[i],0,65536);id[i]=cos_par_start(bench_fn,(void*)(uintptr_t)i,stk[i],65536);}
        int bad=0;
        for(int i=0;i<n;i++){if(id[i]<0){bad=id[i];continue;}int st;while((st=cos_par_poll(id[i],&f))<COS_PAR_DONE)cos_sleep_ms(1);if(st!=COS_PAR_DONE)bad=st;cos_par_free(id[i]);}
        snprintf(b,sizeof b,"[PARBENCH] n=%d wall_ms=%llu bad=%d sink0=%llx\n",n,(unsigned long long)(cos_time_ms()-t0),bad,(unsigned long long)bench_sink[0]);cos_puts(b);
        for(int i=0;i<n;i++)free(stk[i]);
    }
    int st=bench_run(bench_syscall,0,&f,0);
    snprintf(b,sizeof b,"[PARBENCH] syscall: state=%d vec=%llu\n",st,(unsigned long long)f.vec);cos_puts(b);
    st=bench_run(bench_wild,0,&f,0);
    snprintf(b,sizeof b,"[PARBENCH] wild: state=%d vec=%llu cr2=%llx\n",st,(unsigned long long)f.vec,(unsigned long long)f.cr2);cos_puts(b);
    bench_flag=0;st=bench_run(bench_spin,0,&f,1);
    snprintf(b,sizeof b,"[PARBENCH] stop: state=%d (5=killed)\n",st);cos_puts(b);
    /* after the faults and the kill the cores must still take ordinary jobs */
    st=bench_run(bench_fn,(void*)(uintptr_t)7,&f,0);
    snprintf(b,sizeof b,"[PARBENCH] after: state=%d\n",st);cos_puts(b);
    /* leave a worker spinning and exit: the kernel has to reclaim the core */
    bench_flag=0;{void *stk=malloc(65536);memset(stk,0,65536);int id=cos_par_start(bench_spin,0,stk,65536);while(!bench_flag)cos_sleep_ms(1);(void)id;}
    cos_puts("[PARBENCH] exiting with a worker running\n");
    cos_exit(0);
}
#endif


int main(int argc,char **argv){
#if COS_THREAD_BENCH
    thread_bench();
#endif
    win=cos_win2_create("Media",W,H,&wi);if(win<0)return 1;
    if(argc>1&&argv[1]){if(load_path(argv[1])){playing=true;paused=false;playback_start_ms=cos_time_ms();audio_clock_valid=false;video_clock_valid=false;audio_written_frames=0;}}else msg("Open an MP4 file from Files or drag it here");for(;;){cos_win_event_t e;int got=0;while(cos_win2_poll(win,&e)>0){got=1;if(e.type==COS_EV_CLOSE){clear_media();cos_win2_close(win);return 0;}if(e.type==COS_EV_OPEN||e.type==COS_EV_DROP){char p[256];if(cos_win2_get_path(win,p,sizeof p)>=0)load_path(p);}if(e.type==COS_EV_MOUSE_UP){if(e.y>=wi.height-100&&e.y<wi.height-40&&e.x<115){if(!have_video){msg("Open a supported MP4 first");}else{playing=!playing;paused=false;if(playing&&video_pos>=tracks[video_track].n)video_pos=0;video_clock_valid=false;playback_start_ms=cos_time_ms();audio_clock_valid=false;audio_written_frames=0;}}else if(e.y>=wi.height-100&&e.y<wi.height-40&&e.x<220){playing=false;paused=false;video_pos=0;audio_pos=0;if(audio_opened)cos_audio_stop();msg("Stopped");}}}if(playing&&!paused){
            /* audio_step() already self-limits via cos_audio_status()'s
             * free_samples check, so it is safe to call it in a bounded
             * burst here - a single call per tick starves audio whenever
             * a tick runs long (slow H.264/AV1 decode, a GUI hiccup):
             * one MP3/AAC frame is only ~20-26ms of audio, so one call
             * per 100ms+ tick can't keep the device's ring buffer full no
             * matter how fast the decoder itself is. */
            for(int ai=0;ai<8;ai++) audio_step();
            uint64_t clock=media_clock_ms(); int decoded=0;
#if COS_VALIDATION_FRAME_CRC
            {static uint64_t last_trace; uint64_t now=cos_time_ms(); if(now-last_trace>=2000){last_trace=now; cos_audio_status_t ast; memset(&ast,0,sizeof ast); int ar=cos_audio_status(&ast); char tb[160]; snprintf(tb,sizeof tb,"[MEDIATRACE] clock=%llu next=%llu vpos=%d ar=%d pend=%u free=%u written=%llu apos=%u/%u\n",(unsigned long long)clock,(unsigned long long)next_frame_ms,video_pos,ar,(unsigned)ast.pending_samples,(unsigned)ast.free_samples,(unsigned long long)audio_written_frames,(unsigned)audio_pos,(unsigned)audio_len); cos_puts(tb);}}
#endif
            /* Decode a small bounded burst when the CPU fell behind. This
             * catches up without starving audio or the GUI event loop. */
            /* A decoder that cannot keep up (slow CPU, 1 or 2 cores, 1080p60) falls further behind the audio with every frame: when the picture is a second late, jump to the
             * next key frame at or after the audio clock (the samples in between are never decoded; "dropped" counts them). The picture stays on the last frame until then. */
            if(have_video&&video_track>=0&&video_clock_valid&&!hevc_async&&clock>next_frame_ms+1000){
                track_t *vt=&tracks[video_track]; int j=video_pos;
                while(j<vt->n&&!(vt->s[j].sync&&sample_pts_ms(vt,j)>=clock))j++;
                if(j<vt->n&&j>video_pos){dropped_frames+=(uint32_t)(j-video_pos);video_pos=j;
                    if(vt->codec==CODEC_H264&&h264_dec)edge264_flush(h264_dec);
                    if(vt->codec==CODEC_AV1&&av1_dec_open){if(av1_in_pending){dav1d_data_unref(&av1_in);av1_in_pending=false;}dav1d_flush(av1_dec);av1_drained=false;}
next_frame_ms=sample_pts_ms(vt,j)>150?sample_pts_ms(vt,j)-150:0;}
            }
            while(video_more() && (!video_clock_valid || next_frame_ms<=clock+2) && decoded<3){video_step();decoded++;}
        }/* The video frames themselves are composited by the kernel (media_push_yuv420),
         * so the window chrome only needs repainting when something changed (input, state)
         * or at ~10 Hz for the status text. Repainting every 10 ms tick cost about a fifth of
         * all CPU time during H.264 playback (rounded rects and fills). */
        {static uint64_t last_present; static bool was_playing; uint64_t now=cos_time_ms(); bool changed=(was_playing!=playing); was_playing=playing; if(got||changed||(playing&&(now-last_present>=100||last_present==0))){present();last_present=now;}}
        /* Keep decoding cooperative even when the timer quantum is long. */cos_yield();cos_sleep_ms(10);} }
