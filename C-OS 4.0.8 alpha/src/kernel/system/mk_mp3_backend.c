/**
 * mk_mp3_backend.c - strengthened MP3 player backend shim
 *
 * This version focuses on practical kernel-tree playback support:
 *   - playlist and directory loading
 *   - lightweight ID3v1 metadata extraction
 *   - header-based bitrate/sample-rate/channel probing
 *   - repeat / shuffle / next / previous navigation
 *   - track state snapshots for GUI and shell consumers
 */
#include "mk_storage.h"
#include "fs.h"
#include "mk_mp3.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "ac97.h"
#define MINIMP3_NO_STDIO
#include "minimp3.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;

extern uint64_t get_timer_ticks(void);

#define MP3_MAX_ORDER 256
#define MP3_DEFAULT_DURATION_MS 180000ULL
#define MP3_DEFAULT_VOLUME 80ULL
#define MP3_DEFAULT_BALANCE 50ULL

static mk_mp3_player_t g_player;
static mk_mp3_playlist_t g_playlist;
static mk_mp3_equalizer_t g_equalizer;
static bool g_initialized = false;
static u64 g_last_tick = 0;
static char g_current_path[MK_MP3_MAX_FILENAME_LENGTH];
static u64 g_play_order[MP3_MAX_ORDER];
static u64 g_play_order_count = 0;
static u64 g_play_order_pos = 0;
static int g_repeat_mode = 0; /* 0=off, 1=one, 2=all */
static mk_storage_file_t* g_open_file = NULL;
static u64 g_stream_offset = 0;
static u32 g_synth_phase = 0;

#define MP3_DECODE_INPUT_SIZE 8192
#define MP3_AUDIO_QUEUE_SAMPLES 65536

static mp3dec_t g_mp3_decoder;
static u8 g_decode_input[MP3_DECODE_INPUT_SIZE];
static u64 g_decode_input_offset = 0;
static size_t g_decode_input_filled = 0;
static bool g_decode_eof = false;

static int64_t g_audio_queue[MP3_AUDIO_QUEUE_SAMPLES];
static size_t g_audio_queue_head = 0;
static size_t g_audio_queue_tail = 0;
static size_t g_audio_queue_count = 0;
static u64 g_audio_sample_rate = 44100;
static u64 g_audio_channels = 2;
static u64 g_audio_bits_per_sample = 16;

static void mp3_audio_queue_reset(void);
static void mp3_decoder_reset(void);
static void mp3_sync_player_from_current(bool preserve_state);
static void mp3_reset_play_cursor(void);
static void mp3_finish_track_if_needed(void);

static void mp3_copy_str(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_size && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void mp3_trim_inplace(char* s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == '\0')) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n') {
        ++start;
    }
    if (start > 0) {
        size_t i = 0;
        while (s[start + i]) {
            s[i] = s[start + i];
            ++i;
        }
        s[i] = '\0';
    }
}

static const char* mp3_basename(const char* path) {
    if (!path || !path[0]) return "Unknown";
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return (*last) ? last : path;
}

static void mp3_strip_extension(char* s) {
    if (!s) return;
    size_t len = strlen(s);
    for (size_t i = len; i > 0; --i) {
        if (s[i - 1] == '.') {
            s[i - 1] = '\0';
            return;
        }
        if (s[i - 1] == '/' || s[i - 1] == '\\') return;
    }
}

