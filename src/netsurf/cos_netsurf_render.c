/*
 * cos_netsurf_render.c - walks the real DOM tree produced by the real
 * NetSurf content pipeline and extracts a simple, linear sequence of
 * display lines (title / headings / paragraphs / links) that GUI code
 * can show without needing to know anything about libdom.
 *
 * This is deliberately NOT a port of content/handlers/html/box_construct.c
 * (Tier B - see PORTING_NOTES.md): there is no CSS box model and no
 * layout. What this *is*: real fetch (cos_fetch_http.c / data.c) ->
 * real HTML5 parse via libhubbub into a real DOM via libdom (html.c,
 * Tier A) -> this file walking that real dom_document with libdom's
 * own official tree-walker (dom/walk.h) to produce text. Every byte
 * this emits came from the real engine's parse of the real page, not
 * from a hand-rolled tag stripper - that's the specific gap between
 * "NetSurf label" and "NetSurf engine" this closes. See
 * cos_netsurf_render_page()'s doc comment in cos_netsurf.h for the
 * exact contract GUI callers (gui_apps_browser.c) get.
 */
#include <string.h>
#include <stdbool.h>

#include "utils/errors.h"
#include "utils/corestrings.h" /* corestring_dom_href */
#include "utils/libdom.h"   /* pulls in <dom/dom.h> - dom_document,
                              * dom_node, dom_element, dom_characterdata
                              * etc. */
#include "dom/walk.h"        /* libdom_treewalk() */

#include "content/hlcache.h"
#include "netsurf/content.h" /* content_get_type()/content_get_title() -
                               * the PUBLIC header (netsurf/include/
                               * netsurf/content.h), found via
                               * -I$(NS_DIR)/include. Not the same file
                               * as content/content.h (the internal
                               * one, found via -I$(NS_DIR)), which
                               * does not declare either function. */
#include "content/handlers/html/html_save.h" /* html_get_document() */

#include "cos_netsurf_render.h"
#include "cos_netsurf_internal.h"
#include "serial.h"

/* ---- small helpers ---------------------------------------------- */

/* Case-insensitive match of a dom_string element/tag name against an
 * upper-case C literal, e.g. tag_is(name, "SCRIPT"). Written this way
 * (rather than trusting libdom to hand back a particular case)
 * because HTML tag-name casing isn't guaranteed by any layer this
 * file can see from here. */
static bool tag_is(dom_string *name, const char *upper_literal)
{
    if (name == NULL) {
        return false;
    }

    size_t llen = strlen(upper_literal);
    if (dom_string_byte_length(name) != llen) {
        return false;
    }

    const char *d = dom_string_data(name);
    for (size_t i = 0; i < llen; i++) {
        char a = d[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 32);
        }
        if (a != upper_literal[i]) {
            return false;
        }
    }
    return true;
}

/* Appends `add` (an arbitrary run of raw text-node bytes, which may
 * contain newlines/tabs/runs of spaces) onto `buf` (a `*len`-tracked
 * buffer of size `cap`), collapsing all whitespace to single spaces
 * and never emitting a leading space into an empty buffer or a
 * doubled space after one already there. Silently truncates rather
 * than overflowing if `add` would exceed `cap` - long paragraphs get
 * cut off, which is an acceptable degradation for a text-mode view. */
static void append_collapsed(char *buf, size_t cap, size_t *len,
                              const char *add, size_t add_len)
{
    for (size_t i = 0; i < add_len; i++) {
        unsigned char ch = (unsigned char)add[i];
        bool is_ws = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                      ch == '\f' || ch == '\v');

        if (is_ws) {
            if (*len == 0 || buf[*len - 1] == ' ') {
                continue; /* skip leading/duplicate whitespace */
            }
            ch = ' ';
        }

        if (*len + 1 >= cap) {
            return; /* truncate, leaving room for the NUL */
        }
        buf[*len] = (char)ch;
        (*len)++;
    }
    buf[*len] = '\0';
}

/* ---- walk state ---------------------------------------------------
 * See the design note in this file's header: one "current" block
 * accumulator (plain text or a heading, tracked by cur_kind/
 * cur_heading_level) plus a separate accumulator specifically for
 * in-progress anchor text, since an <a> can appear mid-paragraph and
 * its label needs to become its own COS_NS_LINE_LINK line without
 * losing whatever text came before/will come after it in the
 * enclosing block. */

struct render_ctx {
    cos_ns_render_line_t *out;
    int max_lines;
    int count;

    cos_ns_line_kind_t cur_kind;   /* COS_NS_LINE_TEXT or _HEADING while
                                     * accumulating; meaningless when
                                     * cur_len == 0 */
    int cur_heading_level;
    char cur_buf[COS_NS_LINE_TEXT_MAX];
    size_t cur_len;

    bool in_anchor;
    char anchor_href[COS_NS_LINE_HREF_MAX];
    char anchor_buf[COS_NS_LINE_TEXT_MAX];
    size_t anchor_len;
};

