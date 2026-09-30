/*
 * modern_ui.h - C-OS 4.0.8 alpha Modern UI Framework
 * Provides advanced UI components with animations, themes, and modern design
 */

#ifndef MODERN_UI_H
#define MODERN_UI_H

#include "../include/gui.h"
#include "../include/serial.h"
#include "../include/string.h"

/* Forward declarations */
typedef struct modern_component modern_component_t;

/* Modern UI Component Types */
#define UI_TYPE_WINDOW          0
#define UI_TYPE_BUTTON          1
#define UI_TYPE_TEXT_FIELD       2
#define UI_TYPE_LABEL           3
#define UI_TYPE_IMAGE           4
#define UI_TYPE_PROGRESS_BAR    5
#define UI_TYPE_SLIDER          6
#define UI_TYPE_CHECKBOX        7
#define UI_TYPE_RADIO_GROUP      8
#define UI_TYPE_LIST            9
#define UI_TYPE_GRID            10
#define UI_TYPE_TAB_CONTAINER    11
#define UI_TYPE_MENU            12
#define UI_TYPE_TOOLTIP         13

/* Animation Types */
#define ANIM_TYPE_FADE_IN      0
#define ANIM_TYPE_FADE_OUT     1
#define ANIM_TYPE_SLIDE_IN     2
#define ANIM_TYPE_SLIDE_OUT    3
#define ANIM_TYPE_SCALE_IN      4
#define ANIM_TYPE_SCALE_OUT     5
#define ANIM_TYPE_ROTATE       6
#define ANIM_TYPE_BOUNCE        7

/* Theme System */
typedef struct {
    /* Color Scheme */
    uint64_t primary_color;
    uint64_t secondary_color;
    uint64_t accent_color;
    uint64_t background_color;
    uint64_t surface_color;
    uint64_t text_color;
    uint64_t text_secondary;
    uint64_t border_color;
    uint64_t shadow_color;
    uint64_t highlight_color;
    
    /* Typography */
    char font_family[32];
    int base_font_size;
    int title_font_size;
    int small_font_size;
    bool font_smoothing;
    
    /* Layout */
    int border_radius;
    int padding;
    int margin;
    int spacing;
    int button_height;
    int input_height;
    
    /* Visual Effects */
    bool shadows_enabled;
    bool gradients_enabled;
    bool animations_enabled;
    bool transparency_enabled;
    float transparency_level;
    
    /* Theme Metadata */
    char theme_name[64];
    char author[64];
    char version[16];
    bool dark_mode;
    bool high_contrast;
} modern_theme_t;

/* Animation Properties */
typedef struct {
    int type;
    uint64_t duration_ms;
    uint64_t delay_ms;
    float easing_factor;
    bool auto_reverse;
    bool loop;
    
    /* Current State */
    bool active;
    uint64_t start_time;
    uint64_t current_time;
    float progress;
    modern_component_t* component;
    
    /* Callbacks */
    void (*on_start)(modern_component_t* component);
    void (*on_update)(modern_component_t* component, float progress);
    void (*on_complete)(modern_component_t* component);
} ui_animation_t;

/* Modern Component Base */
typedef struct modern_component {
    int type;
    int x, y, w, h;
    bool visible;
    bool enabled;
    bool focused;
    bool hover;
    bool pressed;
    bool disabled;
    
    /* Styling */
    uint64_t bg_color;
    uint64_t fg_color;
    uint64_t border_color;
    uint64_t hover_color;
    uint64_t active_color;
    int border_radius;
    int padding;
    
    /* Layout */
    int min_width, min_height;
    int max_width, max_height;
    bool auto_size;
    
    /* Events */
    void (*on_click)(struct modern_component* comp, int x, int y);
    void (*on_double_click)(struct modern_component* comp, int x, int y);
    void (*on_right_click)(struct modern_component* comp, int x, int y);
    void (*on_hover)(struct modern_component* comp, bool hover);
    void (*on_focus)(struct modern_component* comp, bool focused);
    void (*on_key)(struct modern_component* comp, char key, bool pressed);
    void (*on_resize)(struct modern_component* comp, int w, int h);
    
    /* Animation */
    ui_animation_t* animation;
    
    /* Data */
    void* component_data;
    char text[256];
    char tooltip[256];
    
    /* Children */
    struct modern_component** children;
    int child_count;
    struct modern_component* parent;
    
    /* Rendering */
    void (*render)(struct modern_component* comp);
    void (*update)(struct modern_component* comp);
} modern_component_t;

/* Window Component */
typedef struct {
    modern_component_t base;
    char title[128];
    bool closable;
    bool minimizable;
    bool maximizable;
    bool resizable;
    bool modal;
    bool always_on_top;
    
    /* Window State */
    int state; /* 0=normal, 1=minimized, 2=maximized, 3=fullscreen */
    int previous_x, previous_y, previous_w, previous_h;
    
    /* Content Area */
    int content_x, content_y, content_w, content_h;
    
    /* Window Controls */
    modern_component_t* close_button;
    modern_component_t* minimize_button;
    modern_component_t* maximize_button;
    
    /* Title Bar */
    modern_component_t* title_label;
    modern_component_t* icon;
    
} modern_window_t;