static bool mp3_has_suffix_ci(const char* name, const char* suffix) {
    if (!name || !suffix) return false;
    size_t nl = strlen(name);
    size_t sl = strlen(suffix);
    if (sl > nl) return false;
    const char* p = name + (nl - sl);
    for (size_t i = 0; i < sl; ++i) {
        char a = p[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void mp3_join_path(char* out, size_t out_size, const char* dir, const char* name) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!dir || !dir[0]) {
        mp3_copy_str(out, out_size, name);
        return;
    }
    if (!name || !name[0]) {
        mp3_copy_str(out, out_size, dir);
        return;
    }
    size_t pos = 0;
    for (; pos + 1 < out_size && dir[pos]; ++pos) out[pos] = dir[pos];
    if (pos > 0 && out[pos - 1] != '/' && out[pos - 1] != '\\' && pos + 1 < out_size) {
        out[pos++] = '/';
    }
    for (size_t i = 0; pos + 1 < out_size && name[i]; ++i) {
        out[pos++] = name[i];
    }
    out[pos] = '\0';
}

static bool mp3_is_audio_file(const char* path) {
    const char* name = mp3_basename(path);
    return mp3_has_suffix_ci(name, ".mp3") ||
           mp3_has_suffix_ci(name, ".wav") ||
           mp3_has_suffix_ci(name, ".ogg") ||
           mp3_has_suffix_ci(name, ".flac");
}


static void mp3_close_stream(void) {
    if (g_open_file) {
        mk_storage_close_file(g_open_file);
        g_open_file = NULL;
    }
    g_player.current_file = NULL;
}

static int mp3_open_stream_for_current(void) {
    if (g_playlist.count == 0 || g_playlist.current >= g_playlist.count) return -1;
    mp3_close_stream();
    if (!g_current_path[0]) return -1;
    g_open_file = mk_storage_open_file(g_current_path);
    if (!g_open_file) return -1;
    g_player.current_file = g_open_file;
    g_stream_offset = (g_player.current_position * (g_player.bitrate ? g_player.bitrate : 128ULL)) / 8ULL;
    if (g_stream_offset > g_open_file->size) g_stream_offset = 0;
    if (g_stream_offset > 0) mk_storage_seek_file(g_open_file, g_stream_offset);
    mp3_decoder_reset();
    if (g_stream_offset > 0) {
        g_decode_input_offset = g_stream_offset;
    }
    return 0;
}

static void mp3_reset_play_cursor(void) {
    g_player.current_position = 0;
    g_stream_offset = 0;
    g_synth_phase = 0;
    if (g_playlist.count > 0 && g_playlist.current < g_playlist.count) {
        g_playlist.entries[g_playlist.current].position = 0;
    }
    if (g_open_file) {
        mk_storage_seek_file(g_open_file, 0);
    }
    mp3_decoder_reset();
    mp3_audio_queue_reset();
}

static void mp3_audio_queue_reset(void) {
    g_audio_queue_head = 0;
    g_audio_queue_tail = 0;
    g_audio_queue_count = 0;
}

static void mp3_decoder_reset(void) {
    mp3dec_init(&g_mp3_decoder);
    g_decode_input_offset = 0;
    g_decode_input_filled = 0;
    g_decode_eof = false;
}

static void mp3_decoder_consume(size_t bytes) {
    if (bytes == 0 || g_decode_input_filled == 0) return;
    if (bytes >= g_decode_input_filled) {
        g_decode_input_offset += g_decode_input_filled;
        g_decode_input_filled = 0;
        return;
    }
    size_t remaining = g_decode_input_filled - bytes;
    for (size_t i = 0; i < remaining; ++i) {
        g_decode_input[i] = g_decode_input[bytes + i];
    }
    g_decode_input_offset += bytes;
    g_decode_input_filled = remaining;
}

static bool mp3_decoder_refill(void) {
    if (!g_open_file || g_decode_eof) return g_decode_input_filled > 0;
    if (g_decode_input_filled >= MP3_DECODE_INPUT_SIZE) return true;
    int read = mk_storage_read_file(g_open_file, g_decode_input + g_decode_input_filled,
                                    g_decode_input_offset + g_decode_input_filled,
                                    MP3_DECODE_INPUT_SIZE - g_decode_input_filled);
    if (read < 0) return false;
    if (read == 0) {
        g_decode_eof = true;
        return g_decode_input_filled > 0;
    }
    g_decode_input_filled += (size_t)read;
    return true;
}

static void mp3_audio_queue_push(const int64_t* samples, size_t count) {
    if (!samples || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        if (g_audio_queue_count >= MP3_AUDIO_QUEUE_SAMPLES) break;
        g_audio_queue[g_audio_queue_tail] = samples[i];
        g_audio_queue_tail = (g_audio_queue_tail + 1) % MP3_AUDIO_QUEUE_SAMPLES;
        ++g_audio_queue_count;
    }
}

static void mp3_audio_queue_consume(size_t count) {
    if (count == 0 || g_audio_queue_count == 0) return;
    if (count >= g_audio_queue_count) {
        g_audio_queue_head = 0;
        g_audio_queue_tail = 0;
        g_audio_queue_count = 0;
        return;
    }
    g_audio_queue_head = (g_audio_queue_head + count) % MP3_AUDIO_QUEUE_SAMPLES;
    g_audio_queue_count -= count;
}

static int mp3_decode_next_frame(void) {
    if (!g_open_file) return -1;

    for (int attempts = 0; attempts < 32; ++attempts) {
        if (g_decode_input_filled < 4 && !g_decode_eof) {
            if (!mp3_decoder_refill()) return -1;
        }
        if (g_decode_input_filled == 0) return 0;

        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&g_mp3_decoder, g_decode_input, (int)g_decode_input_filled, pcm, &info);

        if (info.frame_bytes > 0) {
            mp3_decoder_consume((size_t)info.frame_bytes);
        } else if (!g_decode_eof) {
            if (!mp3_decoder_refill()) return -1;
        } else if (g_decode_input_filled > 0) {
            mp3_decoder_consume(1);
        } else {
            return 0;
        }

        if (samples > 0) {
            if (info.hz > 0) g_player.sample_rate = (u64)info.hz;
            if (info.channels > 0) g_player.channels = (u64)info.channels;
            g_audio_sample_rate = g_player.sample_rate ? g_player.sample_rate : 44100ULL;
            g_audio_channels = g_player.channels ? g_player.channels : 2ULL;
            g_audio_bits_per_sample = 16;
            int64_t converted[MINIMP3_MAX_SAMPLES_PER_FRAME];
            for (int i = 0; i < samples; ++i) {
                converted[i] = (int64_t)pcm[i];
            }
            mp3_audio_queue_push(converted, (size_t)samples);
            mk_audio_write_samples(converted, (uint64_t)samples);
            return samples;
        }
    }
    return 0;
}

