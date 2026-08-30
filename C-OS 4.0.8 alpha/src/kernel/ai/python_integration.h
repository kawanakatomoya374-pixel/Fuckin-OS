/*
 * python_integration.h - C-OS 4.0.8 alpha Python AI Integration
 * Provides Python backend integration for advanced AI capabilities
 */

#ifndef PYTHON_INTEGRATION_H
#define PYTHON_INTEGRATION_H

#include "../../include/memory.h"
#include "../../include/string.h"
#include "../../include/serial.h"

/* Python Integration States */
#define PYTHON_STATE_UNAVAILABLE  0
#define PYTHON_STATE_AVAILABLE   1
#define PYTHON_STATE_INITIALIZED 2
#define PYTHON_STATE_ERROR       3

/* Python Script Types */
#define PYTHON_SCRIPT_TEXT_PROCESSOR   0
#define PYTHON_SCRIPT_CODE_GENERATOR   1
#define PYTHON_SCRIPT_SYSTEM_OPTIMIZER 2
#define PYTHON_SCRIPT_IMAGE_ANALYZER   3
#define PYTHON_SCRIPT_NLP_ENGINE       4
#define PYTHON_SCRIPT_PREDICTION_MODEL  5

/* Python Execution Result */
typedef struct {
    int exit_code;
    char stdout[8192];
    char stderr[4096];
    uint64_t execution_time_ms;
    bool success;
    char error_message[256];
} python_result_t;

/* Python Context Structure */
typedef struct {
    uint8_t state;
    char python_path[256];
    char python_version[32];
    char working_directory[256];
    char module_path[256];
    uint64_t memory_limit;
    uint64_t execution_timeout;
    bool debug_mode;
} python_context_t;

/* Python Script Registry */
typedef struct {
    char script_name[64];
    char script_path[256];
    uint8_t script_type;
    char description[256];
    bool loaded;
    uint64_t last_modified;
} python_script_t;

/* Python Integration Functions */
int python_init(void);
int python_shutdown(void);
bool python_is_available(void);
const char* python_get_version(void);

/* Script Management Functions */
int python_load_script(const char* script_path);
int python_unload_script(const char* script_name);
python_script_t* python_find_script(const char* script_name);
int python_list_scripts(char* script_list, size_t list_size);

/* Execution Functions */
int python_execute_script(const char* script_path, const char* input, char* output, size_t output_size);
int python_execute_function(const char* module_name, const char* function_name, 
                        const char* args, char* output, size_t output_size);
int python_execute_code(const char* python_code, char* output, size_t output_size);

/* AI Model Integration Functions */
int python_load_ai_model(const char* model_path);
int python_inference(const char* model_name, const char* input_data, char* output, size_t output_size);
int python_train_model(const char* model_name, const char* training_data_path);
int python_evaluate_model(const char* model_name, const char* test_data_path, char* metrics, size_t metrics_size);

/* Data Processing Functions */
int python_process_json(const char* json_data, char* processed_data, size_t output_size);
int python_process_csv(const char* csv_data, char* processed_data, size_t output_size);
int python_process_text(const char* text_data, char* processed_data, size_t output_size);
int python_process_image(const void* image_data, uint64_t width, uint64_t height, char* analysis, size_t analysis_size);

/* System Integration Functions */
int python_integrate_with_filesystem(void);
int python_integrate_with_gui(void);
int python_integrate_with_network(void);
int python_integrate_with_hardware(void);

/* Performance Monitoring Functions */
python_result_t* python_get_last_result(void);
uint64_t python_get_execution_count(void);
uint64_t python_get_average_execution_time(void);
void python_reset_performance_stats(void);

/* Configuration Functions */
int python_set_memory_limit(uint64_t limit_mb);
int python_set_execution_timeout(uint64_t timeout_seconds);
int python_set_working_directory(const char* directory);
int python_enable_debug_mode(bool enable);

/* Error Handling Functions */
const char* python_get_last_error(void);
void python_clear_error(void);
int python_get_error_code(void);

/* Utility Functions */
int python_validate_script(const char* script_path);
int python_check_syntax(const char* python_code);
char* python_escape_string(const char* input);
int python_compress_output(const char* output, char* compressed, size_t compressed_size);

/* Async Execution Functions */
int python_execute_async(const char* script_path, const char* input);
bool python_is_execution_complete(void);
python_result_t* python_get_async_result(void);
int python_cancel_execution(void);

/* Module Management Functions */
int python_import_module(const char* module_name);
int python_reload_module(const char* module_name);
char* python_list_modules(void);
int python_get_module_info(const char* module_name, char* info, size_t info_size);

/* Security Functions */
int python_validate_script_security(const char* script_path);
int python_set_execution_permissions(uint64_t permissions);
bool python_is_script_allowed(const char* script_path);

#endif /* PYTHON_INTEGRATION_H */
