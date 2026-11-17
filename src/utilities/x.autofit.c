#include <m_pd.h>

#include <g_canvas.h>

#define PADDING 30
#define MIN_WIDTH 120
#define MIN_HEIGHT 100

typedef struct _autofit_geometry {
    t_gpointer pointer;

    int old_x2;
    int old_y2;

    int new_x2;
    int new_y2;

    struct _autofit_geometry *next;
} t_autofit_geometry;

// ─────────────────────────────────────
static int max(int a, int b) { return a > b ? a : b; }

// ─────────────────────────────────────
static void autofit_collect(t_canvas *canvas, t_autofit_geometry **list) {
    int right = 0;
    int bottom = 0;
    int zoom = max(1, glist_getzoom(canvas));

    int embedded = canvas->gl_isgraph && !canvas->gl_havewindow;

    // Collect nested subpatches.
    for (t_gobj *g = canvas->gl_list; g; g = g->g_next) {
        if (pd_class(&g->g_pd) == canvas_class) {
            t_canvas *child = (t_canvas *)g;

            if (!canvas_isabstraction(child)) {
                autofit_collect(child, list);
            }
        }
    }

    // Preserve legacy graphs.
    if (canvas->gl_isgraph && !canvas->gl_goprect) {
        return;
    }

    // Preserve inverted coordinate systems.
    if (!canvas->gl_isgraph && canvas->gl_y2 < canvas->gl_y1) {
        return;
    }

    // Calculate bounding box without modifying the canvas.
    for (t_gobj *g = canvas->gl_list; g; g = g->g_next) {
        if (pd_class(&g->g_pd) == garray_class) {
            continue;
        }

        int x1 = 0, y1 = 0;
        int x2 = 0, y2 = 0;

        gobj_getrect(g, canvas, &x1, &y1, &x2, &y2);

        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        if (embedded) {
            t_object *obj = pd_checkobject(&g->g_pd);

            if (!obj) {
                continue;
            }

            x2 = obj->te_xpix * zoom + (x2 - x1);
            y2 = obj->te_ypix * zoom + (y2 - y1);
        }

        right = max(right, (x2 + zoom - 1) / zoom);
        bottom = max(bottom, (y2 + zoom - 1) / zoom);
    }

    if (canvas->gl_goprect) {
        right = max(right, canvas->gl_xmargin + canvas->gl_pixwidth);

        bottom = max(bottom, canvas->gl_ymargin + canvas->gl_pixheight);
    }

    int width = max(MIN_WIDTH, right + PADDING);
    int height = max(MIN_HEIGHT, bottom + PADDING);

    // Store original and calculated geometry.
    t_autofit_geometry *geo = getbytes(sizeof(*geo));

    if (!geo) {
        return;
    }

    gpointer_init(&geo->pointer);
    gpointer_setglist(&geo->pointer, canvas, NULL);

    geo->old_x2 = canvas->gl_screenx2;
    geo->old_y2 = canvas->gl_screeny2;

    geo->new_x2 = canvas->gl_screenx1 + width;
    geo->new_y2 = canvas->gl_screeny1 + height;

    geo->next = *list;
    *list = geo;
}

// ─────────────────────────────────────
static void autofit_apply(t_autofit_geometry *list) {
    for (t_autofit_geometry *geo = list; geo; geo = geo->next) {
        t_gstub *stub = geo->pointer.gp_stub;

        if (stub && stub->gs_which == GP_GLIST) {
            t_canvas *canvas = stub->gs_un.gs_glist;

            canvas->gl_screenx2 = geo->new_x2;
            canvas->gl_screeny2 = geo->new_y2;
        }
    }
}

// ─────────────────────────────────────
static void autofit_restore(t_autofit_geometry *list) {
    while (list) {
        t_autofit_geometry *geo = list;
        list = list->next;

        t_gstub *stub = geo->pointer.gp_stub;

        // The canvas may have been destroyed during saving.
        if (stub && stub->gs_which == GP_GLIST) {
            t_canvas *canvas = stub->gs_un.gs_glist;

            if (canvas->gl_screenx2 == geo->new_x2 && canvas->gl_screeny2 == geo->new_y2) {

                canvas->gl_screenx2 = geo->old_x2;
                canvas->gl_screeny2 = geo->old_y2;
            }
        }

        gpointer_unset(&geo->pointer);
        freebytes(geo, sizeof(*geo));
    }
}

// ─────────────────────────────────────
static void autofit_menusave(t_canvas *canvas, t_floatarg destroy) {
    t_autofit_geometry *list = NULL;

    autofit_collect(canvas_getrootfor(canvas), &list);
    autofit_apply(list);

    pd_vmess(&canvas->gl_pd, gensym("menusave_aliased"), "f", (double)destroy);

    autofit_restore(list);
}

// ─────────────────────────────────────
static void autofit_savetofile(t_canvas *canvas, t_symbol *filename, t_symbol *directory,
                               t_floatarg destroy) {
    t_autofit_geometry *list = NULL;

    autofit_collect(canvas_getrootfor(canvas), &list);
    autofit_apply(list);

    pd_vmess(&canvas->gl_pd, gensym("savetofile_aliased"), "ssf", filename, directory,
             (double)destroy);

    autofit_restore(list);
}

// ─────────────────────────────────────
void setup_x0x2eautofit(void) {
    static int initialized = 0;

    if (initialized) {
        return;
    }

    if (zgetfn(&canvas_class, gensym("menusave_aliased")) ||
        zgetfn(&canvas_class, gensym("savetofile_aliased"))) {
        pd_error(NULL, "autofit: save methods already overridden");
        return;
    }

    initialized = 1;

    class_addmethod(canvas_class, (t_method)autofit_menusave, gensym("menusave"), A_DEFFLOAT, 0);

    class_addmethod(canvas_class, (t_method)autofit_savetofile, gensym("savetofile"), A_SYMBOL,
                    A_SYMBOL, A_DEFFLOAT, 0);
}