static void mp3_finish_track_if_needed(void) {
    if (g_repeat_mode == 1) {
        mp3_reset_play_cursor();
        if (g_open_file) mk_storage_seek_file(g_open_file, 0);
        mp3_decoder_reset();
        return;
    }

    if (g_repeat_mode == 2 && g_playlist.count > 0) {
        if (mk_mp3_playlist_play_next() == 0) return;
        g_playlist.current = 0;
        mp3_sync_player_from_current(false);
        if (mp3_open_stream_for_current() == 0) {
            mp3_decoder_reset();
            g_player.state = MK_MP3_STATE_PLAYING;
            return;
        }
    }

    mk_mp3_stop();
}

static void mp3_set_default_track(mk_mp3_playlist_entry_t* entry, const char* filename) {
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));
    mp3_copy_str(entry->filename, sizeof(entry->filename), filename);
    entry->duration = MP3_DEFAULT_DURATION_MS;
    entry->position = 0;
    entry->sample_rate = 44100;
    entry->channels = 2;
    entry->bitrate = 128;
    entry->valid = true;

    char title[128];
    mp3_copy_str(title, sizeof(title), mp3_basename(filename));
    mp3_strip_extension(title);
    if (title[0] == '\0') mp3_copy_str(title, sizeof(title), "Unknown Track");
    mp3_copy_str(entry->title, sizeof(entry->title), title);
    entry->artist[0] = '\0';
    entry->album[0] = '\0';
    entry->year = 0;
}

static void mp3_copy_tag_field(char* dst, size_t dst_size, const u8* src, size_t src_size) {
    if (!dst || dst_size == 0) return;
    size_t n = 0;
    while (n < src_size && n + 1 < dst_size && src[n] != '\0') {
        dst[n] = (char)src[n];
        ++n;
    }
    dst[n] = '\0';
    mp3_trim_inplace(dst);
}

static void mp3_apply_id3v1(mk_storage_file_t* file, mk_mp3_playlist_entry_t* entry) {
    if (!file || !entry || file->size < 128ULL) return;

    u8 tag[128];
    if (mk_storage_read_file(file, tag, file->size - 128ULL, sizeof(tag)) != (int)sizeof(tag)) return;
    if (tag[0] != 'T' || tag[1] != 'A' || tag[2] != 'G') return;

    char tmp[64];
    mp3_copy_tag_field(entry->title, sizeof(entry->title), &tag[3], 30);
    mp3_copy_tag_field(entry->artist, sizeof(entry->artist), &tag[33], 30);
    mp3_copy_tag_field(entry->album, sizeof(entry->album), &tag[63], 30);
    mp3_copy_tag_field(tmp, sizeof(tmp), &tag[93], 4);
    if (tmp[0]) {
        u64 year = 0;
        for (size_t i = 0; tmp[i]; ++i) {
            if (tmp[i] < '0' || tmp[i] > '9') { year = 0; break; }
            year = year * 10ULL + (u64)(tmp[i] - '0');
        }
        entry->year = year;
    }

    if (entry->title[0] == '\0') {
        char title[128];
        mp3_copy_str(title, sizeof(title), mp3_basename(entry->filename));
        mp3_strip_extension(title);
        mp3_copy_str(entry->title, sizeof(entry->title), title[0] ? title : "Unknown Track");
    }
}

static bool mp3_parse_mpeg_header(const u8 hdr[4], u64* sample_rate, u64* channels, u64* bitrate) {
    static const u64 br_mpeg1_layer3[16]  = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const u64 br_mpeg2_layer3[16]  = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const u64 sr_mpeg1[4]          = {44100, 48000, 32000, 0};
    static const u64 sr_mpeg2[4]          = {22050, 24000, 16000, 0};
    static const u64 sr_mpeg25[4]         = {11025, 12000, 8000, 0};

    if (!hdr || !sample_rate || !channels || !bitrate) return false;
    if (hdr[0] != 0xFF || (hdr[1] & 0xE0) != 0xE0) return false;

    u8 version_id = (u8)((hdr[1] >> 3) & 0x3U);
    u8 layer_id   = (u8)((hdr[1] >> 1) & 0x3U);
    u8 br_idx     = (u8)((hdr[2] >> 4) & 0x0F);
    u8 sr_idx     = (u8)((hdr[2] >> 2) & 0x03);
    u8 ch_mode    = (u8)((hdr[3] >> 6) & 0x03);

    if (layer_id != 1 || version_id == 1) return false; /* Layer III only, reserved version invalid */

    if (version_id == 3) {
        *sample_rate = sr_mpeg1[sr_idx];
        *bitrate = br_mpeg1_layer3[br_idx];
    } else if (version_id == 2) {
        *sample_rate = sr_mpeg2[sr_idx];
        *bitrate = br_mpeg2_layer3[br_idx];
    } else if (version_id == 0) {
        *sample_rate = sr_mpeg25[sr_idx];
        *bitrate = br_mpeg2_layer3[br_idx];
    } else {
        return false;
    }

    if (*sample_rate == 0 || *bitrate == 0) return false;
    *channels = (ch_mode == 3) ? 1 : 2;
    return true;
}

