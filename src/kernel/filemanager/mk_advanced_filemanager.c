/**
 * mk_advanced_filemanager.c - Advanced File Manager
 * Clean, compile-safe implementation for the C-OS demo build.
 */
#include "types.h"
#include "serial.h"
#include "memory.h"
#include "string.h"
#include "mk_advanced_filemanager.h"

#define MK_FILEMANAGER_MAGIC 0x46494C45U  /* "FILE" */
#define MK_MAX_FILE_OPERATIONS 128
#define MK_MAX_FILE_SEARCHES   64
#define MK_MAX_FILE_BOOKMARKS   64
#define MK_MAX_FILE_TAGS       128
#define MK_MAX_FILE_VIEWS       32
#define MK_MAX_FILE_FILTERS     32

/* File operation types */
enum {
    MK_FILE_OP_COPY = 1,
    MK_FILE_OP_MOVE,
    MK_FILE_OP_DELETE,
    MK_FILE_OP_DELETE_PERMANENT
};

/* File operation status */
enum {
    MK_FILE_OP_STATUS_PENDING = 1,
    MK_FILE_OP_STATUS_RUNNING,
    MK_FILE_OP_STATUS_PAUSED,
    MK_FILE_OP_STATUS_COMPLETED,
    MK_FILE_OP_STATUS_FAILED,
    MK_FILE_OP_STATUS_CANCELLED
};

/* Bookmark types */
enum {
    MK_BOOKMARK_TYPE_FOLDER = 1,
    MK_BOOKMARK_TYPE_FILE,
    MK_BOOKMARK_TYPE_NETWORK,
    MK_BOOKMARK_TYPE_DEVICE
};

/* File type masks */
enum {
    MK_FILE_TYPE_ALL = 0xFFFFFFFFULL,
    MK_FILE_TYPE_IMAGE = 1ULL << 0,
    MK_FILE_TYPE_DOCUMENT = 1ULL << 1,
    MK_FILE_TYPE_AUDIO = 1ULL << 2,
    MK_FILE_TYPE_VIDEO = 1ULL << 3,
    MK_FILE_TYPE_ARCHIVE = 1ULL << 4,
    MK_FILE_TYPE_REGULAR = 1ULL << 5,
    MK_FILE_TYPE_DIRECTORY = 1ULL << 6
};

/* Search status */
enum {
    MK_FILE_SEARCH_STATUS_RUNNING = 1,
    MK_FILE_SEARCH_STATUS_COMPLETED,
    MK_FILE_SEARCH_STATUS_FAILED
};

/* Filter types */
enum {
    MK_FILE_FILTER_PATTERN = 1
};

/* View types */
enum {
    MK_FILE_VIEW_LIST = 1,
    MK_FILE_VIEW_GRID,
    MK_FILE_VIEW_DETAILS
};

/* Sort types */
enum {
    MK_FILE_SORT_NAME = 1,
    MK_FILE_SORT_SIZE,
    MK_FILE_SORT_DATE
};

/* Sort order */
enum {
    MK_FILE_SORT_ASCENDING = 1,
    MK_FILE_SORT_DESCENDING = 2
};

typedef struct {
    uint64_t operation_id;
    uint64_t operation_type;
    char source_path[512];
    char destination_path[512];
    uint64_t file_count;
    uint64_t total_size;
    uint64_t processed_size;
    uint64_t status;
    uint64_t progress;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t error_count;
    char error_message[512];
    bool background;
    bool paused;
    bool cancelled;
    uint64_t priority;
} mk_file_operation_t;

typedef struct {
    uint64_t search_id;
    char search_pattern[256];
    char search_path[512];
    uint64_t file_type_mask;
    bool case_sensitive;
    bool recursive;
    bool active;
    uint64_t status;
    uint64_t result_count;
} mk_file_search_t;

typedef struct {
    uint64_t bookmark_id;
    char name[128];
    char path[512];
    char description[256];
    uint64_t bookmark_type;
    char icon[64];
} mk_file_bookmark_t;

typedef struct {
    uint64_t tag_id;
    char name[64];
    char color[16];
    char description[128];
    uint64_t usage_count;
} mk_file_tag_t;

