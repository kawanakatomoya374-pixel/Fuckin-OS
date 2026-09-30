/*
 * modern_ui.c - C-OS 4.0.8 alpha Modern UI Framework Implementation
 * Provides advanced UI components with animations, themes, and modern design
 */

#include "modern_ui.h"
#include "serial.h"
#include "memory.h"
#include "timer.h"
#include "mouse.h"
#include "vga.h"
#include "gui_render_engine.h"
#include "settings_manager.h"
#include "theme_system.h"
// Standard library headers removed for freestanding environment

/* RGB color macro */
#define rgb(r, g, b) ((uint64_t)((r) | ((g) << 8) | ((b) << 16)))

/* Global UI Manager */
static ui_manager_t g_ui_manager;
static modern_theme_t g_built_in_themes[8];
static int g_theme_count = 0;

/* Function prototypes */
void modern_ui_update_component_recursive(modern_component_t* comp);
void modern_ui_handle_mouse_hover(void);
modern_component_t* modern_ui_find_component_recursive(modern_component_t* comp, int x, int y);
void modern_window_render(modern_component_t* comp);
void modern_button_render(modern_component_t* comp);
void modern_text_field_render(modern_component_t* comp);
void modern_label_render(modern_component_t* comp);
void modern_progress_bar_render(modern_component_t* comp);
void modern_window_update(modern_component_t* comp);
void modern_button_update(modern_component_t* comp);
void modern_text_field_update(modern_component_t* comp);
void modern_progress_bar_update(modern_component_t* comp);


/* Drag/resize helpers for movable UI items and icon buttons */
typedef struct {
    modern_component_t* comp;
    bool active;
    bool resizing;
    int offset_x;
    int offset_y;
    int start_x;
    int start_y;
    int start_w;
    int start_h;
} modern_drag_state_t;

typedef struct {
    modern_component_t* comp;
    bool draggable;
    bool resizable;
    uint32_t icon_size;
} modern_component_behavior_t;

static modern_drag_state_t g_drag_state = {0};
static modern_component_behavior_t g_behavior_table[64];
static int g_behavior_count = 0;

