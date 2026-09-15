/* -------------------------- x.cputime ------------------------------ */

#include <m_pd.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static t_class *xcputime_class;

typedef struct _xcputime {
    t_object x_obj;

#ifdef _WIN32
    LARGE_INTEGER x_freq;
    LARGE_INTEGER x_start;
#else
    struct timespec x_start;
#endif

} t_xcputime;

/* ====================================================== */
/* high-resolution timestamp                              */
/* ====================================================== */

#ifdef _WIN32

static double xcputime_getms(LARGE_INTEGER *start, LARGE_INTEGER *end, LARGE_INTEGER *freq) {
    return 1000.0 * ((double)(end->QuadPart - start->QuadPart) / (double)freq->QuadPart);
}

#else

static double xcputime_getms(struct timespec *start, struct timespec *end) {
    double sec = (double)(end->tv_sec - start->tv_sec);

    double nsec = (double)(end->tv_nsec - start->tv_nsec);

    return (sec * 1000.0) + (nsec * 1e-6);
}

#endif

/* ====================================================== */
/* start measurement                                      */
/* ====================================================== */

static void xcputime_bang(t_xcputime *x) {
#ifdef _WIN32

    QueryPerformanceCounter(&x->x_start);

#else

    clock_gettime(CLOCK_MONOTONIC, &x->x_start);

#endif
}

/* ====================================================== */
/* stop measurement                                       */
/* ====================================================== */

static void xcputime_bang2(t_xcputime *x) {
    t_float elapsed;

#ifdef _WIN32

    LARGE_INTEGER now;

    QueryPerformanceCounter(&now);

    elapsed = (t_float)xcputime_getms(&x->x_start, &now, &x->x_freq);

#else

    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    elapsed = (t_float)xcputime_getms(&x->x_start, &now);

#endif

    outlet_float(x->x_obj.ob_outlet, elapsed);
}

/* ====================================================== */
/* constructor                                            */
/* ====================================================== */

static void *xcputime_new(void) {
    t_xcputime *x = (t_xcputime *)pd_new(xcputime_class);
    outlet_new(&x->x_obj, gensym("float"));
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("bang"), gensym("bang2"));
#ifdef _WIN32
    QueryPerformanceFrequency(&x->x_freq);
#endif
    xcputime_bang(x);
    return x;
}

/* ====================================================== */
/* setup                                                  */
/* ====================================================== */
extern "C" void setup_x0x2ecputime(void) {
    xcputime_class = class_new(gensym("x.cputime"), (t_newmethod)xcputime_new, 0, sizeof(t_xcputime),
                               CLASS_DEFAULT, A_NULL);
    class_addbang(xcputime_class, xcputime_bang);
    class_addmethod(xcputime_class, (t_method)xcputime_bang2, gensym("bang2"), A_NULL);
}