typedef struct {
    uint64_t view_id;
    char name[64];
    char path[512];
    uint64_t view_type;
    uint64_t sort_type;
    uint64_t sort_order;
    uint64_t item_count;
} mk_file_view_t;

typedef struct {
    uint64_t filter_id;
    char name[64];
    char pattern[256];
    uint64_t filter_type;
    uint64_t file_type_mask;
} mk_file_filter_t;

static mk_file_operation_t file_operations[MK_MAX_FILE_OPERATIONS];
static mk_file_search_t file_searches[MK_MAX_FILE_SEARCHES];
static mk_file_bookmark_t file_bookmarks[MK_MAX_FILE_BOOKMARKS];
static mk_file_tag_t file_tags[MK_MAX_FILE_TAGS];
static mk_file_view_t file_views[MK_MAX_FILE_VIEWS];
static mk_file_filter_t file_filters[MK_MAX_FILE_FILTERS];

static uint64_t file_operation_count = 0;
static uint64_t file_search_count = 0;
static uint64_t file_bookmark_count = 0;
static uint64_t file_tag_count = 0;
static uint64_t file_view_count = 0;
static uint64_t file_filter_count = 0;
static uint64_t filemanager_started = 0;

static void mk_uint_to_string(uint64_t value, char* buffer) {
    if (!buffer) return;
    snprintf(buffer, 32, "%llu", (unsigned long long)value);
}

static uint64_t mk_filemanager_get_timestamp(void) {
    static uint64_t tick = 1;
    return tick++;
}

static void mk_filemanager_log(const char* msg) {
    serial_puts("[FILEMANAGER] ");
    serial_puts(msg ? msg : "(null)");
    serial_puts("\n");
}

static void mk_filemanager_copy_string(char* dst, const char* src, size_t max_len) {
    if (!dst || max_len == 0) return;
    if (!src) src = "";
    strncpy(dst, src, max_len - 1);
    dst[max_len - 1] = '\0';
}

static mk_file_operation_t* mk_filemanager_find_operation(uint64_t operation_id);
static mk_file_search_t* mk_filemanager_find_search(uint64_t search_id);
static mk_file_tag_t* mk_filemanager_find_tag(uint64_t tag_id);
static mk_file_view_t* mk_filemanager_find_view(uint64_t view_id);
static void mk_filemanager_create_default_bookmarks(void);
static void mk_filemanager_create_default_tags(void);
static void mk_filemanager_create_default_filters(void);
static uint64_t mk_filemanager_create_filter(const char* name, const char* pattern, uint64_t file_type_mask);
static void mk_filemanager_execute_operation(mk_file_operation_t* operation);
static void mk_filemanager_execute_search(mk_file_search_t* search);
static void mk_filemanager_scan_directory(mk_file_view_t* view);
static void mk_filemanager_update_operation_progress(mk_file_operation_t* operation);
static void mk_filemanager_update_search_progress(mk_file_search_t* search);
static void mk_filemanager_optimize_operations(void);
static void mk_filemanager_optimize_searches(void);
static void mk_filemanager_optimize_views(void);
static void mk_filemanager_optimize_memory(void);
static void mk_filemanager_calculate_operation_stats(mk_file_operation_t* operation, bool recursive);

void mk_advanced_filemanager_init(void) {
    memset(file_operations, 0, sizeof(file_operations));
    memset(file_searches, 0, sizeof(file_searches));
    memset(file_bookmarks, 0, sizeof(file_bookmarks));
    memset(file_tags, 0, sizeof(file_tags));
    memset(file_views, 0, sizeof(file_views));
    memset(file_filters, 0, sizeof(file_filters));

    file_operation_count = 0;
    file_search_count = 0;
    file_bookmark_count = 0;
    file_tag_count = 0;
    file_view_count = 0;
    file_filter_count = 0;
    filemanager_started = MK_FILEMANAGER_MAGIC;

    mk_filemanager_log("advanced file manager initialized");
    mk_filemanager_create_default_bookmarks();
    mk_filemanager_create_default_tags();
    mk_filemanager_create_default_filters();

    /* Seed a few simple views so the UI has something to show. */
    mk_filemanager_create_view("List", "/", MK_FILE_VIEW_LIST, MK_FILE_SORT_NAME, MK_FILE_SORT_ASCENDING);
    mk_filemanager_create_view("Details", "/", MK_FILE_VIEW_DETAILS, MK_FILE_SORT_NAME, MK_FILE_SORT_ASCENDING);
}

