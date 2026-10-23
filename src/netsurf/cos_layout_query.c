/**
 * cos_layout_query.c - expose NetSurf's laid-out box tree to script.
 *
 * Layout-dependent DOM members (getBoundingClientRect, offsetWidth/Height,
 * clientWidth/Height, offsetTop/Left) cannot be synthesised from libdom: the
 * geometry only exists after NetSurf has run layout, in the box tree that
 * hangs off the current html_content. A probe of the live runtime confirmed
 * every one of those members was simply absent from the element prototype.
 *
 * Returning plausible zeros instead would be worse than absence, because
 * feature-detection code takes the "layout is available" branch and then
 * misbehaves - so these report failure honestly when there is no box (display
 * :none, not yet laid out, detached node) and the binding then returns an
 * all-zero rect exactly as a real browser does for an unrendered element.
 */
#include <stdbool.h>
#include <stddef.h>

#include "utils/errors.h"
#include "netsurf/browser_window.h"
#include "content/hlcache.h"
#include "html/box.h"
#include "html/box_inspect.h"
/* html_save.h declares html_get_box_tree() but assumes libdom types are
 * already visible, so include the DOM headers first. */
#include "dom/dom.h"
#include "html/html_save.h"

extern struct browser_window *cos_netsurf_browser_window(void);
extern void cos_netsurf_window_get_scroll_offsets(int *scroll_x, int *scroll_y);

/* Depth-first search for the box whose originating DOM node is `node`.
 * NetSurf can generate several boxes for one element (anonymous boxes, inline
 * splits); the first match in document order is the principal box, which is
 * what the CSSOM geometry is defined against. */
static struct box *cos_box_for_node(struct box *box, void *node)
{
	if (box == NULL) return NULL;
	if (box->node == (struct dom_node *)node) return box;
	for (struct box *child = box->children; child != NULL; child = child->next) {
		struct box *found = cos_box_for_node(child, node);
		if (found != NULL) return found;
	}
	/* Floats and absolutely positioned boxes hang off separate lists. */
	for (struct box *f = box->float_children; f != NULL; f = f->next_float) {
		struct box *found = cos_box_for_node(f, node);
		if (found != NULL) return found;
	}
	return NULL;
}

/**
 * Fills the border-box geometry of `node` in viewport coordinates.
 *
 * box->x/y are the padding edge relative to the parent, so box_coords() is
 * used to accumulate absolute document coordinates, the border and padding
 * edges are added back to get the border box (what getBoundingClientRect
 * reports), and the window scroll offset is subtracted to make it
 * viewport-relative.
 *
 * Returns false when the node has no box at all, in which case the caller
 * should report an empty rect rather than invent numbers.
 */
bool cos_layout_border_box(void *node, int *out_x, int *out_y,
			   int *out_w, int *out_h)
{
	if (node == NULL) return false;
	struct browser_window *bw = cos_netsurf_browser_window();
	if (bw == NULL) return false;
	struct hlcache_handle *h = browser_window_get_content(bw);
	if (h == NULL) return false;
	struct box *layout = html_get_box_tree(h);
	if (layout == NULL) return false;

	struct box *box = cos_box_for_node(layout, node);
	if (box == NULL) return false;

	int x = 0, y = 0;
	box_coords(box, &x, &y);

	/* box_coords() yields the PADDING edge (box.h: "Coordinate of left
	 * padding edge relative to parent"), so stepping out to the border
	 * edge subtracts the border width only. An earlier version subtracted
	 * the padding as well, which pushed the reported origin inside-out by
	 * exactly the padding amount - a 300x100 box with 10px padding and 5px
	 * border reported left = -2 instead of 8. Sizes were unaffected (they
	 * are computed from box->width/height independently), so only position
	 * was wrong, which is why it showed up as a nested element appearing
	 * mis-inset rather than as an obviously broken rect. */
	int left   = x - box->border[LEFT].width;
	int top    = y - box->border[TOP].width;
	int width  = box->width  + box->padding[LEFT] + box->padding[RIGHT] +
		     box->border[LEFT].width + box->border[RIGHT].width;
	int height = box->height + box->padding[TOP] + box->padding[BOTTOM] +
		     box->border[TOP].width + box->border[BOTTOM].width;

	int scroll_x = 0, scroll_y = 0;
	cos_netsurf_window_get_scroll_offsets(&scroll_x, &scroll_y);

	if (out_x) *out_x = left - scroll_x;
	if (out_y) *out_y = top - scroll_y;
	if (out_w) *out_w = width;
	if (out_h) *out_h = height;
	return true;
}

/* Padding box (content + padding, excluding border): what clientWidth and
 * clientHeight report. */
bool cos_layout_padding_box(void *node, int *out_w, int *out_h)
{
	if (node == NULL) return false;
	struct browser_window *bw = cos_netsurf_browser_window();
	if (bw == NULL) return false;
	struct hlcache_handle *h = browser_window_get_content(bw);
	if (h == NULL) return false;
	struct box *layout = html_get_box_tree(h);
	if (layout == NULL) return false;
	struct box *box = cos_box_for_node(layout, node);
	if (box == NULL) return false;

	if (out_w) *out_w = box->width  + box->padding[LEFT] + box->padding[RIGHT];
	if (out_h) *out_h = box->height + box->padding[TOP]  + box->padding[BOTTOM];
	return true;
}