static void mp3_probe_file(mk_storage_file_t* file, mk_mp3_playlist_entry_t* entry) {
    if (!file || !entry) return;

    u64 sample_rate = entry->sample_rate ? entry->sample_rate : 44100ULL;
    u64 channels = entry->channels ? entry->channels : 2ULL;
    u64 bitrate = entry->bitrate ? entry->bitrate : 128ULL;

    u8 hdr[4];
    if (mk_storage_read_file(file, hdr, 0, sizeof(hdr)) == (int)sizeof(hdr)) {
        u64 parsed_sr = 0, parsed_ch = 0, parsed_br = 0;
        if (mp3_parse_mpeg_header(hdr, &parsed_sr, &parsed_ch, &parsed_br)) {
            sample_rate = parsed_sr;
            channels = parsed_ch;
            bitrate = parsed_br;
        }
    }

    if (mp3_has_suffix_ci(entry->filename, ".flac")) {
        sample_rate = 48000;
        channels = 2;
        bitrate = 320;
    } else if (mp3_has_suffix_ci(entry->filename, ".ogg")) {
        sample_rate = 44100;
        channels = 2;
        bitrate = 192;
    } else if (mp3_has_suffix_ci(entry->filename, ".wav")) {
        sample_rate = 44100;
        channels = 2;
        bitrate = 1411;
    }

    entry->sample_rate = sample_rate;
    entry->channels = channels;
    entry->bitrate = bitrate;
    if (entry->duration == 0 && file->size > 0 && bitrate > 0) {
        entry->duration = (file->size * 8ULL) / bitrate;
        if (entry->duration < 1000ULL) entry->duration = 1000ULL;
    }
    if (entry->duration == 0) entry->duration = MP3_DEFAULT_DURATION_MS;
}

static void mp3_rebuild_play_order(void) {
    g_play_order_count = (g_playlist.count > MP3_MAX_ORDER) ? MP3_MAX_ORDER : g_playlist.count;
    if (g_play_order_count == 0) {
        g_play_order_pos = 0;
        return;
    }

    for (u64 i = 0; i < g_play_order_count; ++i) g_play_order[i] = i;

    if (g_playlist.shuffle && g_play_order_count > 1) {
        u64 seed = get_timer_ticks() ^ (g_play_order_count << 32) ^ 0x9E3779B97F4A7C15ULL;
        for (u64 i = g_play_order_count - 1; i > 0; --i) {
            seed = seed * 6364136223846793005ULL + 1ULL;
            u64 j = seed % (i + 1ULL);
            u64 tmp = g_play_order[i];
            g_play_order[i] = g_play_order[j];
            g_play_order[j] = tmp;
        }
    }

    g_play_order_pos = 0;
    for (u64 i = 0; i < g_play_order_count; ++i) {
        if (g_play_order[i] == g_playlist.current) {
            g_play_order_pos = i;
            break;
        }
    }
}

static void mp3_reset_state_for_empty_playlist(void) {
    memset(&g_player, 0, sizeof(g_player));
    g_player.magic = MK_MP3_MAGIC;
    g_player.state = MK_MP3_STATE_STOPPED;
    g_player.volume = MP3_DEFAULT_VOLUME;
    g_player.balance = MP3_DEFAULT_BALANCE;
    g_player.sample_rate = 44100;
    g_player.channels = 2;
    g_player.bitrate = 128;
    g_player.repeat = false;
    g_player.shuffle = false;
    g_player.current_title[0] = '\0';
    g_player.current_artist[0] = '\0';
    g_player.current_album[0] = '\0';
    g_player.current_year = 0;
    g_player.current_file = NULL;
    g_player.current_position = 0;
    g_player.total_duration = 0;
}

static void mp3_sync_player_from_current(bool preserve_state) {
    if (g_playlist.count == 0 || g_playlist.current >= g_playlist.count) {
        mp3_reset_state_for_empty_playlist();
        return;
    }

    mk_mp3_playlist_entry_t* cur = &g_playlist.entries[g_playlist.current];
    if (!preserve_state || g_player.magic != MK_MP3_MAGIC) {
        g_player.magic = MK_MP3_MAGIC;
        if (g_player.volume == 0) g_player.volume = MP3_DEFAULT_VOLUME;
        if (g_player.balance == 0) g_player.balance = MP3_DEFAULT_BALANCE;
    }
    g_player.current_file = NULL;
    g_player.current_position = cur->position;
    g_player.total_duration = cur->duration;
    g_player.sample_rate = cur->sample_rate ? cur->sample_rate : 44100ULL;
    g_player.channels = cur->channels ? cur->channels : 2ULL;
    g_player.bitrate = cur->bitrate ? cur->bitrate : 128ULL;
    g_player.repeat = (g_repeat_mode != 0);
    g_player.shuffle = g_playlist.shuffle;
    g_player.state = preserve_state ? g_player.state : MK_MP3_STATE_STOPPED;
    mp3_copy_str(g_current_path, sizeof(g_current_path), cur->filename);
    mp3_copy_str(g_player.current_title, sizeof(g_player.current_title), cur->title[0] ? cur->title : mp3_basename(cur->filename));
    mp3_copy_str(g_player.current_artist, sizeof(g_player.current_artist), cur->artist);
    mp3_copy_str(g_player.current_album, sizeof(g_player.current_album), cur->album);
    g_player.current_year = cur->year;
}

