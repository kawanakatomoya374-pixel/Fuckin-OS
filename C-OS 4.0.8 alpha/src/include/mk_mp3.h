#ifndef MK_MP3_H
#define MK_MP3_H

#include "types.h"
#include "mk_core.h"
#include "mk_ipc.h"

typedef struct mk_storage_file mk_storage_file_t;

// MP3 constants
#define MK_MP3_MAGIC 0x4D50335F  // "MP3_"
#define MK_MP3_SERVER_PID 12
#define MK_MP3_MAX_FILENAME_LENGTH 256

// MP3 player states
#define MK_MP3_STATE_STOPPED 0
#define MK_MP3_STATE_PLAYING 1
#define MK_MP3_STATE_PAUSED 2
#define MK_MP3_STATE_ERROR 3

// MP3 decoder states
#define MK_MP3_DECODER_STATE_IDLE 0
#define MK_MP3_DECODER_STATE_LOADING 1
#define MK_MP3_DECODER_STATE_READY 2
#define MK_MP3_DECODER_STATE_DECODING 3
#define MK_MP3_DECODER_STATE_ERROR 4

// Message types for MP3 server
#define MK_MP3_MSG_LOAD 1
#define MK_MP3_MSG_PLAY 2
#define MK_MP3_MSG_PAUSE 3
#define MK_MP3_MSG_RESUME 4
#define MK_MP3_MSG_STOP 5
#define MK_MP3_MSG_SET_VOLUME 6
#define MK_MP3_MSG_SEEK 7
#define MK_MP3_MSG_GET_STATE 8
#define MK_MP3_MSG_RESPONSE 9
#define MK_MP3_MSG_STATE 10

// Forward declarations
typedef struct mk_mp3_player mk_mp3_player_t;
typedef struct mk_mp3_request mk_mp3_request_t;
typedef struct mk_mp3_response mk_mp3_response_t;

// MP3 header structure
typedef struct {
    uint8_t sync[4];
    uint64_t version;
    uint64_t layer;
    uint64_t protection;
    uint64_t bitrate;
    uint64_t sample_rate;
    uint64_t padding;
    uint64_t channel_mode;
    uint64_t mode_extension;
    uint64_t copyright;
    uint64_t original;
    uint64_t emphasis;
} mp3_header_t;

// MP3 player structure
struct mk_mp3_player {
    uint64_t magic;
    uint64_t state;
    mk_storage_file_t* current_file;
    uint64_t current_position;
    uint64_t total_duration;
    uint64_t volume;
    uint64_t balance;
    uint64_t sample_rate;
    uint64_t channels;
    uint64_t bitrate;
    bool repeat;
    bool shuffle;
    char current_title[128];
    char current_artist[64];
    char current_album[64];
    uint64_t current_year;
};

// MP3 request structure
struct mk_mp3_request {
    char filename[MK_MP3_MAX_FILENAME_LENGTH];
    uint64_t volume;
    uint64_t position;
    uint64_t flags;
};

// MP3 response structure
struct mk_mp3_response {
    bool success;
    uint64_t duration;
    uint64_t sample_rate;
    uint64_t channels;
    uint64_t bitrate;
    uint64_t position;
    uint64_t error_code;
};

// MP3 functions
void mk_mp3_init(void);
int mk_mp3_load_file(const char* filename);
int mk_mp3_load_directory(const char* dir_path);
int mk_mp3_clear_playlist(void);
int mk_mp3_play(void);
int mk_mp3_pause(void);
int mk_mp3_resume(void);
int mk_mp3_stop(void);
int mk_mp3_set_volume(uint64_t volume);
int mk_mp3_seek(uint64_t position_ms);
int mk_mp3_set_repeat(int mode);
int mk_mp3_set_shuffle(bool enabled);
int mk_mp3_playlist_play_next(void);
int mk_mp3_playlist_play_previous(void);
void mk_mp3_update(void);

// MP3 player state
mk_mp3_player_t* mk_mp3_get_player_state(void);
const char* mk_mp3_get_current_title(void);
const char* mk_mp3_get_current_artist(void);
const char* mk_mp3_get_current_album(void);
uint64_t mk_mp3_get_current_year(void);

// MP3 server functions
void mk_mp3_server_main(void);

// MP3 audio interface
void mk_audio_start_playback(uint64_t sample_rate, uint64_t channels, uint64_t bits_per_sample);
void mk_audio_stop_playback(void);
void mk_audio_write_samples(const int64_t* samples, uint64_t size);
uint64_t mk_audio_buffer_space(void);
void mk_audio_set_volume(uint64_t volume);

// MP3 utilities
bool mk_mp3_is_valid_file(mk_storage_file_t* file);
uint64_t mk_mp3_get_duration(mk_storage_file_t* file);
uint64_t mk_mp3_get_bitrate(mk_storage_file_t* file);
uint64_t mk_mp3_get_sample_rate(mk_storage_file_t* file);
uint64_t mk_mp3_get_channels(mk_storage_file_t* file);

// MP3 equalizer
typedef struct {
    uint64_t bands[10]; // 10-band equalizer
    bool enabled;
} mk_mp3_equalizer_t;

void mk_mp3_set_equalizer(const mk_mp3_equalizer_t* eq);
mk_mp3_equalizer_t* mk_mp3_get_equalizer(void);

// MP3 playlist
typedef struct {
    char filename[MK_MP3_MAX_FILENAME_LENGTH];
    uint64_t duration;
    uint64_t position;
    char title[128];
    char artist[64];
    char album[64];
    uint64_t year;
    uint64_t sample_rate;
    uint64_t channels;
    uint64_t bitrate;
    bool valid;
} mk_mp3_playlist_entry_t;

typedef struct {
    mk_mp3_playlist_entry_t entries[256];
    uint64_t count;
    uint64_t current;
    bool repeat;
    bool shuffle;
} mk_mp3_playlist_t;

int mk_mp3_playlist_add(const char* filename);
int mk_mp3_playlist_remove(uint64_t index);
int mk_mp3_playlist_play_next(void);
int mk_mp3_playlist_play_previous(void);
mk_mp3_playlist_t* mk_mp3_get_playlist(void);
#endif // MK_MP3_H
