#include <m_pd.h>

#include <g_canvas.h>

#include <new>

static t_class *xdarray_class;

struct t_xdarray_buffer {
    // Internal structure, similar to [array define]
    t_canvas *buffer;
    t_glist *graph;
    t_garray *array;

    // Array names
    t_symbol *name;
    t_symbol *realname;
    t_xdarray_buffer *next;
};

typedef struct _xdarray {
    t_object x_obj;

    // Patch containing [x.darray~]
    t_canvas *canvas;
    t_xdarray_buffer *buffers;
    t_xdarray_buffer *current;
} t_xdarray;

// ─────────────────────────────────────
static t_xdarray_buffer *xdarray_find(t_xdarray *x, t_symbol *realname) {
    for (t_xdarray_buffer *buffer = x->buffers; buffer; buffer = buffer->next) {
        if (buffer->realname == realname)
            return buffer;
    }
    return NULL;
}

// ─────────────────────────────────────
static void xdarray_create(t_xdarray *x, t_symbol *name, t_floatarg fsize) {
    int size = (int)fsize;

    if (size < 1)
        size = 1;

    t_symbol *realname = canvas_realizedollar(x->canvas, name);
    if (xdarray_find(x, realname) || pd_findbyclass(realname, garray_class)) {
        pd_error(x, "[x.darray~] Array '%s' already exists", realname->s_name);
        return;
    }

    t_xdarray_buffer *entry = new (std::nothrow) t_xdarray_buffer;
    if (!entry) {
        pd_error(x, "[x.darray~] Could not allocate array entry");
        return;
    }

    canvas_setcurrent(x->canvas);

    // ─────────────────────────────────
    t_atom args[6];
    SETFLOAT(args + 0, GLIST_DEFCANVASXLOC);
    SETFLOAT(args + 1, GLIST_DEFCANVASYLOC);
    SETFLOAT(args + 2, 600);
    SETFLOAT(args + 3, 400);
    SETSYMBOL(args + 4, name);
    SETFLOAT(args + 5, 0);
    t_canvas *buffer = canvas_new(NULL, NULL, 6, args);

    if (!buffer) {
        canvas_unsetcurrent(x->canvas);
        delete entry;
        pd_error(x, "[x.darray~] Could not create internal canvas");
        return;
    }

    // ─────────────────────────────────
    t_glist *graph =
        glist_addglist((t_glist *)buffer, &s_, 0, -1, size > 1 ? size - 1 : 1, 1, 50, 350, 550, 50);

    // ─────────────────────────────────
    t_garray *array = graph ? graph_array(graph, name, &s_float, size, 0) : NULL;

    canvas_unsetcurrent(buffer);
    canvas_unsetcurrent(x->canvas);

    if (!array) {
        canvas_free(buffer);
        delete entry;

        pd_error(x, "[x.darray~] Could not create array '%s'", realname->s_name);

        return;
    }

    buffer->gl_loading = 0;
    buffer->gl_edit = 0;

    entry->buffer = buffer;
    entry->graph = graph;
    entry->array = array;
    entry->name = name;
    entry->realname = realname;
    entry->next = x->buffers;
    x->buffers = entry;
    x->current = entry;
}

// ─────────────────────────────────────
static void xdarray_delete(t_xdarray *x, t_symbol *name) {
    t_symbol *realname = canvas_realizedollar(x->canvas, name);
    t_xdarray_buffer **link = &x->buffers;
    while (*link && (*link)->realname != realname)
        link = &(*link)->next;

    if (!*link) {
        pd_error(x, "[x.darray~] '%s' is not owned by this object", realname->s_name);
        return;
    }

    t_xdarray_buffer *entry = *link;
    *link = entry->next;
    if (x->current == entry)
        x->current = x->buffers;
    canvas_free(entry->buffer);
    delete entry;
}

// ─────────────────────────────────────
static void xdarray_set(t_xdarray *x, t_symbol *name) {
    t_symbol *realname = canvas_realizedollar(x->canvas, name);
    t_xdarray_buffer *entry = xdarray_find(x, realname);
    if (!entry) {
        pd_error(x, "[x.darray~] '%s' is not owned by this object", realname->s_name);
        return;
    }
    x->current = entry;
}

// ─────────────────────────────────────
static void xdarray_open(t_xdarray *x) {
    if (!x->current) {
        pd_error(x, "[x.darray~] No array defined");
        return;
    }
    canvas_vis(x->current->buffer, 1);
}

// ─────────────────────────────────────
static void xdarray_click(t_xdarray *x, t_floatarg xpos, t_floatarg ypos, t_floatarg shift,
                          t_floatarg ctrl, t_floatarg alt) {
    xdarray_open(x);
}

// ─────────────────────────────────────
static void *xdarray_new(void) {
    t_xdarray *x = (t_xdarray *)pd_new(xdarray_class);
    x->canvas = canvas_getcurrent();
    x->buffers = NULL;
    x->current = NULL;
    return x;
}

// ─────────────────────────────────────
static void xdarray_free(t_xdarray *x) {
    while (x->buffers) {
        t_xdarray_buffer *entry = x->buffers;
        x->buffers = entry->next;
        canvas_free(entry->buffer);
        delete entry;
    }
    x->current = NULL;
}

// ─────────────────────────────────────
extern "C" void setup_x0x2edarray_tilde(void) {
    xdarray_class = class_new(gensym("x.darray~"), (t_newmethod)xdarray_new, (t_method)xdarray_free,
                              sizeof(t_xdarray), CLASS_DEFAULT, A_NULL);

    class_addmethod(xdarray_class, (t_method)xdarray_create, gensym("create"), A_SYMBOL, A_DEFFLOAT,
                    0);
    class_addmethod(xdarray_class, (t_method)xdarray_delete, gensym("delete"), A_SYMBOL, 0);
    class_addmethod(xdarray_class, (t_method)xdarray_set, gensym("set"), A_SYMBOL, 0);
    class_addmethod(xdarray_class, (t_method)xdarray_open, gensym("open"), A_NULL, 0);
    class_addmethod(xdarray_class, (t_method)xdarray_click, gensym("click"), A_FLOAT, A_FLOAT,
                    A_FLOAT, A_FLOAT, A_FLOAT, 0);
}