static void mp3_sync_playlist_state(void) {
    if (g_playlist.count == 0) {
        mp3_reset_state_for_empty_playlist();
        return;
    }
    if (g_playlist.current >= g_playlist.count) g_playlist.current = 0;
    g_playlist.repeat = (g_repeat_mode != 0);
    g_player.repeat = g_playlist.repeat;
    g_player.shuffle = g_playlist.shuffle;
    mp3_sync_player_from_current(true);
}

static int mp3_find_index_by_name(const char* filename) {
    if (!filename) return -1;
    for (u64 i = 0; i < g_playlist.count; ++i) {
        if (strcmp(g_playlist.entries[i].filename, filename) == 0) return (int)i;
    }
    return -1;
}

static int mp3_append_track(const char* filename) {
    if (g_playlist.count >= (sizeof(g_playlist.entries) / sizeof(g_playlist.entries[0]))) return -1;
    u64 idx = g_playlist.count++;
    mk_mp3_playlist_entry_t* entry = &g_playlist.entries[idx];
    mp3_set_default_track(entry, filename);

    mk_storage_file_t* file = mk_storage_open_file(filename);
    if (file) {
        mp3_apply_id3v1(file, entry);
        mp3_probe_file(file, entry);
        mk_storage_close_file(file);
    }
    return (int)idx;
}

static int mp3_load_path_into_playlist(const char* filename) {
    if (!filename || !filename[0]) return -1;
    if (!mp3_is_audio_file(filename)) return -1;

    int idx = mp3_find_index_by_name(filename);
    if (idx < 0) {
        idx = mp3_append_track(filename);
        if (idx < 0) return -1;
    }

    g_playlist.current = (u64)idx;
    g_playlist.entries[idx].position = 0;
    g_playlist.entries[idx].valid = true;
    g_player.state = MK_MP3_STATE_STOPPED;
    mp3_sync_player_from_current(false);
    mp3_rebuild_play_order();
    g_last_tick = get_timer_ticks();

    serial_puts("[MP3] loaded: ");
    serial_puts(filename);
    serial_puts("\n");
    return 0;
}

static int mp3_current_order_index(void) {
    if (g_play_order_count == 0 || g_playlist.count == 0) return -1;
    if (g_play_order_pos >= g_play_order_count) g_play_order_pos = 0;
    u64 index = g_play_order[g_play_order_pos];
    if (index >= g_playlist.count) return -1;
    return (int)index;
}

static int mp3_move_order(int delta) {
    if (g_play_order_count == 0) return -1;
    if (delta == 0) return mp3_current_order_index();

    if (delta > 0) {
        if (g_play_order_pos + 1 < g_play_order_count) {
            ++g_play_order_pos;
        } else if (g_repeat_mode == 2) {
            g_play_order_pos = 0;
        } else {
            return -1;
        }
    } else {
        if (g_play_order_pos > 0) {
            --g_play_order_pos;
        } else if (g_repeat_mode == 2) {
            g_play_order_pos = g_play_order_count - 1;
        } else {
            return -1;
        }
    }

    int idx = mp3_current_order_index();
    if (idx < 0) return -1;
    g_playlist.current = (u64)idx;
    g_playlist.entries[idx].position = 0;
    mp3_sync_player_from_current(false);
    g_last_tick = get_timer_ticks();
    return idx;
}

static void mp3_advance_time(void) {
    if (!g_initialized) return;
    if (g_player.state != MK_MP3_STATE_PLAYING) {
        g_last_tick = get_timer_ticks();
        return;
    }

    u64 now = get_timer_ticks();
    if (now <= g_last_tick) return;

    u64 elapsed_ms = (now - g_last_tick); /* timer.c runs at 1000Hz: 1 tick == 1 ms */
    g_last_tick = now;

    if (g_audio_sample_rate > 0 && g_audio_channels > 0) {
        u64 drain = (elapsed_ms * g_audio_sample_rate * g_audio_channels) / 1000ULL;
        if (drain > 0) mp3_audio_queue_consume((size_t)drain);
    }

    if (g_player.current_position + elapsed_ms >= g_player.total_duration) {
        if (g_repeat_mode == 1) {
            g_player.current_position = 0;
            g_playlist.entries[g_playlist.current].position = 0;
            return;
        }

        int next_idx = mp3_move_order(+1);
        if (next_idx >= 0) {
            g_player.state = MK_MP3_STATE_PLAYING;
            g_player.current_position = 0;
            g_playlist.entries[next_idx].position = 0;
            mp3_decoder_reset();
            if (g_open_file) mk_storage_seek_file(g_open_file, 0);
            return;
        }

        g_player.state = MK_MP3_STATE_STOPPED;
        g_player.current_position = 0;
        g_playlist.entries[g_playlist.current].position = 0;
        return;
    }

    g_player.current_position += elapsed_ms;
    g_playlist.entries[g_playlist.current].position = g_player.current_position;
}

