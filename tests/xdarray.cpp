/* Build:
   c++ -std=c++11 -shared -fPIC -I/usr/include/pd tests/xdarray.cpp \
       -o /tmp/xdarray_test.pd_linux
   Run:
   pd -nogui -noaudio -stderr -path /tmp tests/xdarray.pd
*/
#include "../src/utilities/x.darray~.cpp"
#include <cassert>
#include <initializer_list>

static t_class *xdarray_test_class;

static void send_name(t_xdarray *x, const char *method, t_symbol *name) {
    t_atom arg;
    SETSYMBOL(&arg, name);
    pd_typedmess(&x->x_obj.ob_pd, gensym(method), 1, &arg);
}

static t_garray *find_array(t_symbol *name) {
    return (t_garray *)pd_findbyclass(name, garray_class);
}

static void check_size(t_symbol *name, int expected) {
    t_garray *array = find_array(name);
    assert(array);
    int size = 0;
    t_word *words = NULL;
    assert(garray_getfloatwords(array, &size, &words));
    assert(size == expected);
}

static void *xdarray_test_new(void) {
    t_object *test = (t_object *)pd_new(xdarray_test_class);
    t_canvas *owner = canvas_getcurrent();
    t_xdarray *x = (t_xdarray *)xdarray_new();
    t_xdarray *other = (t_xdarray *)xdarray_new();
    t_symbol *a = gensym("xdarray-test-a");
    t_symbol *b = gensym("xdarray-test-b");
    t_symbol *c = gensym("xdarray-test-c");
    t_symbol *foreign = gensym("xdarray-test-foreign");

    // Exercise Pd's method dispatch, including an omitted size.
    send_name(x, "create", a);
    check_size(a, 1);
    xdarray_create(x, b, 512);
    xdarray_create(x, c, 1024);
    check_size(b, 512);
    check_size(c, 1024);
    assert(canvas_getcurrent() == owner);
    assert(x->current->realname == c);
    for (t_symbol *name : {a, b, c}) {
        send_name(x, "set", name);
        assert(x->current->realname == name);
    }

    // Selection and failed operations must preserve existing samples.
    int size;
    t_word *words;
    assert(garray_getfloatwords(find_array(a), &size, &words));
    words[0].w_float = 42;
    xdarray_create(other, foreign, 16);
    send_name(x, "set", a);
    xdarray_create(x, a, 99);        // Expected duplicate error.
    xdarray_create(x, foreign, 99);  // Expected duplicate error.
    send_name(x, "set", foreign);    // Expected ownership error.
    send_name(x, "delete", foreign); // Expected ownership error.
    assert(x->current->realname == a);
    check_size(a, 1);
    check_size(foreign, 16);
    assert(words[0].w_float == 42);

    // Remove an unselected middle entry, then the selected tail and last entry.
    send_name(x, "delete", b);
    assert(!find_array(b));
    assert(x->current->realname == a);
    send_name(x, "delete", a);
    assert(!find_array(a));
    assert(x->current->realname == c);
    send_name(x, "delete", c);
    assert(!find_array(c));
    assert(!x->current && !x->buffers);
    send_name(x, "set", a);    // Expected ownership error after deletion.
    send_name(x, "delete", a); // Expected ownership error after deletion.
    assert(!x->current && !x->buffers);

    // Reuse deleted names and resolve dollar names in the owning patch.
    send_name(x, "create", a);
    send_name(x, "create", b);
    t_symbol *local = gensym("$0-xdarray-test");
    t_symbol *resolved = canvas_realizedollar(owner, local);
    send_name(x, "create", local);
    check_size(resolved, 1);
    send_name(x, "set", resolved);
    assert(x->current->realname == resolved);
    send_name(x, "delete", local);
    assert(!find_array(resolved));
    assert(x->current->realname == b);
    send_name(x, "create", local);

    // Destruction releases every owned array without touching other instances.
    pd_free(&x->x_obj.ob_pd);
    assert(!find_array(a) && !find_array(b) && !find_array(resolved));
    check_size(foreign, 16);
    pd_free(&other->x_obj.ob_pd);
    assert(!find_array(foreign));
    assert(canvas_getcurrent() == owner);
    post("xdarray tests passed");
    return test;
}

extern "C" void xdarray_test_setup(void) {
    setup_x0x2edarray_tilde();
    xdarray_test_class = class_new(gensym("xdarray_test"), (t_newmethod)xdarray_test_new, NULL,
                                   sizeof(t_object), CLASS_DEFAULT, A_NULL);
}