static void emit_line(struct render_ctx *ctx, cos_ns_line_kind_t kind,
                       int heading_level, const char *text, const char *href)
{
    if (ctx->count >= ctx->max_lines) {
        return;
    }
    cos_ns_render_line_t *line = &ctx->out[ctx->count];
    line->kind = kind;
    line->heading_level = heading_level;
    line->text[0] = '\0';
    line->href[0] = '\0';
    if (text != NULL) {
        cos_strlcpy(line->text, text, sizeof(line->text));
    }
    if (href != NULL) {
        cos_strlcpy(line->href, href, sizeof(line->href));
    }
    ctx->count++;
}

/* Flushes whatever plain-text/heading accumulator is pending (see
 * struct render_ctx's comment) as its own line, if there's anything
 * in it. Safe to call when there's nothing pending - it's a no-op. */
static void flush_cur(struct render_ctx *ctx)
{
    if (ctx->cur_len == 0) {
        return;
    }
    emit_line(ctx, ctx->cur_kind, ctx->cur_heading_level, ctx->cur_buf, NULL);
    ctx->cur_len = 0;
    ctx->cur_buf[0] = '\0';
    ctx->cur_kind = COS_NS_LINE_TEXT;
    ctx->cur_heading_level = 0;
}

static int heading_level_of(dom_string *name)
{
    if (tag_is(name, "H1")) return 1;
    if (tag_is(name, "H2")) return 2;
    if (tag_is(name, "H3")) return 3;
    if (tag_is(name, "H4")) return 4;
    if (tag_is(name, "H5")) return 5;
    if (tag_is(name, "H6")) return 6;
    return 0;
}

/* Block-level elements: entering one always ends whatever text/
 * heading was accumulating immediately before it. This list isn't
 * exhaustive HTML5 (no <table>-internal distinctions, no <details>
 * etc.) - it only needs to cover the common cases well enough for a
 * readable plain-text rendering, not reproduce CSS 'display' values,
 * which is what Tier B would be for. */
static bool is_block_boundary(dom_string *name)
{
    static const char *const tags[] = {
        "P", "DIV", "SECTION", "ARTICLE", "HEADER", "FOOTER", "NAV",
        "ASIDE", "MAIN", "FIGURE", "FIGCAPTION", "BLOCKQUOTE", "PRE",
        "UL", "OL", "LI", "TABLE", "TR", "TD", "TH", "THEAD", "TBODY",
        "TFOOT", "FORM", "FIELDSET", "ADDRESS", "DL", "DT", "DD",
        "BR", "HR",
        NULL
    };
    for (int i = 0; tags[i] != NULL; i++) {
        if (tag_is(name, tags[i])) {
            return true;
        }
    }
    return false;
}