/* Button Component */
typedef struct {
    modern_component_t base;
    char text[128];
    char icon_path[256];
    int icon_size;
    
    /* Button Style */
    int style; /* 0=primary, 1=secondary, 2=outline, 3=ghost */
    bool auto_width;
    
    /* Visual Feedback */
    bool show_ripple;
    uint64_t ripple_color;
    int ripple_duration;
    
} modern_button_t;

/* Text Field Component */
typedef struct {
    modern_component_t base;
    char text[1024];
    int cursor_position;
    int selection_start, selection_end;
    
    /* Text Field Properties */
    bool password_mode;
    bool multiline;
    int max_length;
    char placeholder[256];
    
    /* Validation */
    bool (*validator)(const char* text);
    char error_message[256];
    
    /* Auto-complete */
    char suggestions[16][64];
    int suggestion_count;
    int selected_suggestion;
    
} modern_text_field_t;

/* Progress Bar Component */
typedef struct {
    modern_component_t base;
    float value;
    float min_value, max_value;
    
    /* Progress Bar Style */
    int orientation; /* 0=horizontal, 1=vertical */
    bool show_percentage;
    bool animated;
    
    /* Colors */
    uint64_t progress_color;
    uint64_t background_color;
    uint64_t buffer_color;
    
} modern_progress_bar_t;

/* List Component */
typedef struct {
    modern_component_t base;
    char items[64][256];
    int item_count;
    int selected_item;
    int scroll_position;
    int visible_items;
    
    /* List Properties */
    bool multi_select;
    int selected_items[16];
    int selected_count;
    bool show_scrollbar;
    
    /* Item Rendering */
    int item_height;
    bool alternating_colors;
    
    /* Search */
    char search_query[128];
    int filtered_count;
    int filtered_indices[64];
    
} modern_list_t;

/* UI Manager */
typedef struct {
    modern_component_t* root_component;
    modern_component_t* focused_component;
    modern_component_t* hovered_component;
    modern_component_t* active_component;
    
    /* Theme */
    modern_theme_t* current_theme;
    modern_theme_t themes[8];
    int theme_count;
    
    /* Animation System */
    ui_animation_t animations[32];
    int active_animations;
    uint64_t animation_time;
    
    /* Event System */
    void (*click_handlers[32])(modern_component_t* comp, int x, int y);
    void (*key_handlers[16])(modern_component_t* comp, char key, bool pressed);
    int click_handler_count;
    int key_handler_count;
    
    /* Performance */
    uint64_t frame_time;
    uint64_t render_time;
    int fps;
    
} ui_manager_t;

/* Modern UI Functions */
void modern_ui_init(void);
void modern_ui_shutdown(void);
void modern_ui_update(void);
void modern_ui_render(void);

/* Component Creation Functions */
modern_component_t* modern_create_window(int x, int y, int w, int h, const char* title);
modern_component_t* modern_create_button(int x, int y, int w, int h, const char* text);
modern_component_t* modern_create_text_field(int x, int y, int w, int h, const char* placeholder);
modern_component_t* modern_create_label(int x, int y, const char* text);
modern_component_t* modern_create_progress_bar(int x, int y, int w, int h);
modern_component_t* modern_create_list(int x, int y, int w, int h);

/* Component Management */
void modern_component_add_child(modern_component_t* parent, modern_component_t* child);
void modern_component_remove_child(modern_component_t* parent, modern_component_t* child);
void modern_component_set_visible(modern_component_t* comp, bool visible);
void modern_component_set_enabled(modern_component_t* comp, bool enabled);
void modern_component_set_focus(modern_component_t* comp, bool focused);

/* Theme Management */
void modern_ui_load_theme(const char* theme_name);
void modern_ui_set_theme(modern_theme_t* theme);
modern_theme_t* modern_ui_get_theme(void);
void modern_ui_save_theme(const char* theme_name);

/* Animation Functions */
ui_animation_t* modern_ui_create_animation(int type, uint64_t duration_ms);
void modern_ui_start_animation(modern_component_t* comp, ui_animation_t* anim);
void modern_ui_stop_animation(modern_component_t* comp);
void modern_ui_update_animations(void);

/* Event Handling */
void modern_ui_handle_mouse_click(int x, int y);
void modern_ui_handle_mouse_move(int x, int y);
void modern_ui_handle_key_press(char key);
void modern_ui_handle_key_release(char key);
void modern_ui_register_click_handler(void (*handler)(modern_component_t* comp, int x, int y));
void modern_ui_register_key_handler(void (*handler)(modern_component_t* comp, char key, bool pressed));

/* Layout Functions */
void modern_ui_layout_horizontal(modern_component_t* parent, int spacing);
void modern_ui_layout_vertical(modern_component_t* parent, int spacing);
void modern_ui_layout_grid(modern_component_t* parent, int cols, int rows, int spacing);

/* Utility Functions */
modern_component_t* modern_ui_get_component_at(int x, int y);
modern_component_t* modern_ui_get_focused_component(void);
void modern_ui_set_root_component(modern_component_t* root);
void modern_ui_center_component(modern_component_t* comp);

/* Built-in Themes */
extern modern_theme_t THEME_DARK;
extern modern_theme_t THEME_LIGHT;
extern modern_theme_t THEME_BLUE;
extern modern_theme_t THEME_GREEN;
extern modern_theme_t THEME_PURPLE;
extern modern_theme_t THEME_HIGH_CONTRAST;

#endif /* MODERN_UI_H */
