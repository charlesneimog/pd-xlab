#include <string>

#include <m_pd.h>

extern "C" {
#include <g_canvas.h>
#include <s_stuff.h>
}

struct xlab {
    t_object obj;
};

static t_class *xlab_class;
static std::string xlab_lib_path;

// ─────────────────────────────────────
static void xlab_add_path(t_canvas *canvas, const std::string &path) {
    if (!canvas || path.empty())
        return;

    t_atom args[2];
    SETSYMBOL(&args[0], gensym("-path"));
    SETSYMBOL(&args[1], gensym(path.c_str()));

    // Equivalent to sending: declare -path <path>
    pd_typedmess(reinterpret_cast<t_pd *>(canvas), gensym("declare"), 2, args);
}

// ─────────────────────────────────────
static void xlab_add_paths(t_canvas *canvas) {
    xlab_add_path(canvas, xlab_lib_path);
    xlab_add_path(canvas, xlab_lib_path + "/sf");
    xlab_add_path(canvas, xlab_lib_path + "/lua/pd-upic");
    xlab_add_path(canvas, xlab_lib_path + "/lua/pd-orchidea");
}

// ─────────────────────────────────────
static void xlab_version(xlab *) {
    post("[pd-xlab] version %d.%d.%d built on %s %s", 0, 1, 0, __DATE__, __TIME__);
}

// ─────────────────────────────────────
static void *xlab_new() {
    auto *x = reinterpret_cast<xlab *>(pd_new(xlab_class));
    t_canvas *canvas = canvas_getcurrent();
    if (!canvas) {
        pd_error(x, "[xlab] could not find the current canvas");
        return x;
    }
    xlab_add_paths(canvas);
    return x;
}

// ─────────────────────────────────────
static void xlab_load_dependency(t_canvas *canvas, const char *library) {
    const std::string path = xlab_lib_path + "/" + library;
    if (!sys_load_lib(canvas, path.c_str())) {
        logpost(nullptr, 2,
                "[xlab] %s could not be loaded; some objects or "
                "abstractions will not work",
                library);
    }
}

// ─────────────────────────────────────
extern "C" void xlab_setup() {
    int major;
    int minor;
    int micro;

    sys_getversion(&major, &minor, &micro);
    const bool pd_is_too_old = major < 0 || (major == 0 && minor < 56);
    if (pd_is_too_old) {
        pd_error(nullptr,
                 "[xlab] Pd %d.%d.%d is too old; "
                 "Pd 0.56 or newer is required",
                 major, minor, micro);
        return;
    }

    xlab_class = class_new(gensym("xlab"), reinterpret_cast<t_newmethod>(xlab_new), nullptr,
                           sizeof(xlab), CLASS_DEFAULT, A_NULL);
    class_addmethod(xlab_class, reinterpret_cast<t_method>(xlab_version), gensym("version"),
                    A_NULL);

    const char *external_dir = class_gethelpdir(xlab_class);
    if (!external_dir || !external_dir[0]) {
        pd_error(nullptr, "[xlab] could not determine its installation directory");
        return;
    }

    xlab_lib_path = external_dir;
    t_canvas *canvas = canvas_getcurrent();
    xlab_load_dependency(canvas, "lua");
    xlab_load_dependency(canvas, "py4pd");
    post("[pd-xlab] version %d.%d.%d built on %s %s", 0, 1, 0, __DATE__, __TIME__);
}