void mk_mp3_init(void) {
    memset(&g_player, 0, sizeof(g_player));
    memset(&g_playlist, 0, sizeof(g_playlist));
    memset(&g_equalizer, 0, sizeof(g_equalizer));
    g_player.magic = MK_MP3_MAGIC;
    g_player.state = MK_MP3_STATE_STOPPED;
    g_player.volume = MP3_DEFAULT_VOLUME;
    g_player.balance = MP3_DEFAULT_BALANCE;
    g_player.sample_rate = 44100;
    g_player.channels = 2;
    g_player.bitrate = 128;
    g_player.repeat = false;
    g_player.shuffle = false;
    g_player.current_title[0] = '\0';
    g_player.current_artist[0] = '\0';
    g_player.current_album[0] = '\0';
    g_player.current_year = 0;
    g_playlist.repeat = false;
    g_playlist.shuffle = false;
    g_repeat_mode = 0;
    g_initialized = true;
    g_last_tick = get_timer_ticks();
    g_current_path[0] = '\0';
    serial_puts("[MP3] backend initialized\n");
}

int mk_mp3_load_file(const char* filename) {
    if (!g_initialized) mk_mp3_init();
    return mp3_load_path_into_playlist(filename);
}

int mk_mp3_load_directory(const char* dir_path) {
    if (!g_initialized) mk_mp3_init();
    if (!dir_path || !dir_path[0]) return -1;

    fs_entry_t* entries = fs_list_dir(dir_path);
    int total = fs_entry_count_for_path(dir_path);
    if (!entries || total <= 0) return -1;

    int added = 0;
    char full[MK_MP3_MAX_FILENAME_LENGTH];
    for (int i = 0; i < total; ++i) {
        fs_entry_t* e = &entries[i];
        if (!e || e->is_dir || !e->name[0]) continue;
        if (!mp3_is_audio_file(e->name)) continue;

        mp3_join_path(full, sizeof(full), dir_path, e->name);
        if (mp3_load_path_into_playlist(full) == 0) {
            ++added;
        }
    }
    if (added == 0) return -1;
    g_playlist.current = 0;
    mp3_sync_player_from_current(false);
    mp3_rebuild_play_order();
    serial_puts("[MP3] directory loaded\n");
    return 0;
}

int mk_mp3_clear_playlist(void) {
    if (!g_initialized) mk_mp3_init();
    memset(&g_playlist, 0, sizeof(g_playlist));
    g_repeat_mode = 0;
    g_player.repeat = false;
    g_player.shuffle = false;
    mp3_reset_state_for_empty_playlist();
    g_current_path[0] = '\0';
    g_play_order_count = 0;
    g_play_order_pos = 0;
    g_last_tick = get_timer_ticks();
    serial_puts("[MP3] playlist cleared\n");
    return 0;
}

int mk_mp3_play(void) {
    if (!g_initialized) mk_mp3_init();
    if (g_playlist.count == 0) return -1;

    if (g_playlist.current >= g_playlist.count) g_playlist.current = 0;
    g_playlist.entries[g_playlist.current].position = g_player.current_position;
    mp3_sync_player_from_current(true);
    if (!g_open_file && mp3_open_stream_for_current() != 0) {
        serial_puts("[MP3] unable to open stream\n");
        return -1;
    }
    mp3_audio_queue_reset();
    mk_audio_start_playback(g_player.sample_rate ? g_player.sample_rate : 44100ULL,
                            g_player.channels ? g_player.channels : 2ULL,
                            16);
    g_player.state = MK_MP3_STATE_PLAYING;
    g_last_tick = get_timer_ticks();
    serial_puts("[MP3] play\n");
    return 0;
}

int mk_mp3_pause(void) {
    if (!g_initialized) return -1;
    if (g_player.state != MK_MP3_STATE_PLAYING) return -1;
    mp3_advance_time();
    g_player.state = MK_MP3_STATE_PAUSED;
    serial_puts("[MP3] pause\n");
    return 0;
}

int mk_mp3_resume(void) {
    if (!g_initialized) return -1;
    if (g_player.state != MK_MP3_STATE_PAUSED) return -1;
    g_player.state = MK_MP3_STATE_PLAYING;
    g_last_tick = get_timer_ticks();
    serial_puts("[MP3] resume\n");
    return 0;
}

int mk_mp3_stop(void) {
    if (!g_initialized) return -1;
    g_player.state = MK_MP3_STATE_STOPPED;
    mk_audio_stop_playback();
    mp3_decoder_reset();
    mp3_close_stream();
    g_player.current_position = 0;
    if (g_playlist.count > 0 && g_playlist.current < g_playlist.count) {
        g_playlist.entries[g_playlist.current].position = 0;
    }
    g_last_tick = get_timer_ticks();
    serial_puts("[MP3] stop\n");
    return 0;
}

