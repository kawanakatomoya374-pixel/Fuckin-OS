/**
 * Keyboard driver for C-OS 4.0.8 alpha
 *
 * PS/2 set 1 scancode handling with a small event queue,
 * modifier tracking, and compatibility helpers for older GUI code.
 * 
 * 修正内容:
 * - バックスペース長押し検出機能追加
 * - キーリピート処理の安定化
 */

#include <keyboard.h>
#include <io.h>
#include <serial.h>
#include <string.h>
#include <idt.h>
#include <timer.h>
#include <irq.h>

#define KB_BUF_SIZE 128
#define KB_RELEASE  0x80

/* バックスペース長押し検出設定 */
#define BACKSPACE_HOLD_THRESHOLD  3   /* 3回押下で長押しと判定 */
#define BACKSPACE_DELETE_ALL      10  /* 10回押下で全文削除 */

static keyboard_event_t kb_buf[KB_BUF_SIZE];
static int kb_head = 0;
static int kb_tail = 0;
static bool kb_initialized = false;
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;
static bool num_lock = false;
static bool e0_prefix = false;
static keyboard_callback_t kb_callback = NULL;

/* バックスペース長押し検出状態 */
static uint8_t backspace_count = 0;        /* 連続押下回数 */
static uint32_t backspace_last_time = 0;   /* 最後の押下時刻 */
static bool backspace_delete_all_mode = false; /* 全文削除モード */

/* キーリピート状態 */
static bool repeat_enabled = true;
static uint16_t repeat_delay_ms = 250;     /* 初期遅延 */
static uint16_t repeat_rate_ms = 50;       /* 繰り返し間隔 */

static const char scancode_map[128] = {
    0, 27,
    '1','2','3','4','5','6','7','8','9','0','-','=',
    '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,
    '\\','z','x','c','v','b','n','m',',','.','/',
    0,
    '*',
    0,
    ' ',
    0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,
    0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0
};

static const char scancode_map_shift[128] = {
    0, 27,
    '!','@','#','$','%','^','&','*','(',')','_','+',
    '\b', '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,
    '|','Z','X','C','V','B','N','M','<','>','?',
    0,
    '*',
    0,
    ' ',
    0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0,0,0,
    0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0
};

/* ブラウザURL編集用拡張状態 */
static bool browser_url_edit_mode = false;    /* URL編集モード */
static uint8_t url_backspace_count = 0;       /* URL編集時のバックスペース回数 */

/* キーリピートタイマー状態 */
static bool repeat_active = false;
static uint8_t repeat_scancode = 0;
static char repeat_ascii = 0;
static uint32_t repeat_next_time = 0;

static uint8_t kb_modifiers(void) {
    uint8_t mods = 0;
    if (shift_pressed) mods |= KEYBOARD_MOD_SHIFT;
    if (ctrl_pressed)  mods |= KEYBOARD_MOD_CTRL;
    if (alt_pressed)   mods |= KEYBOARD_MOD_ALT;
    if (caps_lock)     mods |= KEYBOARD_MOD_CAPS;
    if (num_lock)      mods |= KEYBOARD_MOD_NUM;
    return mods;
}

static void kb_flush_controller(void) {
    for (int i = 0; i < 256; ++i) {
        if ((inb(0x64) & 0x01) == 0) {
            break;
        }
        (void)inb(0x60);
    }
}