uint64_t mk_filemanager_copy_files(const char* source_path, const char* destination_path, bool background, bool recursive) {
    if (file_operation_count >= MK_MAX_FILE_OPERATIONS) return 0;

    mk_file_operation_t* operation = &file_operations[file_operation_count];
    memset(operation, 0, sizeof(*operation));
    operation->operation_id = file_operation_count + 1;
    operation->operation_type = MK_FILE_OP_COPY;
    mk_filemanager_copy_string(operation->source_path, source_path, sizeof(operation->source_path));
    mk_filemanager_copy_string(operation->destination_path, destination_path, sizeof(operation->destination_path));
    operation->status = MK_FILE_OP_STATUS_PENDING;
    operation->priority = 1;
    operation->background = background;
    operation->start_time = mk_filemanager_get_timestamp();

    mk_filemanager_calculate_operation_stats(operation, recursive);
    file_operation_count++;

    if (!background) {
        mk_filemanager_execute_operation(operation);
    }
    return operation->operation_id;
}

uint64_t mk_filemanager_move_files(const char* source_path, const char* destination_path, bool background, bool recursive) {
    if (file_operation_count >= MK_MAX_FILE_OPERATIONS) return 0;

    mk_file_operation_t* operation = &file_operations[file_operation_count];
    memset(operation, 0, sizeof(*operation));
    operation->operation_id = file_operation_count + 1;
    operation->operation_type = MK_FILE_OP_MOVE;
    mk_filemanager_copy_string(operation->source_path, source_path, sizeof(operation->source_path));
    mk_filemanager_copy_string(operation->destination_path, destination_path, sizeof(operation->destination_path));
    operation->status = MK_FILE_OP_STATUS_PENDING;
    operation->priority = 1;
    operation->background = background;
    operation->start_time = mk_filemanager_get_timestamp();

    mk_filemanager_calculate_operation_stats(operation, recursive);
    file_operation_count++;

    if (!background) {
        mk_filemanager_execute_operation(operation);
    }
    return operation->operation_id;
}

uint64_t mk_filemanager_delete_files(const char* path, bool background, bool recursive, bool permanent) {
    if (file_operation_count >= MK_MAX_FILE_OPERATIONS) return 0;

    mk_file_operation_t* operation = &file_operations[file_operation_count];
    memset(operation, 0, sizeof(*operation));
    operation->operation_id = file_operation_count + 1;
    operation->operation_type = permanent ? MK_FILE_OP_DELETE_PERMANENT : MK_FILE_OP_DELETE;
    mk_filemanager_copy_string(operation->source_path, path, sizeof(operation->source_path));
    operation->status = MK_FILE_OP_STATUS_PENDING;
    operation->priority = 1;
    operation->background = background;
    operation->start_time = mk_filemanager_get_timestamp();

    mk_filemanager_calculate_operation_stats(operation, recursive);
    file_operation_count++;

    if (!background) {
        mk_filemanager_execute_operation(operation);
    }
    return operation->operation_id;
}

uint64_t mk_filemanager_search_files(const char* pattern, const char* search_path, uint64_t search_type, bool case_sensitive, bool recursive) {
    (void)search_type;
    if (file_search_count >= MK_MAX_FILE_SEARCHES) return 0;

    mk_file_search_t* search = &file_searches[file_search_count];
    memset(search, 0, sizeof(*search));
    search->search_id = file_search_count + 1;
    mk_filemanager_copy_string(search->search_pattern, pattern, sizeof(search->search_pattern));
    mk_filemanager_copy_string(search->search_path, search_path, sizeof(search->search_path));
    search->file_type_mask = MK_FILE_TYPE_ALL;
    search->case_sensitive = case_sensitive;
    search->recursive = recursive;
    search->active = true;
    search->status = MK_FILE_SEARCH_STATUS_RUNNING;
    file_search_count++;

    mk_filemanager_execute_search(search);
    return search->search_id;
}