static int modern_clamp_int(int v, int min_v, int max_v) {
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

static modern_component_behavior_t* modern_find_behavior(modern_component_t* comp) {
    if (!comp) return NULL;
    for (int i = 0; i < g_behavior_count; ++i) {
        if (g_behavior_table[i].comp == comp) {
            return &g_behavior_table[i];
        }
    }
    return NULL;
}

static modern_component_behavior_t* modern_get_or_create_behavior(modern_component_t* comp) {
    modern_component_behavior_t* b = modern_find_behavior(comp);
    if (b) return b;
    if (!comp || g_behavior_count >= (int)(sizeof(g_behavior_table) / sizeof(g_behavior_table[0]))) {
        return NULL;
    }
    b = &g_behavior_table[g_behavior_count++];
    memset(b, 0, sizeof(*b));
    b->comp = comp;
    return b;
}

static bool modern_component_is_window(modern_component_t* comp) {
    return comp && comp->type == UI_TYPE_WINDOW;
}

static int modern_button_icon_size(modern_button_t* button) {
    modern_component_behavior_t* b;
    int size = 0;
    if (!button) return 0;
    b = modern_find_behavior((modern_component_t*)button);
    if (b && b->icon_size > 0) {
        size = (int)b->icon_size;
    } else if (button->icon_size > 0) {
        size = (int)button->icon_size;
    } else if (button->icon_path[0] != '\0') {
        size = (int)settings_get_desktop_icon_size();
    }
    if (size < 0) {
        size = 0;
    }
    if (size > 0 && size < 16) size = 16;
    if (size > 96) size = 96;
    return size;
}

static bool modern_component_is_icon_button(modern_component_t* comp) {
    if (!comp || comp->type != UI_TYPE_BUTTON) {
        return false;
    }
    return modern_button_icon_size((modern_button_t*)comp) > 0;
}

static bool modern_component_default_draggable(modern_component_t* comp) {
    modern_component_behavior_t* b = modern_find_behavior(comp);
    if (b && b->draggable) {
        return true;
    }
    return modern_component_is_window(comp) || modern_component_is_icon_button(comp);
}

static bool modern_component_default_resizable(modern_component_t* comp) {
    modern_component_behavior_t* b = modern_find_behavior(comp);
    if (b && b->resizable) {
        return true;
    }
    return modern_component_is_window(comp);
}

static void modern_ui_stop_drag(void) {
    if (g_drag_state.comp) {
        g_drag_state.comp->pressed = false;
    }
    g_drag_state.comp = NULL;
    g_drag_state.active = false;
    g_drag_state.resizing = false;
}

static void modern_ui_begin_drag(modern_component_t* comp, int mouse_x, int mouse_y, bool resizing) {
    if (!comp) return;
    g_drag_state.comp = comp;
    g_drag_state.active = true;
    g_drag_state.resizing = resizing;
    g_drag_state.offset_x = mouse_x - comp->x;
    g_drag_state.offset_y = mouse_y - comp->y;
    g_drag_state.start_x = comp->x;
    g_drag_state.start_y = comp->y;
    g_drag_state.start_w = comp->w;
    g_drag_state.start_h = comp->h;
    comp->pressed = true;
}

static void modern_ui_apply_drag(int mouse_x, int mouse_y) {
    modern_component_t* comp = g_drag_state.comp;
    modern_component_behavior_t* behavior;
    if (!g_drag_state.active || !comp) return;
    behavior = modern_find_behavior(comp);

    if (g_drag_state.resizing) {
        int new_w = mouse_x - g_drag_state.start_x;
        int new_h = mouse_y - g_drag_state.start_y;
        if (behavior && behavior->resizable) {
            comp->w = modern_clamp_int(new_w, 48, 4096);
            comp->h = modern_clamp_int(new_h, 32, 4096);
            if (comp->update) {
                comp->update(comp);
            }
        }
        return;
    }

    comp->x = mouse_x - g_drag_state.offset_x;
    comp->y = mouse_y - g_drag_state.offset_y;
    if (comp->update) {
        comp->update(comp);
    }
}

void modern_ui_set_component_draggable(modern_component_t* comp, bool draggable) {
    modern_component_behavior_t* b = modern_get_or_create_behavior(comp);
    if (!b) return;
    b->draggable = draggable;
}

void modern_ui_set_component_resizable(modern_component_t* comp, bool resizable) {
    modern_component_behavior_t* b = modern_get_or_create_behavior(comp);
    if (!b) return;
    b->resizable = resizable;
}

void modern_ui_set_component_icon_size(modern_component_t* comp, uint32_t size) {
    modern_component_behavior_t* b = modern_get_or_create_behavior(comp);
    if (!b || !comp || comp->type != UI_TYPE_BUTTON) return;
    b->icon_size = size;
    ((modern_button_t*)comp)->icon_size = size;
}

/* Small utility helpers */
static void modern_copy_string(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    cos_strlcpy(dst, src, dst_size);
}

static void modern_init_component(modern_component_t* comp, int type, int x, int y, int w, int h) {
    if (!comp) {
        return;
    }
    memset(comp, 0, sizeof(*comp));
    comp->type = type;
    comp->x = x;
    comp->y = y;
    comp->w = w;
    comp->h = h;
    comp->visible = true;
    comp->enabled = true;
    comp->focused = false;
    comp->hover = false;
    comp->pressed = false;
    comp->disabled = false;
}

static void modern_destroy_component_recursive(modern_component_t* comp) {
    if (!comp) {
        return;
    }
    for (int i = 0; i < comp->child_count; ++i) {
        modern_destroy_component_recursive(comp->children[i]);
    }
    if (comp->children) {
        kfree(comp->children);
    }
    kfree(comp);
}

static void modern_render_component_recursive(modern_component_t* comp) {
    if (!comp || !comp->visible) {
        return;
    }
    if (comp->render) {
        comp->render(comp);
    }
    for (int i = 0; i < comp->child_count; ++i) {
        modern_render_component_recursive(comp->children[i]);
    }
}

/* Built-in Themes */
modern_theme_t THEME_DARK = {
    .primary_color = rgb(45, 85, 145),
    .secondary_color = rgb(100, 160, 255),
    .accent_color = rgb(255, 200, 0),
    .background_color = rgb(25, 35, 55),
    .surface_color = rgb(35, 45, 65),
    .text_color = rgb(255, 255, 255),
    .text_secondary = rgb(200, 200, 200),
    .border_color = rgb(100, 100, 100),
    .shadow_color = rgb(0, 0, 0),
    .highlight_color = rgb(255, 255, 0),
    .font_family = "Arial",
    .base_font_size = 12,
    .title_font_size = 16,
    .small_font_size = 10,
    .font_smoothing = true,
    .border_radius = 6,
    .padding = 8,
    .margin = 4,
    .spacing = 4,
    .button_height = 32,
    .input_height = 28,
    .shadows_enabled = true,
    .gradients_enabled = true,
    .animations_enabled = true,
    .transparency_enabled = false,
    .transparency_level = 0.9f,
    .theme_name = "Dark Theme",
    .author = "C-OS 4.0.8 alpha Project",
    .version = "1.0",
    .dark_mode = true,
    .high_contrast = false
};

modern_theme_t THEME_LIGHT = {
    .primary_color = rgb(70, 130, 180),
    .secondary_color = rgb(100, 160, 255),
    .accent_color = rgb(255, 150, 0),
    .background_color = rgb(245, 248, 252),
    .surface_color = rgb(255, 255, 255),
    .text_color = rgb(30, 30, 30),
    .text_secondary = rgb(100, 100, 100),
    .border_color = rgb(200, 200, 200),
    .shadow_color = rgb(0, 0, 0),
    .highlight_color = rgb(255, 220, 0),
    .font_family = "Arial",
    .base_font_size = 12,
    .title_font_size = 16,
    .small_font_size = 10,
    .font_smoothing = true,
    .border_radius = 6,
    .padding = 8,
    .margin = 4,
    .spacing = 4,
    .button_height = 32,
    .input_height = 28,
    .shadows_enabled = true,
    .gradients_enabled = true,
    .animations_enabled = true,
    .transparency_enabled = false,
    .transparency_level = 0.9f,
    .theme_name = "Light Theme",
    .author = "C-OS 4.0.8 alpha Project",
    .version = "1.0",
    .dark_mode = false,
    .high_contrast = false
};

modern_theme_t THEME_BLUE = {
    .primary_color = rgb(0, 120, 215),
    .secondary_color = rgb(0, 180, 255),
    .accent_color = rgb(255, 200, 0),
    .background_color = rgb(240, 248, 255),
    .surface_color = rgb(255, 255, 255),
    .text_color = rgb(0, 60, 120),
    .text_secondary = rgb(0, 100, 180),
    .border_color = rgb(180, 210, 230),
    .shadow_color = rgb(0, 40, 80),
    .highlight_color = rgb(255, 220, 0),
    .font_family = "Arial",
    .base_font_size = 12,
    .title_font_size = 16,
    .small_font_size = 10,
    .font_smoothing = true,
    .border_radius = 8,
    .padding = 10,
    .margin = 6,
    .spacing = 6,
    .button_height = 36,
    .input_height = 32,
    .shadows_enabled = true,
    .gradients_enabled = true,
    .animations_enabled = true,
    .transparency_enabled = true,
    .transparency_level = 0.85f,
    .theme_name = "Blue Theme",
    .author = "C-OS 4.0.8 alpha Project",
    .version = "1.0",
    .dark_mode = false,
    .high_contrast = false
};

modern_theme_t THEME_GREEN = {
    .primary_color = rgb(0, 140, 90),
    .secondary_color = rgb(70, 200, 140),
    .accent_color = rgb(255, 210, 0),
    .background_color = rgb(242, 250, 244),
    .surface_color = rgb(255, 255, 255),
    .text_color = rgb(18, 70, 44),
    .text_secondary = rgb(80, 120, 96),
    .border_color = rgb(180, 210, 190),
    .shadow_color = rgb(0, 40, 20),
    .highlight_color = rgb(255, 230, 120),
    .font_family = "Arial",
    .base_font_size = 12,
    .title_font_size = 16,
    .small_font_size = 10,
    .font_smoothing = true,
    .border_radius = 8,
    .padding = 10,
    .margin = 6,
    .spacing = 6,
    .button_height = 36,
    .input_height = 32,
    .shadows_enabled = true,
    .gradients_enabled = true,
    .animations_enabled = true,
    .transparency_enabled = true,
    .transparency_level = 0.88f,
    .theme_name = "Green Theme",
    .author = "C-OS 4.0.8 alpha Project",
    .version = "1.0",
    .dark_mode = false,
    .high_contrast = false
};

modern_theme_t THEME_PURPLE = {
    .primary_color = rgb(115, 70, 180),
    .secondary_color = rgb(170, 120, 235),
    .accent_color = rgb(255, 200, 60),
    .background_color = rgb(248, 244, 252),
    .surface_color = rgb(255, 255, 255),
    .text_color = rgb(62, 32, 98),
    .text_secondary = rgb(112, 90, 140),
    .border_color = rgb(205, 190, 220),
    .shadow_color = rgb(50, 20, 80),
    .highlight_color = rgb(255, 230, 180),
    .font_family = "Arial",
    .base_font_size = 12,
    .title_font_size = 16,
    .small_font_size = 10,
    .font_smoothing = true,
    .border_radius = 8,
    .padding = 10,
    .margin = 6,
    .spacing = 6,
    .button_height = 36,
    .input_height = 32,
    .shadows_enabled = true,
    .gradients_enabled = true,
    .animations_enabled = true,
    .transparency_enabled = true,
    .transparency_level = 0.88f,
    .theme_name = "Purple Theme",
    .author = "C-OS 4.0.8 alpha Project",
    .version = "1.0",
    .dark_mode = false,
    .high_contrast = false
};

modern_theme_t THEME_HIGH_CONTRAST = {
    .primary_color = rgb(0, 0, 0),
    .secondary_color = rgb(255, 255, 255),
    .accent_color = rgb(255, 255, 0),
    .background_color = rgb(0, 0, 0),
    .surface_color = rgb(30, 30, 30),
    .text_color = rgb(255, 255, 255),
    .text_secondary = rgb(220, 220, 220),
    .border_color = rgb(255, 255, 255),
    .shadow_color = rgb(0, 0, 0),
    .highlight_color = rgb(255, 255, 0),
    .font_family = "Arial",
    .base_font_size = 12,
    .title_font_size = 16,
    .small_font_size = 10,
    .font_smoothing = false,
    .border_radius = 2,
    .padding = 8,
    .margin = 4,
    .spacing = 4,
    .button_height = 34,
    .input_height = 30,
    .shadows_enabled = false,
    .gradients_enabled = false,
    .animations_enabled = false,
    .transparency_enabled = false,
    .transparency_level = 1.0f,
    .theme_name = "High Contrast",
    .author = "C-OS 4.0.8 alpha Project",
    .version = "1.0",
    .dark_mode = true,
    .high_contrast = true
};

/* Initialize Modern UI System */
void modern_ui_init(void) {
    serial_puts("[MODERN_UI] Initializing modern UI framework\n");
    
    // Initialize UI manager
    memset(&g_ui_manager, 0, sizeof(ui_manager_t));
    g_ui_manager.frame_time = 0;
    g_ui_manager.render_time = 0;
    g_ui_manager.fps = 60;
    
    // Initialize built-in themes
    memcpy(&g_built_in_themes[0], &THEME_DARK, sizeof(modern_theme_t));
    memcpy(&g_built_in_themes[1], &THEME_LIGHT, sizeof(modern_theme_t));
    memcpy(&g_built_in_themes[2], &THEME_BLUE, sizeof(modern_theme_t));
    g_theme_count = 3;
    
    /* Sync with new theme system */
    theme_t* sys_theme = theme_get_current();
    if (sys_theme) {
        g_built_in_themes[0].primary_color = color_to_uint64(sys_theme->accent_color);
        g_built_in_themes[0].background_color = color_to_uint64(sys_theme->background_color);
        g_built_in_themes[0].surface_color = color_to_uint64(sys_theme->foreground_color);
        g_built_in_themes[0].text_color = color_to_uint64(sys_theme->text_color);
        g_built_in_themes[0].border_color = color_to_uint64(sys_theme->border_color);
    }
    
    // Set default theme
    g_ui_manager.current_theme = &g_built_in_themes[0]; // Dark theme
    memcpy(&g_ui_manager.themes[0], &g_built_in_themes[0], sizeof(modern_theme_t));
    g_ui_manager.theme_count = g_theme_count;
    for (int i = 0; i < g_theme_count; ++i) {
        const char* os_name = settings_get_os_name();
        strncpy(g_built_in_themes[i].author, os_name ? os_name : "C-OS", sizeof(g_built_in_themes[i].author) - 1);
        g_built_in_themes[i].author[sizeof(g_built_in_themes[i].author) - 1] = '\0';
        strncpy(g_ui_manager.themes[i].author, os_name ? os_name : "C-OS", sizeof(g_ui_manager.themes[i].author) - 1);
        g_ui_manager.themes[i].author[sizeof(g_ui_manager.themes[i].author) - 1] = '\0';
    }
    
    // Initialize animation system
    memset(g_ui_manager.animations, 0, sizeof(g_ui_manager.animations));
    g_ui_manager.active_animations = 0;
    g_ui_manager.animation_time = 0;
    
    serial_puts("[MODERN_UI] Modern UI framework initialized\n");
    gui_render_init();
}

/* Create Modern Window */
modern_component_t* modern_create_window(int x, int y, int w, int h, const char* title) {
    modern_window_t* window = kmalloc(sizeof(modern_window_t));
    if (!window) return NULL;
    memset(window, 0, sizeof(*window));
    
    // Initialize base component
    window->base.type = UI_TYPE_WINDOW;
    window->base.x = x;
    window->base.y = y;
    window->base.w = w;
    window->base.h = h;
    window->base.visible = true;
    window->base.enabled = true;
    window->base.focused = false;
    window->base.hover = false;
    window->base.pressed = false;
    window->base.disabled = false;
    
    // Apply theme colors
    modern_theme_t* theme = g_ui_manager.current_theme;
    window->base.bg_color = theme->surface_color;
    window->base.fg_color = theme->text_color;
    window->base.border_color = theme->border_color;
    window->base.border_radius = theme->border_radius;
    window->base.padding = theme->padding;
    
    // Window properties
    modern_copy_string(window->title, sizeof(window->title), title);
    window->closable = true;
    window->minimizable = true;
    window->maximizable = true;
    window->resizable = true;
    window->modal = false;
    window->always_on_top = false;
    window->state = 0; // Normal
    
    // Calculate content area
    window->content_x = x + theme->border_radius;
    window->content_y = y + 30; // Title bar height
    window->content_w = w - (theme->border_radius * 2);
    window->content_h = h - 30 - theme->border_radius;
    
    // Create title bar components
    window->title_label = modern_create_label(x + theme->border_radius + 5, y + 5, title);
    window->close_button = modern_create_button(x + w - 25, y + 5, 20, 20, "X");
    window->minimize_button = modern_create_button(x + w - 70, y + 5, 20, 20, "-");
    window->maximize_button = modern_create_button(x + w - 45, y + 5, 20, 20, "□");
    
    // Set render function
    window->base.render = (void (*)(modern_component_t*))modern_window_render;
    window->base.update = (void (*)(modern_component_t*))modern_window_update;
    modern_ui_set_component_draggable((modern_component_t*)window, true);
    modern_ui_set_component_resizable((modern_component_t*)window, true);
    
    serial_puts("[MODERN_UI] Created window: ");
    serial_puts(title);
    serial_puts("\n");
    
    return (modern_component_t*)window;
}

/* Create Modern Button */
modern_component_t* modern_create_button(int x, int y, int w, int h, const char* text) {
    modern_button_t* button = kmalloc(sizeof(modern_button_t));
    if (!button) return NULL;
    memset(button, 0, sizeof(*button));
    
    // Initialize base component
    button->base.type = UI_TYPE_BUTTON;
    button->base.x = x;
    button->base.y = y;
    button->base.w = w;
    button->base.h = h;
    button->base.visible = true;
    button->base.enabled = true;
    button->base.focused = false;
    button->base.hover = false;
    button->base.pressed = false;
    button->base.disabled = false;
    
    // Apply theme colors
    modern_theme_t* theme = g_ui_manager.current_theme;
    button->base.bg_color = theme->primary_color;
    button->base.fg_color = theme->text_color;
    button->base.border_color = theme->border_color;
    button->base.border_radius = theme->border_radius;
    button->base.padding = theme->padding;
    
    // Button properties
    modern_copy_string(button->text, sizeof(button->text), text);
    button->icon_path[0] = '\0';
    button->icon_size = 0;
    button->style = 0; // Primary style
    button->auto_width = false;
    
    // Visual feedback
    button->show_ripple = theme->animations_enabled;
    button->ripple_color = theme->highlight_color;
    button->ripple_duration = 300;
    
    // Set hover and active colors
    button->base.hover_color = theme->secondary_color;
    button->base.active_color = theme->accent_color;
    
    // Set render function
    button->base.render = (void (*)(modern_component_t*))modern_button_render;
    button->base.update = (void (*)(modern_component_t*))modern_button_update;
    
    return (modern_component_t*)button;
}

/* Create Text Field */
modern_component_t* modern_create_text_field(int x, int y, int w, int h, const char* placeholder) {
    modern_text_field_t* text_field = kmalloc(sizeof(modern_text_field_t));
    if (!text_field) return NULL;
    memset(text_field, 0, sizeof(*text_field));
    
    // Initialize base component
    text_field->base.type = UI_TYPE_TEXT_FIELD;
    text_field->base.x = x;
    text_field->base.y = y;
    text_field->base.w = w;
    text_field->base.h = h;
    text_field->base.visible = true;
    text_field->base.enabled = true;
    text_field->base.focused = false;
    text_field->base.hover = false;
    text_field->base.pressed = false;
    text_field->base.disabled = false;
    
    // Apply theme colors
    modern_theme_t* theme = g_ui_manager.current_theme;
    text_field->base.bg_color = theme->surface_color;
    text_field->base.fg_color = theme->text_color;
    text_field->base.border_color = theme->border_color;
    text_field->base.border_radius = theme->border_radius;
    text_field->base.padding = theme->padding;
    
    // Text field properties
    text_field->text[0] = '\0';
    text_field->cursor_position = 0;
    text_field->selection_start = 0;
    text_field->selection_end = 0;
    text_field->password_mode = false;
    text_field->multiline = false;
    text_field->max_length = 256;
    modern_copy_string(text_field->placeholder, sizeof(text_field->placeholder), placeholder);
    
    // Validation
    text_field->validator = NULL;
    text_field->error_message[0] = '\0';
    
    // Auto-complete
    text_field->suggestion_count = 0;
    text_field->selected_suggestion = 0;
    
    // Set render function
    text_field->base.render = (void (*)(modern_component_t*))modern_text_field_render;
    text_field->base.update = (void (*)(modern_component_t*))modern_text_field_update;
    
    return (modern_component_t*)text_field;
}

/* Create Label */
modern_component_t* modern_create_label(int x, int y, const char* text) {
    modern_component_t* label = kmalloc(sizeof(modern_component_t));
    if (!label) return NULL;
    memset(label, 0, sizeof(*label));
    
    // Initialize base component
    label->type = UI_TYPE_LABEL;
    label->x = x;
    label->y = y;
    label->w = text ? (int)strlen(text) * 8 : 0; // Approximate width
    label->h = 16;
    label->visible = true;
    label->enabled = true;
    label->focused = false;
    label->hover = false;
    label->pressed = false;
    label->disabled = false;
    
    // Apply theme colors
    modern_theme_t* theme = g_ui_manager.current_theme;
    label->bg_color = theme->background_color; // Transparent background
    label->fg_color = theme->text_color;
    label->border_color = theme->background_color; // No border
    label->border_radius = 0;
    label->padding = 0;
    
    // Label properties
    modern_copy_string(label->text, sizeof(label->text), text);
    label->tooltip[0] = '\0';
    
    // Set render function
    label->render = (void (*)(modern_component_t*))modern_label_render;
    label->update = NULL; // Labels don't need updates
    
    return label;
}

/* Create Progress Bar */
modern_component_t* modern_create_progress_bar(int x, int y, int w, int h) {
    modern_progress_bar_t* progress = kmalloc(sizeof(modern_progress_bar_t));
    if (!progress) return NULL;
    memset(progress, 0, sizeof(*progress));
    
    // Initialize base component
    progress->base.type = UI_TYPE_PROGRESS_BAR;
    progress->base.x = x;
    progress->base.y = y;
    progress->base.w = w;
    progress->base.h = h;
    progress->base.visible = true;
    progress->base.enabled = true;
    progress->base.focused = false;
    progress->base.hover = false;
    progress->base.pressed = false;
    progress->base.disabled = false;
    
    // Apply theme colors
    modern_theme_t* theme = g_ui_manager.current_theme;
    progress->base.bg_color = theme->surface_color;
    progress->base.fg_color = theme->text_color;
    progress->base.border_color = theme->border_color;
    progress->base.border_radius = theme->border_radius;
    progress->base.padding = theme->padding;
    
    // Progress bar properties
    progress->value = 0.0f;
    progress->min_value = 0.0f;
    progress->max_value = 100.0f;
    progress->orientation = 0; // Horizontal
    progress->show_percentage = true;
    progress->animated = theme->animations_enabled;
    
    // Progress bar colors
    progress->progress_color = theme->primary_color;
    progress->background_color = theme->surface_color;
    progress->buffer_color = theme->secondary_color;
    
    // Set render function
    progress->base.render = (void (*)(modern_component_t*))modern_progress_bar_render;
    progress->base.update = (void (*)(modern_component_t*))modern_progress_bar_update;
    
    return (modern_component_t*)progress;
}


void modern_ui_shutdown(void) {
    modern_destroy_component_recursive(g_ui_manager.root_component);
    memset(&g_ui_manager, 0, sizeof(g_ui_manager));
}

void modern_ui_render(void) {
    uint64_t start_time = get_timer_ticks();
    if (g_ui_manager.root_component) {
        modern_render_component_recursive(g_ui_manager.root_component);
    }
    uint64_t end_time = get_timer_ticks();
    g_ui_manager.render_time = end_time - start_time;
    g_ui_manager.fps = (g_ui_manager.frame_time > 0) ? (int)(1000 / (g_ui_manager.frame_time ? g_ui_manager.frame_time : 1)) : g_ui_manager.fps;
}

modern_component_t* modern_ui_get_focused_component(void) {
    return g_ui_manager.focused_component;
}

void modern_ui_set_root_component(modern_component_t* root) {
    g_ui_manager.root_component = root;
}

void modern_ui_center_component(modern_component_t* comp) {
    if (!comp) {
        return;
    }
    int screen_w = (int)SCREEN_W;
    int screen_h = (int)SCREEN_H;
    comp->x = (screen_w - comp->w) / 2;
    comp->y = (screen_h - comp->h) / 2;
}

void modern_component_add_child(modern_component_t* parent, modern_component_t* child) {
    if (!parent || !child) {
        return;
    }
    modern_component_t** new_children = krealloc(parent->children,
        sizeof(modern_component_t*) * (size_t)(parent->child_count + 1));
    if (!new_children) {
        return;
    }
    parent->children = new_children;
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void modern_component_remove_child(modern_component_t* parent, modern_component_t* child) {
    if (!parent || !child || parent->child_count <= 0 || !parent->children) {
        return;
    }
    int write = 0;
    for (int i = 0; i < parent->child_count; ++i) {
        if (parent->children[i] != child) {
            parent->children[write++] = parent->children[i];
        }
    }
    parent->child_count = write;
    child->parent = NULL;
}

void modern_component_set_visible(modern_component_t* comp, bool visible) {
    if (comp) {
        comp->visible = visible;
    }
}

void modern_component_set_enabled(modern_component_t* comp, bool enabled) {
    if (comp) {
        comp->enabled = enabled;
    }
}

void modern_component_set_focus(modern_component_t* comp, bool focused) {
    if (!comp) {
        return;
    }
    comp->focused = focused;
    if (focused) {
        g_ui_manager.focused_component = comp;
    } else if (g_ui_manager.focused_component == comp) {
        g_ui_manager.focused_component = NULL;
    }
}

void modern_ui_set_theme(modern_theme_t* theme) {
    if (!theme) {
        return;
    }
    g_ui_manager.current_theme = theme;
}

void modern_ui_load_theme(const char* theme_name) {
    if (!theme_name) {
        return;
    }
    if (strcmp(theme_name, "dark") == 0 || strcmp(theme_name, "Dark Theme") == 0) {
        modern_ui_set_theme(&THEME_DARK);
    } else if (strcmp(theme_name, "light") == 0 || strcmp(theme_name, "Light Theme") == 0) {
        modern_ui_set_theme(&THEME_LIGHT);
    } else if (strcmp(theme_name, "blue") == 0 || strcmp(theme_name, "Blue Theme") == 0) {
        modern_ui_set_theme(&THEME_BLUE);
    } else if (strcmp(theme_name, "green") == 0 || strcmp(theme_name, "Green Theme") == 0) {
        modern_ui_set_theme(&THEME_GREEN);
    } else if (strcmp(theme_name, "purple") == 0 || strcmp(theme_name, "Purple Theme") == 0) {
        modern_ui_set_theme(&THEME_PURPLE);
    } else if (strcmp(theme_name, "high_contrast") == 0 || strcmp(theme_name, "High Contrast") == 0) {
        modern_ui_set_theme(&THEME_HIGH_CONTRAST);
    }
}

void modern_ui_save_theme(const char* theme_name) {
    (void)theme_name;
    serial_puts("[MODERN_UI] save_theme is a no-op in the freestanding build\n");
}

ui_animation_t* modern_ui_create_animation(int type, uint64_t duration_ms) {
    if (g_ui_manager.active_animations >= (int)(sizeof(g_ui_manager.animations) / sizeof(g_ui_manager.animations[0]))) {
        return NULL;
    }
    ui_animation_t* anim = &g_ui_manager.animations[g_ui_manager.active_animations++];
    memset(anim, 0, sizeof(*anim));
    anim->type = type;
    anim->duration_ms = duration_ms ? duration_ms : 1;
    anim->easing_factor = 1.0f;
    return anim;
}

void modern_ui_start_animation(modern_component_t* comp, ui_animation_t* anim) {
    if (!comp || !anim) {
        return;
    }
    anim->component = comp;
    anim->start_time = get_timer_ticks();
    anim->current_time = anim->start_time;
    anim->active = true;
    anim->progress = 0.0f;
    if (anim->on_start) {
        anim->on_start(comp);
    }
}

void modern_ui_stop_animation(modern_component_t* comp) {
    if (!comp) {
        return;
    }
    for (int i = 0; i < g_ui_manager.active_animations; ++i) {
        if (g_ui_manager.animations[i].component == comp) {
            g_ui_manager.animations[i].active = false;
        }
    }
}

void modern_ui_register_click_handler(void (*handler)(modern_component_t* comp, int x, int y)) {
    if (!handler || g_ui_manager.click_handler_count >= 32) {
        return;
    }
    g_ui_manager.click_handlers[g_ui_manager.click_handler_count++] = handler;
}

void modern_ui_register_key_handler(void (*handler)(modern_component_t* comp, char key, bool pressed)) {
    if (!handler || g_ui_manager.key_handler_count >= 16) {
        return;
    }
    g_ui_manager.key_handlers[g_ui_manager.key_handler_count++] = handler;
}

void modern_ui_handle_mouse_click(int x, int y) {
    modern_component_t* comp = modern_ui_get_component_at(x, y);

    if (g_drag_state.active && g_drag_state.comp == comp) {
        modern_ui_stop_drag();
        return;
    }
    if (g_drag_state.active && g_drag_state.comp && comp != g_drag_state.comp) {
        modern_ui_stop_drag();
    }

    if (comp && comp->enabled) {
        modern_component_set_focus(comp, true);

        if (modern_component_default_draggable(comp)) {
            bool resize_mode = false;
            if (modern_component_is_window(comp)) {
                modern_window_t* window = (modern_window_t*)comp;
                if (y >= comp->y + comp->h - 18 && x >= comp->x + comp->w - 18 && window->resizable) {
                    resize_mode = true;
                } else if (y > comp->y + 30) {
                    /* click within the body starts a move; title bar is also movable */
                    resize_mode = false;
                }
            }
            modern_ui_begin_drag(comp, x, y, resize_mode);
        }

        if (comp->on_click) {
            comp->on_click(comp, x, y);
        }
        for (int i = 0; i < g_ui_manager.click_handler_count; ++i) {
            if (g_ui_manager.click_handlers[i]) {
                g_ui_manager.click_handlers[i](comp, x, y);
            }
        }
    } else {
        modern_ui_stop_drag();
    }
}

void modern_ui_handle_mouse_move(int x, int y) {
    if (g_drag_state.active) {
        modern_ui_apply_drag(x, y);
    }
    modern_ui_handle_mouse_hover();
}

void modern_ui_handle_key_press(char key) {
    modern_component_t* target = g_ui_manager.focused_component;
    if (target && target->on_key) {
        target->on_key(target, key, true);
    }
    for (int i = 0; i < g_ui_manager.key_handler_count; ++i) {
        if (g_ui_manager.key_handlers[i]) {
            g_ui_manager.key_handlers[i](target, key, true);
        }
    }
}

void modern_ui_handle_key_release(char key) {
    modern_component_t* target = g_ui_manager.focused_component;
    if (target && target->on_key) {
        target->on_key(target, key, false);
    }
    for (int i = 0; i < g_ui_manager.key_handler_count; ++i) {
        if (g_ui_manager.key_handlers[i]) {
            g_ui_manager.key_handlers[i](target, key, false);
        }
    }
}

modern_component_t* modern_create_list(int x, int y, int w, int h) {
    modern_list_t* list = kmalloc(sizeof(modern_list_t));
    if (!list) {
        return NULL;
    }
    modern_init_component(&list->base, UI_TYPE_LIST, x, y, w, h);
    modern_theme_t* theme = g_ui_manager.current_theme;
    list->base.bg_color = theme->surface_color;
    list->base.fg_color = theme->text_color;
    list->base.border_color = theme->border_color;
    list->base.border_radius = theme->border_radius;
    list->base.padding = theme->padding;
    list->item_count = 0;
    list->selected_item = -1;
    list->scroll_position = 0;
    list->visible_items = (h > 0) ? (h / 16) : 0;
    list->multi_select = false;
    list->selected_count = 0;
    list->show_scrollbar = true;
    list->item_height = 16;
    list->alternating_colors = true;
    list->search_query[0] = '\0';
    list->filtered_count = 0;
    list->base.render = NULL;
    list->base.update = NULL;
    return (modern_component_t*)list;
}

void modern_ui_layout_horizontal(modern_component_t* parent, int spacing) {
    if (!parent || !parent->children) {
        return;
    }
    int x = parent->x;
    for (int i = 0; i < parent->child_count; ++i) {
        modern_component_t* child = parent->children[i];
        if (!child) continue;
        child->x = x;
        child->y = parent->y;
        x += child->w + spacing;
    }
}

void modern_ui_layout_vertical(modern_component_t* parent, int spacing) {
    if (!parent || !parent->children) {
        return;
    }
    int y = parent->y;
    for (int i = 0; i < parent->child_count; ++i) {
        modern_component_t* child = parent->children[i];
        if (!child) continue;
        child->x = parent->x;
        child->y = y;
        y += child->h + spacing;
    }
}

void modern_ui_layout_grid(modern_component_t* parent, int cols, int rows, int spacing) {
    if (!parent || !parent->children || cols <= 0 || rows <= 0) {
        return;
    }
    int cell_w = (parent->w - (cols - 1) * spacing) / cols;
    int cell_h = (parent->h - (rows - 1) * spacing) / rows;
    for (int i = 0; i < parent->child_count; ++i) {
        int row = i / cols;
        int col = i % cols;
        if (row >= rows) break;
        modern_component_t* child = parent->children[i];
        if (!child) continue;
        child->x = parent->x + col * (cell_w + spacing);
        child->y = parent->y + row * (cell_h + spacing);
        child->w = cell_w;
        child->h = cell_h;
    }
}

/* Render Window Component */
void modern_window_render(modern_component_t* comp) {
    modern_window_t* window = (modern_window_t*)comp;
    if (!comp->visible) return;
    
    modern_theme_t* theme = g_ui_manager.current_theme;
    
    // Draw window shadow
    if (theme->shadows_enabled) {
        vga_fill_rect(comp->x + 4, comp->y + 4, comp->w, comp->h, 
                       theme->shadow_color);
    }
    
    // Draw window background
    vga_fill_rounded_rect(comp->x, comp->y, comp->w, comp->h, 
                           theme->border_radius, comp->bg_color);
    
    // Draw window border
    vga_draw_rounded_rect(comp->x, comp->y, comp->w, comp->h, 
                           theme->border_radius, comp->border_color);
    
    // Draw title bar
    vga_fill_rect(comp->x, comp->y, comp->w, 30, theme->primary_color);
    
    // Draw title text
    if (window->title_label) {
        window->title_label->render(window->title_label);
    }
    
    // Draw window controls
    if (window->close_button) window->close_button->render(window->close_button);
    if (window->minimize_button) window->minimize_button->render(window->minimize_button);
    if (window->maximize_button) window->maximize_button->render(window->maximize_button);
    
    // Draw content area background
    vga_fill_rect(window->content_x, window->content_y, 
                   window->content_w, window->content_h, theme->surface_color);
}

/* Render Button Component */
void modern_button_render(modern_component_t* comp) {
    modern_button_t* button = (modern_button_t*)comp;
    if (!comp->visible) return;
    
    modern_theme_t* theme = g_ui_manager.current_theme;
    
    // Determine button color based on state
    uint64_t bg_color = comp->bg_color;
    if (comp->disabled) {
        bg_color = rgb(150, 150, 150);
    } else if (comp->pressed) {
        bg_color = comp->active_color;
    } else if (comp->hover) {
        bg_color = comp->hover_color;
    }
    
    // Draw button shadow
    if (theme->shadows_enabled && !comp->disabled) {
        gui_render_fill_rect(comp->x + 2, comp->y + 2, comp->w, comp->h, theme->shadow_color);
    }

    gui_render_fill_rounded_rect(comp->x, comp->y, comp->w, comp->h, theme->border_radius, bg_color);
    gui_render_draw_rounded_rect(comp->x, comp->y, comp->w, comp->h, theme->border_radius, comp->border_color);

    int text_x = comp->x + (comp->w - strlen(button->text) * 8) / 2;
    int text_y = comp->y + (comp->h - 16) / 2;
    gui_render_draw_text(text_x, text_y, button->text, comp->fg_color, 0xFFFFFFFF);
}

/* Render Text Field */
void modern_text_field_render(modern_component_t* comp) {
    modern_text_field_t* text_field = (modern_text_field_t*)comp;
    if (!comp->visible) return;
    
    modern_theme_t* theme = g_ui_manager.current_theme;
    
    // Draw text field background
    vga_fill_rounded_rect(comp->x, comp->y, comp->w, comp->h, 
                           theme->border_radius, comp->bg_color);
    
    // Draw text field border
    uint64_t border_color = comp->focused ? theme->accent_color : comp->border_color;
    vga_draw_rounded_rect(comp->x, comp->y, comp->w, comp->h, 
                           theme->border_radius, border_color);
    
    // Draw text or placeholder
    const char* display_text = strlen(text_field->text) > 0 ? 
                              text_field->text : text_field->placeholder;
    uint64_t text_color = strlen(text_field->text) > 0 ? 
                             comp->fg_color : theme->text_secondary;
    
    int text_x = comp->x + comp->padding;
    int text_y = comp->y + (comp->h - 16) / 2;
    gui_render_draw_text(text_x, text_y, display_text, text_color, 0xFFFFFFFF);
    
    // Draw cursor if focused
    if (comp->focused && strlen(text_field->text) > 0) {
        int cursor_x = text_x + strlen(text_field->text) * 8;
        gui_render_fill_rect(cursor_x, text_y, 1, 16, comp->fg_color);
    }
}

/* Render Label */
void modern_label_render(modern_component_t* comp) {
    if (!comp->visible) return;
    
    gui_render_draw_text(comp->x, comp->y, comp->text, comp->fg_color, 0xFFFFFFFF);
}

/* Render Progress Bar */
void modern_progress_bar_render(modern_component_t* comp) {
    modern_progress_bar_t* progress = (modern_progress_bar_t*)comp;
    if (!comp->visible) return;
    
    modern_theme_t* theme = g_ui_manager.current_theme;
    
    // Draw progress bar background
    gui_render_fill_rounded_rect(comp->x, comp->y, comp->w, comp->h, theme->border_radius, progress->background_color);

    gui_render_draw_rounded_rect(comp->x, comp->y, comp->w, comp->h, theme->border_radius, comp->border_color);
    
    // Calculate progress width
    float progress_ratio = progress->value / progress->max_value;
    if (progress_ratio > 1.0f) progress_ratio = 1.0f;
    if (progress_ratio < 0.0f) progress_ratio = 0.0f;
    
    int progress_width = (int)((comp->w - 4) * progress_ratio);
    if (progress_width < 0) progress_width = 0;
    
    // Draw progress fill
    if (progress_width > 0) {
        gui_render_fill_rounded_rect(comp->x + 2, comp->y + 2, progress_width, comp->h - 4, theme->border_radius - 2, progress->progress_color);
    }
    
    // Draw percentage text if enabled
    if (progress->show_percentage) {
        char percentage_text[16];
        snprintf(percentage_text, sizeof(percentage_text), "%d%%", (int)(progress_ratio * 100));
        
        int text_x = comp->x + (comp->w - strlen(percentage_text) * 8) / 2;
        int text_y = comp->y + (comp->h - 16) / 2;
        gui_render_draw_text(text_x, text_y, percentage_text, theme->text_color, 0xFFFFFFFF);
    }
}


/* Component Update Helpers */
void modern_window_update(modern_component_t* comp) {
    if (!comp) return;
    modern_window_t* window = (modern_window_t*)comp;
    if (window->title_label) {
        window->title_label->x = comp->x + comp->border_radius + 5;
        window->title_label->y = comp->y + 5;
    }
    if (window->close_button) {
        window->close_button->x = comp->x + comp->w - 25;
        window->close_button->y = comp->y + 5;
    }
    if (window->minimize_button) {
        window->minimize_button->x = comp->x + comp->w - 70;
        window->minimize_button->y = comp->y + 5;
    }
    if (window->maximize_button) {
        window->maximize_button->x = comp->x + comp->w - 45;
        window->maximize_button->y = comp->y + 5;
    }
    window->content_x = comp->x + comp->border_radius;
    window->content_y = comp->y + 30;
    window->content_w = comp->w - (comp->border_radius * 2);
    window->content_h = comp->h - 30 - comp->border_radius;
    if (g_drag_state.active && g_drag_state.comp == comp && g_drag_state.resizing) {
        if (window->title_label) {
            window->title_label->visible = true;
        }
    }
}

void modern_button_update(modern_component_t* comp) {
    if (!comp) return;
    if (comp->disabled) {
        comp->hover = false;
        comp->pressed = false;
    }
}

void modern_text_field_update(modern_component_t* comp) {
    if (!comp) return;
    modern_text_field_t* text_field = (modern_text_field_t*)comp;
    int len = (int)strlen(text_field->text);
    if (text_field->cursor_position < 0) text_field->cursor_position = 0;
    if (text_field->cursor_position > len) text_field->cursor_position = len;
    if (text_field->max_length <= 0) text_field->max_length = 1;
}

void modern_progress_bar_update(modern_component_t* comp) {
    if (!comp) return;
    modern_progress_bar_t* progress = (modern_progress_bar_t*)comp;
    if (progress->max_value < progress->min_value) {
        float tmp = progress->min_value;
        progress->min_value = progress->max_value;
        progress->max_value = tmp;
    }
    if (progress->value < progress->min_value) progress->value = progress->min_value;
    if (progress->value > progress->max_value) progress->value = progress->max_value;
}

/* Update UI System */
void modern_ui_update(void) {
    uint64_t start_time = get_timer_ticks();
    
    // Update animations
    modern_ui_update_animations();
    
    // Update all components
    if (g_ui_manager.root_component) {
        modern_ui_update_component_recursive(g_ui_manager.root_component);
    }
    
    // Handle mouse hover states
    modern_ui_handle_mouse_hover();
    
    uint64_t end_time = get_timer_ticks();
    g_ui_manager.frame_time = end_time - start_time;
}

/* Update Component Recursively */
void modern_ui_update_component_recursive(modern_component_t* comp) {
    if (!comp) return;
    
    // Update this component
    if (comp->update) {
        comp->update(comp);
    }
    
    // Update children
    for (int i = 0; i < comp->child_count; i++) {
        modern_ui_update_component_recursive(comp->children[i]);
    }
}

/* Handle Mouse Hover */
void modern_ui_handle_mouse_hover(void) {
    minimal_mouse_t* mouse = minimal_mouse_get_state();
    
    // Clear previous hover states
    if (g_ui_manager.hovered_component) {
        g_ui_manager.hovered_component->hover = false;
        g_ui_manager.hovered_component = NULL;
    }
    
    // Find component under mouse
    modern_component_t* comp = modern_ui_get_component_at(mouse->x, mouse->y);
    if (comp && comp->enabled) {
        comp->hover = true;
        g_ui_manager.hovered_component = comp;
        
        // Call hover handler
        if (comp->on_hover) {
            comp->on_hover(comp, true);
        }
    }
}

/* Get Component at Position */
modern_component_t* modern_ui_get_component_at(int x, int y) {
    if (!g_ui_manager.root_component) return NULL;
    return modern_ui_find_component_recursive(g_ui_manager.root_component, x, y);
}

/* Find Component Recursively */
modern_component_t* modern_ui_find_component_recursive(modern_component_t* comp, int x, int y) {
    if (!comp || !comp->visible) return NULL;
    
    // Check if point is within this component
    if (x >= comp->x && x < comp->x + comp->w &&
        y >= comp->y && y < comp->y + comp->h) {
        
        // Check children first (they're on top)
        for (int i = 0; i < comp->child_count; i++) {
            modern_component_t* child = modern_ui_find_component_recursive(comp->children[i], x, y);
            if (child) return child;
        }
        
        return comp;
    }
    
    return NULL;
}

/* Update Animations */
void modern_ui_update_animations(void) {
    uint64_t current_time = get_timer_ticks();
    g_ui_manager.animation_time = current_time;
    
    for (int i = 0; i < g_ui_manager.active_animations; i++) {
        ui_animation_t* anim = &g_ui_manager.animations[i];
        if (anim->active) {
            // Calculate progress
            uint64_t elapsed = current_time - anim->start_time;
            anim->progress = (float)elapsed / anim->duration_ms;
            
            if (anim->progress >= 1.0f) {
                anim->progress = 1.0f;
                anim->active = false;
                
                // Call completion callback
                if (anim->on_complete) {
                    anim->on_complete(anim->component);
                }
            }
            
            // Call update callback
            if (anim->on_update) {
                anim->on_update(anim->component, anim->progress);
            }
        }
    }
}

/* Get Current Theme */
modern_theme_t* modern_ui_get_theme(void) {
    return g_ui_manager.current_theme;
}