int mk_mp3_set_volume(uint64_t volume) {
    if (!g_initialized) mk_mp3_init();
    if (volume > 100) volume = 100;
    g_player.volume = volume;
    mk_audio_set_volume(volume);
    serial_puts("[MP3] volume=");
    serial_putdec(volume);
    serial_puts("\n");
    return 0;
}

int mk_mp3_seek(uint64_t position_ms) {
    if (!g_initialized) return -1;
    if (g_playlist.count == 0) return -1;

    u64 dur = g_player.total_duration ? g_player.total_duration : MP3_DEFAULT_DURATION_MS;
    if (position_ms > dur) position_ms = dur;

    g_player.current_position = position_ms;
    g_playlist.entries[g_playlist.current].position = position_ms;
    g_stream_offset = (position_ms * (g_player.bitrate ? g_player.bitrate : 128ULL)) / 8ULL;
    if (g_open_file && g_stream_offset <= g_open_file->size) {
        mk_storage_seek_file(g_open_file, g_stream_offset);
    }
    g_last_tick = get_timer_ticks();
    serial_puts("[MP3] seek\n");
    return 0;
}

int mk_mp3_set_repeat(int mode) {
    if (!g_initialized) mk_mp3_init();
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_repeat_mode = mode;
    g_playlist.repeat = (mode != 0);
    g_player.repeat = g_playlist.repeat;
    return 0;
}

int mk_mp3_set_shuffle(bool enabled) {
    if (!g_initialized) mk_mp3_init();
    g_playlist.shuffle = enabled;
    g_player.shuffle = enabled;
    mp3_rebuild_play_order();
    return 0;
}

void mk_mp3_update(void) {
    if (!g_initialized) return;
    mp3_advance_time();

    if (g_player.state != MK_MP3_STATE_PLAYING || g_playlist.count == 0) {
        return;
    }
    if (!g_open_file && mp3_open_stream_for_current() != 0) {
        serial_puts("[MP3] stream unavailable\n");
        mk_mp3_stop();
        return;
    }

    for (int loops = 0; loops < 8; ++loops) {
        if (!g_open_file) break;
        if (mk_audio_buffer_space() < 512) break;

        int decoded = mp3_decode_next_frame();
        if (decoded > 0) {
            continue;
        }

        if (g_decode_eof && g_decode_input_filled == 0) {
            mp3_finish_track_if_needed();
            return;
        }

        if (decoded < 0) {
            serial_puts("[MP3] decode error\n");
            mk_mp3_stop();
            return;
        }

        break;
    }
}

mk_mp3_player_t* mk_mp3_get_player_state(void) {
    mk_mp3_update();
    return &g_player;
}

const char* mk_mp3_get_current_title(void) {
    mk_mp3_update();
    return g_player.current_title[0] ? g_player.current_title : "No track";
}

const char* mk_mp3_get_current_artist(void) {
    mk_mp3_update();
    return g_player.current_artist[0] ? g_player.current_artist : "Unknown artist";
}

const char* mk_mp3_get_current_album(void) {
    mk_mp3_update();
    return g_player.current_album[0] ? g_player.current_album : "Unknown album";
}

uint64_t mk_mp3_get_current_year(void) {
    mk_mp3_update();
    return g_player.current_year;
}

int mk_mp3_playlist_add(const char* filename) {
    return mk_mp3_load_file(filename);
}

int mk_mp3_playlist_remove(uint64_t index) {
    if (!g_initialized || index >= g_playlist.count) return -1;
    for (u64 i = index; i + 1 < g_playlist.count; ++i) {
        g_playlist.entries[i] = g_playlist.entries[i + 1];
    }
    if (g_playlist.count > 0) --g_playlist.count;
    if (g_playlist.count == 0) {
        mp3_reset_state_for_empty_playlist();
        g_playlist.current = 0;
        g_play_order_count = 0;
        g_play_order_pos = 0;
        return 0;
    }
    if (g_playlist.current >= g_playlist.count) g_playlist.current = g_playlist.count - 1;
    mp3_rebuild_play_order();
    mp3_sync_player_from_current(false);
    return 0;
}

int mk_mp3_playlist_play_next(void) {
    if (!g_initialized || g_playlist.count == 0) return -1;
    int idx = mp3_move_order(+1);
    if (idx < 0) {
        if (g_repeat_mode == 2) {
            g_playlist.current = 0;
            mp3_sync_player_from_current(false);
            return mk_mp3_play();
        }
        return mk_mp3_stop();
    }
    g_playlist.current = (u64)idx;
    g_player.current_position = 0;
    g_playlist.entries[g_playlist.current].position = 0;
    mp3_sync_player_from_current(false);
    return mk_mp3_play();
}

int mk_mp3_playlist_play_previous(void) {
    if (!g_initialized || g_playlist.count == 0) return -1;
    int idx = mp3_move_order(-1);
    if (idx < 0) {
        if (g_repeat_mode == 2) {
            g_playlist.current = g_playlist.count - 1;
            mp3_sync_player_from_current(false);
            return mk_mp3_play();
        }
        return -1;
    }
    g_playlist.current = (u64)idx;
    g_player.current_position = 0;
    g_playlist.entries[g_playlist.current].position = 0;
    mp3_sync_player_from_current(false);
    return mk_mp3_play();
}

