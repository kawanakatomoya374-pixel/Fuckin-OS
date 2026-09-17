#ifndef MK_JPG_H
#define MK_JPG_H

#include "types.h"
#include "mk_core.h"
#include "mk_ipc.h"

// JPEG constants
#define MK_JPG_MAGIC 0x4A50475F  // "JPG_"
#define MK_JPG_SERVER_PID 13
#define MK_JPG_MAX_FILENAME_LENGTH 256

// JPEG player states
#define MK_JPG_STATE_IDLE 0
#define MK_JPG_STATE_LOADING 1
#define MK_JPG_STATE_DECODED 2
#define MK_JPG_STATE_DECODING 3
#define MK_JPG_STATE_ERROR 4

// JPEG output formats
#define MK_JPG_FORMAT_RGB24 0
#define MK_JPG_FORMAT_RGBA32 1
#define MK_JPG_FORMAT_GRAYSCALE 2

// Message types for JPEG server
#define MK_JPG_MSG_LOAD 1
#define MK_JPG_MSG_DECODE 2
#define MK_JPG_MSG_CONVERT 3
#define MK_JPG_MSG_GET_INFO 4
#define MK_JPG_MSG_RESPONSE 5
#define MK_JPG_MSG_INFO 6

// Forward declarations
typedef struct mk_jpg_player mk_jpg_player_t;
typedef struct mk_jpg_request mk_jpg_request_t;
typedef struct mk_jpg_response mk_jpg_response_t;
typedef struct mk_jpg_info mk_jpg_info_t;

// JPEG header structure
typedef struct {
    uint8_t soi[2];
    uint64_t version;
    uint64_t density_unit;
    uint64_t x_density;
    uint64_t y_density;
    uint64_t thumb_width;
    uint64_t thumb_height;
} jpg_header_t;

// JPEG component structure
typedef struct {
    uint64_t id;
    uint64_t sampling_factor;
    uint64_t h_sampling;
    uint64_t v_sampling;
    uint64_t quant_table_id;
} jpeg_component_t;

// JPEG Huffman table structure
typedef struct {
    uint64_t codes[256];
    uint64_t code_lengths[256];
    uint64_t code_count;
    uint64_t table_class;
    uint64_t table_id;
} jpeg_huffman_table_t;

// JPEG quantization table structure
typedef struct {
    uint8_t values[64];
    uint64_t precision;
    uint64_t table_id;
} jpeg_quant_table_t;

// JPEG player structure
struct mk_jpg_player {
    uint64_t magic;
    uint64_t state;
    mk_storage_file_t* current_file;
    uint64_t width;
    uint64_t height;
    uint64_t components;
    uint64_t precision;
    bool progressive;
    uint8_t* output_buffer;
    uint64_t output_format;
    uint64_t output_stride;
};

// JPEG request structure
struct mk_jpg_request {
    char filename[MK_JPG_MAX_FILENAME_LENGTH];
    uint64_t output_format;
    uint8_t* output_buffer;
    uint64_t flags;
};

// JPEG response structure
struct mk_jpg_response {
    bool success;
    uint64_t width;
    uint64_t height;
    uint64_t components;
    uint64_t error_code;
};

// JPEG info structure
struct mk_jpg_info {
    uint64_t width;
    uint64_t height;
    uint64_t components;
    uint64_t precision;
    bool progressive;
    uint64_t file_size;
    uint64_t compression_type;
    uint64_t color_space;
};

// JPEG functions
void mk_jpg_init(void);
int mk_jpg_load_file(const char* filename);
int mk_jpg_decode(void);
int mk_jpg_convert_to_rgb(uint8_t* rgb_buffer, uint64_t rgb_format);
mk_jpg_info_t* mk_jpg_get_info(void);

// JPEG player state
mk_jpg_player_t* mk_jpg_get_player_state(void);

// JPEG server functions
void mk_jpg_server_main(void);

// JPEG utilities
bool mk_jpg_is_valid_file(mk_storage_file_t* file);
uint64_t mk_jpg_get_width(mk_storage_file_t* file);
uint64_t mk_jpg_get_height(mk_storage_file_t* file);
uint64_t mk_jpg_get_components(mk_storage_file_t* file);
bool mk_jpg_is_progressive(mk_storage_file_t* file);

// JPEG transformations
typedef struct {
    bool flip_horizontal;
    bool flip_vertical;
    bool rotate_90;
    bool rotate_180;
    bool rotate_270;
    uint64_t scale_factor; // 100 = 100%, 50 = 50%, etc.
} mk_jpg_transform_t;

int mk_jpg_apply_transform(const mk_jpg_transform_t* transform);
int mk_jpg_resize(uint64_t new_width, uint64_t new_height);
int mk_jpg_crop(uint64_t x, uint64_t y, uint64_t width, uint64_t height);

