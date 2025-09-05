#include "neimog.hpp"
#include <string>

extern "C" {
#include <s_stuff.h>
}

static t_class *neimogLib;

// ==============================================
static void *neimog_new(void) {
    neimog *x = (neimog *)pd_new(neimogLib);
    return (x);
}

// ==============================================
extern "C" void neimog_setup(void) {
    int major, minor, micro;
    sys_getversion(&major, &minor, &micro);
    if (major < 0 && minor < 54) {
        return;
    }

    neimogLib = class_new(gensym("neimog"), (t_newmethod)neimog_new, 0, sizeof(neimog),
                          CLASS_NOINLET, A_NULL, 0);

    // add to the search path
    std::string libPath = neimogLib->c_externdir->s_name;
    std::string AbsPath = libPath + "/Abstractions/";
    std::string AudiosPath = libPath + "/Resources/Audios/";
    std::string sf = libPath + "/sf/";
    std::string vst = libPath + "/vst/";
    std::string vst3 = libPath + "/vst3/";
    std::string lua = libPath + "/lua/";
    std::string py = libPath + "/python/";

    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, sf.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, vst.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, vst3.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, libPath.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, AbsPath.c_str(), 0);
    STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, AudiosPath.c_str(), 0);

    const char *lualibs[] = {"pd-upic", "pd-orchidea"};
    for (auto &lib : lualibs) {
        std::string lualib = libPath + "/lua/" + lib;
        STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, lualib.c_str(), 0);
    }

    std::string ExtPath = neimogLib->c_externdir->s_name;
    ExtPath += "/Help-Patches/";

    // Lua Library
    t_canvas *cnv = canvas_getcurrent();
    int result = sys_load_lib(cnv, "pdlua");
    if (!result) {
        pd_error(nullptr, "[pd-neimog] pdlua not installed, please install it going to Help->Find "
                          "Externals. Search for pdlua, click on install. Wait, and restart "
                          "PureData. pd-neimog requires pdlua");
    };

    result = sys_load_lib(cnv, "py4pd");
    if (!result) {
        pd_error(nullptr, "[pd-neimog] py4pd was not load, some objects will not work");
    }

    result = sys_load_lib(cnv, "saf");
    if (!result) {
        pd_error(nullptr, "[pd-neimog] saf was not load, some objects will not work");
    }

    // Load Externals
    class_set_extern_dir(gensym(ExtPath.c_str()));

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

    // manipulations
    transposer_tilde_setup();

    // mir
    bock_tilde_setup();
    nonset_tilde_setup();
    onsetsds_tilde_setup();

    // plugins
    patcherize_setup();

    // utils
    infinite0x2erecord_tilde_setup();

    post("[pd-neimog] version %d.%d.%d", 0, 1, 0);
}