mk_mp3_playlist_t* mk_mp3_get_playlist(void) {
    return &g_playlist;
}

mk_mp3_equalizer_t* mk_mp3_get_equalizer(void) {
    return &g_equalizer;
}

void mk_mp3_set_equalizer(const mk_mp3_equalizer_t* eq) {
    if (!eq) return;
    g_equalizer = *eq;
}

bool mk_mp3_is_valid_file(mk_storage_file_t* file) {
    if (!file) return false;
    return mp3_is_audio_file(file->name) || mp3_has_suffix_ci(file->extension, "mp3") ||
           mp3_has_suffix_ci(file->extension, "wav") || mp3_has_suffix_ci(file->extension, "ogg") ||
           mp3_has_suffix_ci(file->extension, "flac");
}

uint64_t mk_mp3_get_bitrate(mk_storage_file_t* file) {
    if (!file) return 128;
    mk_mp3_playlist_entry_t tmp;
    mp3_set_default_track(&tmp, file->name);
    mp3_probe_file(file, &tmp);
    return tmp.bitrate ? tmp.bitrate : 128;
}

uint64_t mk_mp3_get_sample_rate(mk_storage_file_t* file) {
    if (!file) return 44100;
    mk_mp3_playlist_entry_t tmp;
    mp3_set_default_track(&tmp, file->name);
    mp3_probe_file(file, &tmp);
    return tmp.sample_rate ? tmp.sample_rate : 44100;
}

uint64_t mk_mp3_get_channels(mk_storage_file_t* file) {
    if (!file) return 2;
    mk_mp3_playlist_entry_t tmp;
    mp3_set_default_track(&tmp, file->name);
    mp3_probe_file(file, &tmp);
    return tmp.channels ? tmp.channels : 2;
}

uint64_t mk_mp3_get_duration(mk_storage_file_t* file) {
    if (!file) return MP3_DEFAULT_DURATION_MS;
    mk_mp3_playlist_entry_t tmp;
    mp3_set_default_track(&tmp, file->name);
    mp3_probe_file(file, &tmp);
    return tmp.duration ? tmp.duration : MP3_DEFAULT_DURATION_MS;
}

void mk_mp3_server_main(void) {
    serial_puts("[MP3] server ready\n");
}

/* Audio shims: kept for compatibility with code that expects them to exist. */
void mk_audio_start_playback(uint64_t sample_rate, uint64_t channels, uint64_t bits_per_sample) {
    g_audio_sample_rate = sample_rate ? sample_rate : 44100ULL;
    g_audio_channels = channels ? channels : 2ULL;
    g_audio_bits_per_sample = bits_per_sample ? bits_per_sample : 16ULL;
    mp3_audio_queue_reset();
    if (ac97_is_available()) {
        ac97_stop();
        ac97_configure((uint32_t)g_audio_sample_rate, (uint32_t)g_audio_channels);
        ac97_set_volume((uint32_t)g_player.volume);
    }
}

void mk_audio_stop_playback(void) {
    mp3_audio_queue_reset();
    mp3_close_stream();
    if (ac97_is_available()) {
        ac97_stop();
    }
}

void mk_audio_write_samples(const int64_t* samples, uint64_t size) {
    if (!samples || size == 0) return;
    mp3_audio_queue_push(samples, (size_t)size);

    if (!ac97_is_available()) return;

    /* AC97 PCM-out expects stereo interleaved 16-bit samples. mp3d
     * output is already interleaved per-channel, but mono tracks need
     * to be duplicated to both channels since the hardware path here
     * is fixed stereo (see ac97_configure()). */
    int16_t stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    uint64_t out_count;
    if (g_audio_channels >= 2) {
        out_count = size;
        if (out_count > MINIMP3_MAX_SAMPLES_PER_FRAME * 2) out_count = MINIMP3_MAX_SAMPLES_PER_FRAME * 2;
        for (uint64_t i = 0; i < out_count; ++i) {
            stereo[i] = (int16_t)samples[i];
        }
    } else {
        uint64_t frames = size;
        if (frames > MINIMP3_MAX_SAMPLES_PER_FRAME) frames = MINIMP3_MAX_SAMPLES_PER_FRAME;
        out_count = frames * 2;
        for (uint64_t i = 0; i < frames; ++i) {
            int16_t s = (int16_t)samples[i];
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }
    }

    uint64_t written = 0;
    while (written < out_count) {
        uint64_t n = ac97_write_samples(stereo + written, out_count - written);
        if (n == 0) break; /* ring momentarily full; drop rather than block the decoder */
        written += n;
    }
}

uint64_t mk_audio_buffer_space(void) {
    return (g_audio_queue_count < MP3_AUDIO_QUEUE_SAMPLES) ? (MP3_AUDIO_QUEUE_SAMPLES - g_audio_queue_count) : 0;
}

void mk_audio_set_volume(uint64_t volume) {
    if (volume > 100) volume = 100;
    g_player.volume = volume;
    if (ac97_is_available()) {
        ac97_set_volume((uint32_t)volume);
    }
}