// JPEG compression
typedef struct {
    uint64_t quality; // 1-100
    uint64_t smoothing;
    bool optimize_huffman;
    bool progressive;
} mk_jpg_compression_t;

int mk_jpg_compress(const uint8_t* rgb_data, uint64_t width, uint64_t height, 
                   const mk_jpg_compression_t* compression, uint8_t** output_data, 
                   uint64_t* output_size);
int mk_jpg_compress_to_file(const uint8_t* rgb_data, uint64_t width, uint64_t height, 
                           const mk_jpg_compression_t* compression, const char* filename);

// JPEG effects
typedef struct {
    int64_t brightness; // -100 to 100
    int64_t contrast;   // -100 to 100
    int64_t saturation; // -100 to 100
    int64_t hue;        // 0 to 360
    int64_t sharpness;  // 0 to 100
    bool grayscale;
    bool sepia;
} mk_jpg_effects_t;

int mk_jpg_apply_effects(const mk_jpg_effects_t* effects);
int mk_jpg_adjust_brightness(int64_t value);
int mk_jpg_adjust_contrast(int64_t value);
int mk_jpg_adjust_saturation(int64_t value);
int mk_jpg_adjust_hue(int64_t value);
int mk_jpg_adjust_sharpness(int64_t value);
int mk_jpg_convert_to_grayscale(void);
int mk_jpg_apply_sepia_filter(void);

// JPEG metadata
typedef struct {
    char title[256];
    char author[256];
    char description[512];
    char copyright[256];
    char software[256];
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t camera_make[64];
    uint64_t camera_model[64];
    uint64_t iso_speed;
    uint64_t exposure_time;
    uint64_t aperture;
    uint64_t focal_length;
    uint64_t flash_used;
    double gps_latitude;
    double gps_longitude;
    double gps_altitude;
} mk_jpg_metadata_t;

mk_jpg_metadata_t* mk_jpg_get_metadata(void);
int mk_jpg_set_metadata(const mk_jpg_metadata_t* metadata);
int mk_jpg_read_exif_data(void);
int mk_jpg_write_exif_data(void);

// JPEG thumbnails
typedef struct {
    uint8_t* data;
    uint64_t width;
    uint64_t height;
    uint64_t size;
    bool embedded;
} mk_jpg_thumbnail_t;

mk_jpg_thumbnail_t* mk_jpg_get_thumbnail(void);
int mk_jpg_generate_thumbnail(uint64_t max_width, uint64_t max_height);
int mk_jpg_embed_thumbnail(const mk_jpg_thumbnail_t* thumbnail);

// JPEG color space conversion
int mk_jpg_convert_rgb_to_ycbcr(uint8_t* rgb_data, uint8_t* y_data, 
                                 uint8_t* cb_data, uint8_t* cr_data, 
                                 uint64_t width, uint64_t height);
int mk_jpg_convert_ycbcr_to_rgb(uint8_t* y_data, uint8_t* cb_data, 
                                 uint8_t* cr_data, uint8_t* rgb_data, 
                                 uint64_t width, uint64_t height);

// JPEG DCT operations
void mk_jpg_dct_forward(const int64_t* input, int64_t* output);
void mk_jpg_dct_inverse(const int64_t* input, int64_t* output);
void mk_jpg_quantize_block(const int64_t* input, const uint8_t* quant_table, 
                        int64_t* output);
void mk_jpg_dequantize_block(const int64_t* input, const uint8_t* quant_table, 
                          int64_t* output);

// JPEG performance monitoring
typedef struct {
    uint64_t decode_count;
    uint64_t decode_success;
    uint64_t decode_failures;
    uint64_t total_decode_time;
    uint64_t average_decode_time;
    uint64_t peak_memory_usage;
    uint64_t current_memory_usage;
} mk_jpg_stats_t;

mk_jpg_stats_t* mk_jpg_get_stats(void);
void mk_jpg_reset_stats(void);

// JPEG batch operations
typedef struct {
    char input_pattern[256];
    char output_directory[256];
    mk_jpg_transform_t transform;
    mk_jpg_effects_t effects;
    mk_jpg_compression_t compression;
    bool recursive;
} mk_jpg_batch_operation_t;

int mk_jpg_batch_process(const mk_jpg_batch_operation_t* operation);
int mk_jpg_batch_convert(const char* input_dir, const char* output_dir, 
                        uint64_t output_format, uint64_t quality);

// JPEG streaming
typedef struct {
    uint8_t* buffer;
    uint64_t buffer_size;
    uint64_t buffer_position;
    uint64_t total_size;
    bool streaming;
} mk_jpg_stream_t;

mk_jpg_stream_t* mk_jpg_stream_start(mk_storage_file_t* file);
int mk_jpg_stream_read_frame(mk_jpg_stream_t* stream, uint8_t** frame_data, 
                           uint64_t* frame_size);
void mk_jpg_stream_end(mk_jpg_stream_t* stream);

#endif // MK_JPG_H