uint64_t mk_filemanager_create_bookmark(const char* name, const char* path, const char* description, uint64_t bookmark_type) {
    if (file_bookmark_count >= MK_MAX_FILE_BOOKMARKS) return 0;

    mk_file_bookmark_t* bookmark = &file_bookmarks[file_bookmark_count];
    memset(bookmark, 0, sizeof(*bookmark));
    bookmark->bookmark_id = file_bookmark_count + 1;
    mk_filemanager_copy_string(bookmark->name, name, sizeof(bookmark->name));
    mk_filemanager_copy_string(bookmark->path, path, sizeof(bookmark->path));
    mk_filemanager_copy_string(bookmark->description, description, sizeof(bookmark->description));
    bookmark->bookmark_type = bookmark_type;
    mk_filemanager_copy_string(bookmark->icon, "folder", sizeof(bookmark->icon));
    file_bookmark_count++;
    return bookmark->bookmark_id;
}

uint64_t mk_filemanager_create_tag(const char* name, const char* color, const char* description) {
    if (file_tag_count >= MK_MAX_FILE_TAGS) return 0;

    mk_file_tag_t* tag = &file_tags[file_tag_count];
    memset(tag, 0, sizeof(*tag));
    tag->tag_id = file_tag_count + 1;
    mk_filemanager_copy_string(tag->name, name, sizeof(tag->name));
    mk_filemanager_copy_string(tag->color, color, sizeof(tag->color));
    mk_filemanager_copy_string(tag->description, description, sizeof(tag->description));
    file_tag_count++;
    return tag->tag_id;
}

uint64_t mk_filemanager_create_view(const char* name, const char* path, uint64_t view_type, uint64_t sort_type, uint64_t sort_order) {
    if (file_view_count >= MK_MAX_FILE_VIEWS) return 0;

    mk_file_view_t* view = &file_views[file_view_count];
    memset(view, 0, sizeof(*view));
    view->view_id = file_view_count + 1;
    mk_filemanager_copy_string(view->name, name, sizeof(view->name));
    mk_filemanager_copy_string(view->path, path, sizeof(view->path));
    view->view_type = view_type;
    view->sort_type = sort_type;
    view->sort_order = sort_order;
    mk_filemanager_scan_directory(view);
    file_view_count++;
    return view->view_id;
}

void mk_filemanager_list_operations(void) {
    serial_puts("File operations:\n");
    for (uint64_t i = 0; i < file_operation_count; ++i) {
        mk_file_operation_t* op = &file_operations[i];
        serial_puts("  #");
        serial_putdec(op->operation_id);
        serial_puts(" ");
        switch (op->operation_type) {
            case MK_FILE_OP_COPY: serial_puts("Copy"); break;
            case MK_FILE_OP_MOVE: serial_puts("Move"); break;
            case MK_FILE_OP_DELETE: serial_puts("Delete"); break;
            case MK_FILE_OP_DELETE_PERMANENT: serial_puts("Delete Permanent"); break;
            default: serial_puts("Unknown"); break;
        }
        serial_puts(" | ");
        switch (op->status) {
            case MK_FILE_OP_STATUS_PENDING: serial_puts("Pending"); break;
            case MK_FILE_OP_STATUS_RUNNING: serial_puts("Running"); break;
            case MK_FILE_OP_STATUS_PAUSED: serial_puts("Paused"); break;
            case MK_FILE_OP_STATUS_COMPLETED: serial_puts("Completed"); break;
            case MK_FILE_OP_STATUS_FAILED: serial_puts("Failed"); break;
            case MK_FILE_OP_STATUS_CANCELLED: serial_puts("Cancelled"); break;
            default: serial_puts("Unknown"); break;
        }
        serial_puts(" | ");
        serial_putdec(op->progress);
        serial_puts("%\n");
    }
}

void mk_filemanager_list_search_results(uint64_t search_id) {
    mk_file_search_t* search = mk_filemanager_find_search(search_id);
    if (!search) {
        serial_puts("Search not found\n");
        return;
    }
    serial_puts("Search #");
    serial_putdec(search->search_id);
    serial_puts(": ");
    serial_puts(search->search_pattern);
    serial_puts(" -> ");
    switch (search->status) {
        case MK_FILE_SEARCH_STATUS_RUNNING: serial_puts("Running"); break;
        case MK_FILE_SEARCH_STATUS_COMPLETED: serial_puts("Completed"); break;
        case MK_FILE_SEARCH_STATUS_FAILED: serial_puts("Failed"); break;
        default: serial_puts("Unknown"); break;
    }
    serial_puts(" | results: ");
    serial_putdec(search->result_count);
    serial_puts("\n");
}