static char kb_translate(uint8_t scancode) {
    if (scancode >= 128) return 0;

    char c = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
    if (!c) return 0;

    if (caps_lock && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if (caps_lock && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return c;
}

/* ブラウザURL編集モードの設定 */
void keyboard_set_url_edit_mode(bool enabled) {
    browser_url_edit_mode = enabled;
    if (!enabled) {
        url_backspace_count = 0;
        backspace_delete_all_mode = false;
    }
}

/* ブラウザURL編集モードの取得 */
bool keyboard_get_url_edit_mode(void) {
    return browser_url_edit_mode;
}

/* URL編集時のバックスペース回数の取得 */
uint8_t keyboard_get_url_backspace_count(void) {
    return url_backspace_count;
}

/* URL編集時のバックスペースカウントをリセット */
void keyboard_reset_url_backspace_count(void) {
    url_backspace_count = 0;
    backspace_delete_all_mode = false;
}

/* バックスペース長押しで全文削除が必要な状態かチェック */
bool keyboard_should_delete_all_url(void) {
    return backspace_delete_all_mode;
}

/* 全文削除モードのリセット */
void keyboard_clear_delete_all_mode(void) {
    backspace_delete_all_mode = false;
    url_backspace_count = 0;
}

/* キーリピート設定 */
void keyboard_set_repeat(bool enabled) {
    repeat_enabled = enabled;
}

void keyboard_set_repeat_rate(uint16_t delay_ms, uint16_t rate_ms) {
    repeat_delay_ms = delay_ms;
    repeat_rate_ms = rate_ms;
}

/* キーリピートイベントの生成 */
static void kb_generate_repeat_event(void) {
    if (!repeat_active || !repeat_enabled) return;
    if (repeat_scancode == 0) return;

    keyboard_event_t ev;
    ev.scancode = repeat_scancode;
    ev.ascii = repeat_ascii;
    ev.modifiers = kb_modifiers();
    ev.pressed = 1;
    ev.shift = shift_pressed;
    ev.ctrl = ctrl_pressed;
    ev.alt = alt_pressed;
    ev.extended = false;

    int next = (kb_tail + 1) % KB_BUF_SIZE;
    if (next != kb_head) {
        kb_buf[kb_tail] = ev;
        kb_tail = next;
        if (kb_callback) {
            kb_callback(&ev);
        }
    }
}

/* キーリピート更新（タイマー割り込みから呼び出す） */
void keyboard_update_repeat(uint32_t current_time) {
    if (!repeat_active || !repeat_enabled) return;

    if ((int32_t)(current_time - repeat_next_time) < 0) {
        return;
    }

    if (repeat_scancode == KEYBOARD_SCANCODE_BACKSPACE) {
        /* バックスペースの場合、リピート処理を追加 */

        /* URL編集モードでバックスペース長押しを検出 */
        if (browser_url_edit_mode) {
            url_backspace_count++;

            /* 閾値を超えたら全文削除モードに */
            if (url_backspace_count >= BACKSPACE_DELETE_ALL) {
                backspace_delete_all_mode = true;
            }
        }

        /* リピートイベントを生成 */
        kb_generate_repeat_event();
    } else {
        /* 他のキーは通常のリピート */
        kb_generate_repeat_event();
    }

    repeat_next_time = current_time + repeat_rate_ms;
}

static void kb_push(uint8_t scancode, char ascii, bool pressed, bool extended) {
    keyboard_event_t ev;
    ev.scancode = scancode;
    ev.ascii = ascii;
    ev.modifiers = kb_modifiers();
    ev.pressed = pressed ? 1 : 0;
    ev.shift = shift_pressed;
    ev.ctrl = ctrl_pressed;
    ev.alt = alt_pressed;
    ev.extended = extended;

    int next = (kb_tail + 1) % KB_BUF_SIZE;
    if (next == kb_head) {
        return; /* drop event on overflow */
    }

    kb_buf[kb_tail] = ev;
    kb_tail = next;
    /* WARNING: kb_callback, if ever registered, runs synchronously
     * here - inside the keyboard IRQ handler. Nothing in this tree
     * currently calls keyboard_set_callback()/registers one, so this
     * is dead code today, but if that ever changes: the callback must
     * never call mutex_lock() or anything that can block. If it does,
     * and the lock happens to already be held by whatever this IRQ
     * interrupted, the IRQ handler spins forever waiting for a lock
     * whose owner can't run again until this handler returns -
     * deadlocking the whole system. Push into a queue and let a
     * regular (non-IRQ) thread drain it instead. */
    if (kb_callback) {
        kb_callback(&ev);
    }
}

static void kb_start_repeat(uint8_t scancode, char ascii) {
    repeat_active = true;
    repeat_scancode = scancode;
    repeat_ascii = ascii;
    repeat_next_time = (uint32_t)get_timer_ticks() + repeat_delay_ms;
}

static void kb_stop_repeat(void) {
    repeat_active = false;
    repeat_scancode = 0;
    repeat_ascii = 0;
}

static void kb_handle_press(uint8_t code) {
    if (e0_prefix) {
        if (code == KEYBOARD_SCANCODE_RCTRL) {
            ctrl_pressed = true;
            e0_prefix = false;
            return;
        }
        if (code == KEYBOARD_SCANCODE_RALT) {
            alt_pressed = true;
            /* Alt is normally a pure modifier, but the GUI consumes its
             * press as the secondary-cursor left-click trigger. */
            kb_push(code, 0, true, true);
            e0_prefix = false;
            return;
        }
    }

    switch (code) {
        case KEYBOARD_SCANCODE_LSHIFT:
        case KEYBOARD_SCANCODE_RSHIFT:
            shift_pressed = true;
            e0_prefix = false;
            return;
        case KEYBOARD_SCANCODE_LCTRL:
            ctrl_pressed = true;
            e0_prefix = false;
            return;
        case KEYBOARD_SCANCODE_LALT:
            alt_pressed = true;
            /* Preserve a press event for multi-cursor Alt-click handling. */
            kb_push(code, 0, true, false);
            e0_prefix = false;
            return;
        case KEYBOARD_SCANCODE_CAPSLOCK:
            caps_lock = !caps_lock;
            e0_prefix = false;
            return;
        case KEYBOARD_SCANCODE_NUMLOCK:
            num_lock = !num_lock;
            e0_prefix = false;
            return;
        default:
            break;
    }

    /* URL編集モードでバックスペース押下を検出 */
    if (code == KEYBOARD_SCANCODE_BACKSPACE) {
        uint32_t now = get_timer_ticks();
        if ((uint32_t)(now - backspace_last_time) <= 250) {
            backspace_count++;
        } else {
            backspace_count = 1;
        }
        backspace_last_time = now;

        if (browser_url_edit_mode) {
            url_backspace_count++;
            /* 閾値を超えたら全文削除モードに */
            if (url_backspace_count >= BACKSPACE_DELETE_ALL) {
                backspace_delete_all_mode = true;
            }
        }
    } else {
        backspace_count = 0;
    }

    kb_push(code, e0_prefix ? 0 : kb_translate(code), true, e0_prefix);
    bool was_extended = e0_prefix;
    e0_prefix = false;
    
    /* 通常キーの場合、キーリピートを開始 */
    if (code != KEYBOARD_SCANCODE_LSHIFT && 
        code != KEYBOARD_SCANCODE_RSHIFT &&
        code != KEYBOARD_SCANCODE_LCTRL &&
        code != KEYBOARD_SCANCODE_LALT) {
        kb_start_repeat(code, was_extended ? 0 : kb_translate(code));
    }
}

static void kb_handle_release(uint8_t code) {
    if (e0_prefix) {
        if (code == KEYBOARD_SCANCODE_RCTRL) {
            ctrl_pressed = false;
            e0_prefix = false;
            return;
        }
        if (code == KEYBOARD_SCANCODE_RALT) {
            alt_pressed = false;
            /* Pair the modifier press event with a release event so the
             * secondary-cursor click latch can accept the next Alt press. */
            kb_push(code, 0, false, true);
            e0_prefix = false;
            return;
        }
    }

    switch (code) {
        case KEYBOARD_SCANCODE_LSHIFT:
        case KEYBOARD_SCANCODE_RSHIFT:
            shift_pressed = false;
            break;
        case KEYBOARD_SCANCODE_LCTRL:
            ctrl_pressed = false;
            break;
        case KEYBOARD_SCANCODE_LALT:
            alt_pressed = false;
            /* The GUI treats Alt as a click button in multi-cursor mode;
             * release must be observable just like the earlier press. */
            kb_push(code, 0, false, false);
            break;
        default:
            break;
    }
    if (code == KEYBOARD_SCANCODE_BACKSPACE) {
        backspace_count = 0;
    }
    e0_prefix = false;
    
    /* キーリピートを停止 */
    if (code == repeat_scancode) {
        kb_stop_repeat();
    }
}

void keyboard_init(void) {
    kb_head = kb_tail = 0;
    shift_pressed = false;
    ctrl_pressed = false;
    alt_pressed = false;
    caps_lock = false;
    num_lock = false;
    e0_prefix = false;
    kb_callback = NULL;
    kb_initialized = true;
    kb_flush_controller();
    
    /* 拡張状態のリセット */
    backspace_count = 0;
    backspace_last_time = 0;
    backspace_delete_all_mode = false;
    browser_url_edit_mode = false;
    url_backspace_count = 0;
    repeat_active = false;
    irq_clear_mask(IRQ_KEYBOARD);
    
    serial_puts("[KEYBOARD] Keyboard initialized\n");
}

void keyboard_irq_handler(struct regs *r) {
    (void)r;
    if (!kb_initialized) return;

    uint8_t status = inb(0x64);
    if ((status & 0x01) == 0) {
        return;
    }

    /* Leave mouse/aux bytes alone so the mouse driver can consume them. */
    if (status & 0x20) {
        return;
    }

    uint8_t sc = inb(0x60);
    if (sc == 0x00 || sc == 0xFA || sc == 0xFE || sc == 0xFF || sc == 0xFC) {
        return;
    }
    if (sc == 0xE0) {
        e0_prefix = true;
        return;
    }
    if (sc == 0xE1) {
        e0_prefix = false;
        return;
    }

    bool released = (sc & KB_RELEASE) != 0;
    uint8_t code = (uint8_t)(sc & (uint8_t)~KB_RELEASE);

    if (released) kb_handle_release(code);
    else kb_handle_press(code);
}

void keyboard_interrupt_handler(void) {
    keyboard_irq_handler(NULL);
}

/* USB HID keyboard reports arrive as full "which keys are down right
 * now" snapshots, not per-key make/break events, so tusb_hid_bridge.c
 * diffs successive reports itself and calls this once per key that
 * changed state - already translated to the same PS/2 Set-1 code
 * space kb_handle_press()/kb_handle_release() expect (see
 * usb_hid_to_ps2_scancode() in that file). Reusing those two
 * functions means modifier tracking, key repeat and ASCII translation
 * stay in one place regardless of which bus a key came in on.
 *
 * This runs from usb_poll() in ordinary polled context while PS/2
 * IRQ1 can still fire and touch the same e0_prefix/kb_buf state, so
 * it's wrapped in the same save-EFLAGS/cli/.../restore-EFLAGS
 * critical section mouse_apply_usb_report() uses for the same
 * reason. */
void keyboard_inject_scancode(uint8_t code, bool pressed, bool extended) {
    if (!kb_initialized) return;
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    e0_prefix = extended;
    if (pressed) {
        kb_handle_press(code);
        /* USB HID reports are state snapshots and do not provide the PS/2
         * controller's typematic stream. Starting the legacy timer repeat
         * here turns even a short host-injected press into many characters
         * before the next HID release report is polled. Native USB repeat
         * can be implemented from HID timing later; preserve exact one-shot
         * press semantics now. */
        kb_stop_repeat();
    } else {
        kb_handle_release(code);
    }
    __asm__ volatile("push %0\n\tpopfq" :: "r"(flags) : "memory", "cc");
}

bool keyboard_has_event(void) {
    return kb_head != kb_tail;
}

keyboard_event_t keyboard_get_event(void) {
    keyboard_event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (kb_head != kb_tail) {
        ev = kb_buf[kb_head];
        kb_head = (kb_head + 1) % KB_BUF_SIZE;
    }
    return ev;
}

char keyboard_get_char(void) {
    return keyboard_get_event().ascii;
}

uint8_t keyboard_get_scancode(void) {
    return keyboard_get_event().scancode;
}

char keyboard_get_ascii(void) {
    return keyboard_get_event().ascii;
}

bool keyboard_has_key(void) {
    return keyboard_has_event();
}

key_event_t keyboard_get_key(void) {
    return keyboard_get_event();
}

void keyboard_flush(void) {
    kb_head = kb_tail = 0;
    kb_stop_repeat();
    kb_flush_controller();
}

void keyboard_poll(void) {
    if (!kb_initialized) return;

    /* Compatibility shim: hardware input now arrives through IRQ1.
     * Key repeat is driven by the timer interrupt.
     */
}

void keyboard_install_callback(keyboard_callback_t callback) {
    kb_callback = callback;
}

void keyboard_remove_callback(keyboard_callback_t callback) {
    if (kb_callback == callback) kb_callback = NULL;
}

bool keyboard_is_shift_pressed(void) { return shift_pressed; }
bool keyboard_is_ctrl_pressed(void)  { return ctrl_pressed; }
bool keyboard_is_alt_pressed(void)   { return alt_pressed; }
bool keyboard_is_caps_locked(void)    { return caps_lock; }
bool keyboard_is_num_locked(void)     { return num_lock; }

/* ブラウザ向け拡張関数 */
bool keyboard_is_url_edit_mode(void) {
    return browser_url_edit_mode;
}

void keyboard_enter_url_edit_mode(void) {
    browser_url_edit_mode = true;
    url_backspace_count = 0;
    backspace_delete_all_mode = false;
}

void keyboard_exit_url_edit_mode(void) {
    browser_url_edit_mode = false;
    url_backspace_count = 0;
    backspace_delete_all_mode = false;
}

bool keyboard_is_delete_all_pending(void) {
    return backspace_delete_all_mode;
}

void keyboard_ack_delete_all(void) {
    backspace_delete_all_mode = false;
    url_backspace_count = 0;
}

/* キーリピート状態取得 */
bool keyboard_is_repeating(void) {
    return repeat_active;
}

uint8_t keyboard_get_repeat_scancode(void) {
    return repeat_scancode;
}

/* 拡張状態チェック関数（ブラウザが呼び出す） */
bool keyboard_check_backspace_hold(int threshold) {
    return url_backspace_count >= threshold;
}

uint32_t keyboard_get_backspace_count(void) {
    return backspace_count;
}

/* タイマーからの呼び出し（キーリピート処理用） */
void keyboard_timer_tick(uint32_t ticks) {
    if (repeat_active && repeat_enabled) {
        keyboard_update_repeat(ticks);
    }
}