static enum dom_walk_cmd render_walk_cb(enum dom_walk_stage stage,
                                         dom_node_type type, dom_node *node,
                                         void *pw)
{
    struct render_ctx *ctx = (struct render_ctx *)pw;

    if (ctx->count >= ctx->max_lines) {
        return DOM_WALK_CMD_ABORT;
    }

    if (type == DOM_TEXT_NODE) {
        if (stage != DOM_WALK_STAGE_ENTER) {
            return DOM_WALK_CMD_CONTINUE;
        }
        dom_string *data = NULL;
        if (dom_characterdata_get_data(node, &data) != DOM_NO_ERR ||
                data == NULL) {
            return DOM_WALK_CMD_CONTINUE;
        }
        const char *bytes = dom_string_data(data);
        size_t blen = dom_string_byte_length(data);

        if (ctx->in_anchor) {
            append_collapsed(ctx->anchor_buf, sizeof(ctx->anchor_buf),
                              &ctx->anchor_len, bytes, blen);
        } else {
            append_collapsed(ctx->cur_buf, sizeof(ctx->cur_buf),
                              &ctx->cur_len, bytes, blen);
        }
        dom_string_unref(data);
        return DOM_WALK_CMD_CONTINUE;
    }

    if (type != DOM_ELEMENT_NODE) {
        return DOM_WALK_CMD_CONTINUE;
    }

    dom_string *name = NULL;
    if (dom_node_get_node_name(node, &name) != DOM_NO_ERR || name == NULL) {
        return DOM_WALK_CMD_CONTINUE;
    }

    enum dom_walk_cmd result = DOM_WALK_CMD_CONTINUE;

    /* Subtrees whose text content should never reach the page: script
     * source, stylesheet text, anything still stuck in <head> if we
     * ended up walking from the document root (see the no-<body>
     * fallback in cos_netsurf_render_page() below). */
    if (tag_is(name, "SCRIPT") || tag_is(name, "STYLE") ||
            tag_is(name, "HEAD") || tag_is(name, "NOSCRIPT") ||
            tag_is(name, "TEMPLATE") || tag_is(name, "TITLE")) {
        if (stage == DOM_WALK_STAGE_ENTER) {
            flush_cur(ctx);
            result = DOM_WALK_CMD_SKIP;
        }
        dom_string_unref(name);
        return result;
    }

    if (tag_is(name, "A")) {
        if (stage == DOM_WALK_STAGE_ENTER) {
            flush_cur(ctx);
            ctx->in_anchor = true;
            ctx->anchor_len = 0;
            ctx->anchor_buf[0] = '\0';
            ctx->anchor_href[0] = '\0';

            dom_string *href_val = NULL;
            if (dom_element_get_attribute(node, corestring_dom_href,
                        &href_val) == DOM_NO_ERR && href_val != NULL) {
                size_t hl = dom_string_byte_length(href_val);
                if (hl >= sizeof(ctx->anchor_href)) {
                    hl = sizeof(ctx->anchor_href) - 1;
                }
                memcpy(ctx->anchor_href, dom_string_data(href_val), hl);
                ctx->anchor_href[hl] = '\0';
                dom_string_unref(href_val);
            }
        } else { /* LEAVE */
            if (ctx->in_anchor) {
                emit_line(ctx, COS_NS_LINE_LINK, 0,
                          (ctx->anchor_len > 0) ? ctx->anchor_buf : "[link]",
                          ctx->anchor_href);
                ctx->in_anchor = false;
                ctx->anchor_len = 0;
            }
        }
        dom_string_unref(name);
        return DOM_WALK_CMD_CONTINUE;
    }

    int hlevel = heading_level_of(name);
    if (hlevel > 0) {
        if (stage == DOM_WALK_STAGE_ENTER) {
            flush_cur(ctx);
            ctx->cur_kind = COS_NS_LINE_HEADING;
            ctx->cur_heading_level = hlevel;
        } else { /* LEAVE - see the design note: this is a safety net
                   * for headings immediately followed by loose text
                   * with no intervening element; the common case is
                   * already handled by the next block boundary's own
                   * flush_cur() at ENTER. */
            flush_cur(ctx);
        }
        dom_string_unref(name);
        return DOM_WALK_CMD_CONTINUE;
    }

    if (stage == DOM_WALK_STAGE_ENTER && is_block_boundary(name)) {
        flush_cur(ctx);
        if (tag_is(name, "HR")) {
            emit_line(ctx, COS_NS_LINE_BLANK, 0, NULL, NULL);
        }
    }

    dom_string_unref(name);
    return DOM_WALK_CMD_CONTINUE;
}

/* ---- entry point --------------------------------------------------- */

int cos_netsurf_render_page(const char *url_string,
                             cos_ns_render_line_t *out_lines, int max_lines,
                             char *out_error, size_t error_sz)
{
    if (url_string == NULL || out_lines == NULL || max_lines <= 0) {
        if (out_error != NULL) {
            cos_strlcpy(out_error, "bad arguments", error_sz);
        }
        return -1;
    }

    hlcache_handle *handle = NULL;
    nserror err = cos_netsurf_load_url_sync_for_render(url_string, &handle,
                                                         out_error, error_sz);
    if (err != NSERROR_OK) {
        return -1;
    }

    if (content_get_type(handle) != CONTENT_HTML) {
        if (out_error != NULL) {
            cos_strlcpy(out_error,
                "not HTML - the real engine only renders HTML content "
                "so far (see cos_netsurf_render.c)", error_sz);
        }
        hlcache_handle_release(handle);
        return -1;
    }

    dom_document *doc = html_get_document(handle);
    if (doc == NULL) {
        if (out_error != NULL) {
            cos_strlcpy(out_error, "page finished loading but produced "
                        "no DOM (internal error)", error_sz);
        }
        hlcache_handle_release(handle);
        return -1;
    }

    struct render_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out_lines;
    ctx.max_lines = max_lines;
    ctx.cur_kind = COS_NS_LINE_TEXT;

    const char *title = content_get_title(handle);
    if (title != NULL && title[0] != '\0') {
        emit_line(&ctx, COS_NS_LINE_TITLE, 0, title, NULL);
    }

    dom_html_element *body = NULL;
    dom_html_document_get_body(doc, &body);

    dom_node *walk_root = (body != NULL) ? (dom_node *)body
                                          : (dom_node *)doc;

    /* DOM_WALK_ENABLE_ALL: LEAVE is needed for anchors (to know where
     * a link's label ends) and, as a safety net, for headings - see
     * render_walk_cb()'s comments on both. */
    libdom_treewalk(DOM_WALK_ENABLE_ALL, render_walk_cb, walk_root, &ctx);

    /* Anything still pending when the walk ends (e.g. a page that's
     * just one trailing paragraph with no closing block element) */
    flush_cur(&ctx);
    if (ctx.in_anchor && ctx.anchor_len > 0 && ctx.count < ctx.max_lines) {
        emit_line(&ctx, COS_NS_LINE_LINK, 0, ctx.anchor_buf,
                  ctx.anchor_href);
    }

    hlcache_handle_release(handle);

    serial_puts("[NetSurf] real DOM walked and rendered to lines for "
                "GUI display\n");

    return ctx.count;
}