void mk_filemanager_list_bookmarks(void) {
    serial_puts("Bookmarks:\n");
    for (uint64_t i = 0; i < file_bookmark_count; ++i) {
        mk_file_bookmark_t* b = &file_bookmarks[i];
        serial_puts("  #");
        serial_putdec(b->bookmark_id);
        serial_puts(" ");
        serial_puts(b->name);
        serial_puts(" -> ");
        serial_puts(b->path);
        serial_puts(" (");
        switch (b->bookmark_type) {
            case MK_BOOKMARK_TYPE_FOLDER: serial_puts("Folder"); break;
            case MK_BOOKMARK_TYPE_FILE: serial_puts("File"); break;
            case MK_BOOKMARK_TYPE_NETWORK: serial_puts("Network"); break;
            case MK_BOOKMARK_TYPE_DEVICE: serial_puts("Device"); break;
            default: serial_puts("Unknown"); break;
        }
        serial_puts(")\n");
    }
}

void mk_filemanager_monitor_operations(void) {
    for (uint64_t i = 0; i < file_operation_count; ++i) {
        mk_file_operation_t* op = &file_operations[i];
        if (op->status == MK_FILE_OP_STATUS_RUNNING && !op->paused && !op->cancelled) {
            mk_filemanager_update_operation_progress(op);
            if (op->progress >= 100) {
                op->status = MK_FILE_OP_STATUS_COMPLETED;
                op->end_time = mk_filemanager_get_timestamp();
            }
        }
    }

    for (uint64_t i = 0; i < file_search_count; ++i) {
        mk_file_search_t* search = &file_searches[i];
        if (search->status == MK_FILE_SEARCH_STATUS_RUNNING && search->active) {
            mk_filemanager_update_search_progress(search);
            if (search->result_count >= 5) {
                search->status = MK_FILE_SEARCH_STATUS_COMPLETED;
                search->active = false;
            }
        }
    }
}

void mk_filemanager_optimize_performance(void) {
    mk_filemanager_optimize_operations();
    mk_filemanager_optimize_searches();
    mk_filemanager_optimize_views();
    mk_filemanager_optimize_memory();
    mk_filemanager_log("performance optimization complete");
}

static mk_file_operation_t* mk_filemanager_find_operation(uint64_t operation_id) {
    for (uint64_t i = 0; i < file_operation_count; ++i) {
        if (file_operations[i].operation_id == operation_id) return &file_operations[i];
    }
    return NULL;
}

static mk_file_search_t* mk_filemanager_find_search(uint64_t search_id) {
    for (uint64_t i = 0; i < file_search_count; ++i) {
        if (file_searches[i].search_id == search_id) return &file_searches[i];
    }
    return NULL;
}

static mk_file_tag_t* mk_filemanager_find_tag(uint64_t tag_id) {
    for (uint64_t i = 0; i < file_tag_count; ++i) {
        if (file_tags[i].tag_id == tag_id) return &file_tags[i];
    }
    return NULL;
}

static mk_file_view_t* mk_filemanager_find_view(uint64_t view_id) {
    for (uint64_t i = 0; i < file_view_count; ++i) {
        if (file_views[i].view_id == view_id) return &file_views[i];
    }
    return NULL;
}

static void mk_filemanager_create_default_bookmarks(void) {
    mk_filemanager_create_bookmark("Home", "/home/user", "User home directory", MK_BOOKMARK_TYPE_FOLDER);
    mk_filemanager_create_bookmark("Documents", "/home/user/Documents", "Documents folder", MK_BOOKMARK_TYPE_FOLDER);
    mk_filemanager_create_bookmark("Downloads", "/home/user/Downloads", "Downloads folder", MK_BOOKMARK_TYPE_FOLDER);
    mk_filemanager_create_bookmark("Pictures", "/home/user/Pictures", "Pictures folder", MK_BOOKMARK_TYPE_FOLDER);
}

