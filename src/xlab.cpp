#include "xlab.hpp"
#include <string>

extern "C" {
#include <s_stuff.h>
}

static t_class *xlabLib;

// ==============================================
static void *xlab_new(void) {
    xlab *x = (xlab *)pd_new(xlabLib);
    return (x);
}

// ==============================================
extern "C" void xlab_setup(void) {
    int major, minor, micro;
    sys_getversion(&major, &minor, &micro);
    if (major < 0 && minor < 56) {
        pd_error(nullptr, "[xlab] xlab is not supported because or Pd is too old");
        return;
    }

    xlabLib =
        class_new(gensym("xlab"), (t_newmethod)xlab_new, 0, sizeof(xlab), CLASS_NOINLET, A_NULL, 0);

    // add to the search path
    std::string lib_path = xlabLib->c_externdir->s_name;
    std::string abs_path = lib_path + "/Abstractions/";
    std::string audios_path = lib_path + "/Resources/Audios/";
    std::string sf = lib_path + "/sf/";
    std::string vst = lib_path + "/vst/";
    std::string vst3 = lib_path + "/vst3/";
    std::string lua = lib_path + "/lua/";
    std::string py = lib_path + "/python/";

    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, lib_path.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, sf.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, vst.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, vst3.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, abs_path.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, audios_path.c_str(), 0);

    const char *lualibs[] = {"pd-upic", "pd-orchidea"};
    for (auto &lib : lualibs) {
        std::string lualib = lib_path + "/lua/" + lib;
        STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, lualib.c_str(), 0);
    }

    std::string ext_path = xlabLib->c_externdir->s_name;
    ext_path += "/Help-Patches/";

    // Lua Library
    t_canvas *cnv = canvas_getcurrent();
    int result = sys_load_lib(cnv, "pdlua");
    if (!result) {
        pd_error(nullptr, "[pd-xlab] pdlua was not load, some objects will not work");
        return;
    };

    result = sys_load_lib(cnv, "py4pd");
    if (!result) {
        pd_error(nullptr, "[pd-xlab] py4pd was not load, some objects will not work");
    }

    // Load Externals
    class_set_extern_dir(gensym(ext_path.c_str()));

    // arrays
    arrayrotate_setup();
    arraysum_setup();
    arrayappend_setup();

    // statistics
    kldivergence_setup();
    renyi_setup();
    euclidean_setup();
    entropy_setup();
    kalman_setup();

    // utils
    infinite0x2erecord_tilde_setup();

    post("[pd-xlab] version %d.%d.%d", 0, 1, 0);
}