static void mk_filemanager_create_default_tags(void) {
    mk_filemanager_create_tag("Important", "red", "Important files");
    mk_filemanager_create_tag("Work", "blue", "Work-related files");
    mk_filemanager_create_tag("Archive", "gray", "Archived files");
}

static void mk_filemanager_create_default_filters(void) {
    mk_filemanager_create_filter("Images", "*.jpg;*.png;*.gif;*.bmp", MK_FILE_TYPE_IMAGE);
    mk_filemanager_create_filter("Documents", "*.txt;*.doc;*.pdf;*.rtf", MK_FILE_TYPE_DOCUMENT);
    mk_filemanager_create_filter("Audio", "*.mp3;*.wav;*.flac;*.ogg", MK_FILE_TYPE_AUDIO);
    mk_filemanager_create_filter("Video", "*.mp4;*.avi;*.mkv;*.mov", MK_FILE_TYPE_VIDEO);
    mk_filemanager_create_filter("Archives", "*.zip;*.rar;*.7z;*.tar", MK_FILE_TYPE_ARCHIVE);
}

static uint64_t mk_filemanager_create_filter(const char* name, const char* pattern, uint64_t file_type_mask) {
    if (file_filter_count >= MK_MAX_FILE_FILTERS) return 0;
    mk_file_filter_t* filter = &file_filters[file_filter_count];
    memset(filter, 0, sizeof(*filter));
    filter->filter_id = file_filter_count + 1;
    mk_filemanager_copy_string(filter->name, name, sizeof(filter->name));
    mk_filemanager_copy_string(filter->pattern, pattern, sizeof(filter->pattern));
    filter->filter_type = MK_FILE_FILTER_PATTERN;
    filter->file_type_mask = file_type_mask;
    file_filter_count++;
    return filter->filter_id;
}

static void mk_filemanager_calculate_operation_stats(mk_file_operation_t* operation, bool recursive) {
    if (!operation) return;
    operation->file_count = recursive ? 32 : 8;
    operation->total_size = operation->file_count * 4096ULL;
}

static void mk_filemanager_execute_operation(mk_file_operation_t* operation) {
    if (!operation) return;
    operation->status = MK_FILE_OP_STATUS_RUNNING;
    operation->progress = 100;
    operation->processed_size = operation->total_size;
    operation->end_time = mk_filemanager_get_timestamp();
    operation->status = MK_FILE_OP_STATUS_COMPLETED;
}

static void mk_filemanager_execute_search(mk_file_search_t* search) {
    if (!search) return;
    search->result_count = 5;
    search->status = MK_FILE_SEARCH_STATUS_COMPLETED;
    search->active = false;
}

static void mk_filemanager_scan_directory(mk_file_view_t* view) {
    if (!view) return;
    view->item_count = 42;
}

static void mk_filemanager_update_operation_progress(mk_file_operation_t* operation) {
    if (!operation) return;
    if (operation->progress < 100) operation->progress += 10;
    if (operation->progress > 100) operation->progress = 100;
}

static void mk_filemanager_update_search_progress(mk_file_search_t* search) {
    if (!search) return;
    if (search->result_count < 5) search->result_count++;
}

static void mk_filemanager_optimize_operations(void) {
    for (uint64_t i = 0; i < file_operation_count; ++i) {
        if (file_operations[i].status == MK_FILE_OP_STATUS_CANCELLED) {
            file_operations[i] = file_operations[file_operation_count - 1];
            file_operation_count--;
            i--;
        }
    }
}

static void mk_filemanager_optimize_searches(void) {
    for (uint64_t i = 0; i < file_search_count; ++i) {
        if (!file_searches[i].active && file_searches[i].status == MK_FILE_SEARCH_STATUS_COMPLETED) {
            file_searches[i].result_count = 0;
        }
    }
}

static void mk_filemanager_optimize_views(void) {
    for (uint64_t i = 0; i < file_view_count; ++i) {
        mk_filemanager_scan_directory(&file_views[i]);
    }
}

static void mk_filemanager_optimize_memory(void) {
    /* Nothing to compact in the minimal implementation. */
    (void)filemanager_started;
    (void)mk_filemanager_find_tag(0);
    (void)mk_filemanager_find_view(0);
}
