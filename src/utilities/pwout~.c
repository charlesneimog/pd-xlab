/* pw_out~: PipeWire output external with variable channel count
 *
 * Build (Linux):
 *   cc -O2 -std=c11 -fPIC -shared -o pwout~.pd_linux Sources/utilities/pwout~.c \
 *      $(pkg-config --cflags --libs libpipewire-0.3) -I"/usr/include/pd"
 *
 * Notes:
 * - Float32 interleaved to PipeWire; Pd uses deinterleaved per-channel blocks
 * - Uses pw_thread_loop + pw_stream_new_simple (no pw_context/pw_core needed)
 * - Simple SPSC ringbuffer between Pd perform and PipeWire RT thread
 * - Channel count is configurable via creation arg [pwout~ <channels>] (default 2)
 */

#include <m_pd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUFFERS 4
#define MAX_CHANNELS 64

typedef struct {
    float *buf;
    size_t cap;       /* total samples (frames*channels) */
    _Atomic size_t r; /* read pos in frames */
    _Atomic size_t w; /* write pos in frames */
} rb_t;

static t_class *pw_out_class;

typedef struct _pw_out_t {
    t_object x_obj;
    t_sample t_x; /* required by CLASS_MAINSIGNALIN */

    struct pw_thread_loop *tloop;
    struct pw_stream *stream;
    struct spa_hook stream_listener;

    rb_t ring;
    float *tmp_iobuf; /* interleaved temp buffer (nframes * ichcount) */
    size_t tmp_cap;   /* frames capacity for tmp_iobuf */

    unsigned ichcount; /* number of input channels (PipeWire channels) */
    int sr;            /* requested sample rate */
} t_pw_out_t;

/* ------------- Ringbuffer ------------- */
static void rb_init(rb_t *rb, size_t frames_cap, int channels) {
    rb->cap = frames_cap * (size_t)channels;
    rb->buf = (float *)calloc(rb->cap, sizeof(float));
    atomic_store(&rb->r, 0);
    atomic_store(&rb->w, 0);
}

static void rb_free(rb_t *rb) {
    free(rb->buf);
    rb->buf = NULL;
    rb->cap = 0;
}

static size_t rb_read(rb_t *rb, float *dst, size_t frames, int channels) {
    size_t cap_frames = rb->cap / (size_t)channels;
    size_t r = atomic_load_explicit(&rb->r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&rb->w, memory_order_acquire);
    size_t avail = (w + cap_frames - r) % cap_frames;
    size_t todo = frames < avail ? frames : avail;

    size_t idx_frames = r % cap_frames;
    size_t end_frames = cap_frames - idx_frames;

    size_t c1 = todo < end_frames ? todo : end_frames;
    memcpy(dst, rb->buf + (idx_frames * channels), c1 * (size_t)channels * sizeof(float));
    if (todo > c1) {
        memcpy(dst + c1 * channels, rb->buf, (todo - c1) * (size_t)channels * sizeof(float));
    }
    atomic_store_explicit(&rb->r, (r + todo) % cap_frames, memory_order_release);
    return todo;
}

static size_t rb_write(rb_t *rb, const float *src, size_t frames, int channels) {
    size_t cap_frames = rb->cap / (size_t)channels;
    size_t r = atomic_load_explicit(&rb->r, memory_order_acquire);
    size_t w = atomic_load_explicit(&rb->w, memory_order_relaxed);
    size_t freef = (r + cap_frames - w) % cap_frames;
    if (freef == 0)
        freef = cap_frames;
    if (freef == cap_frames)
        freef--; /* keep one slot free */
    size_t todo = frames < freef ? frames : freef;

    size_t idx_frames = w % cap_frames;
    size_t end_frames = cap_frames - idx_frames;

    size_t c1 = todo < end_frames ? todo : end_frames;
    memcpy(rb->buf + (idx_frames * channels), src, c1 * (size_t)channels * sizeof(float));
    if (todo > c1) {
        memcpy(rb->buf, src + c1 * channels, (todo - c1) * (size_t)channels * sizeof(float));
    }
    atomic_store_explicit(&rb->w, (w + todo) % cap_frames, memory_order_release);
    return todo;
}

/* ------------- PipeWire callbacks ------------- */
static void pw_on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    (void)data;
    (void)id;
    (void)param;
}

static void pw_on_process(void *data) {
    t_pw_out_t *x = (t_pw_out_t *)data;
    if (!x || !x->stream)
        return;

    struct pw_buffer *b = pw_stream_dequeue_buffer(x->stream);
    if (!b)
        return;

    struct spa_buffer *buf = b->buffer;
    if (!buf || buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(x->stream, b);
        return;
    }

    float *dst = (float *)buf->datas[0].data;
    uint32_t max_bytes = buf->datas[0].maxsize;
    uint32_t stride = x->ichcount * sizeof(float);
    uint32_t nframes = (stride > 0) ? (max_bytes / stride) : 0;

    if (nframes == 0) {
        pw_stream_queue_buffer(x->stream, b);
        return;
    }

    size_t got = rb_read(&x->ring, dst, nframes, (int)x->ichcount);
    if (got < nframes) {
        memset(dst + got * x->ichcount, 0, (nframes - got) * stride);
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = nframes * stride;

    pw_stream_queue_buffer(x->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = pw_on_param_changed,
    .process = pw_on_process,
};

/* ------------- PipeWire setup/teardown ------------- */
static int pw_setup_stream(t_pw_out_t *x) {
    struct pw_properties *props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Production", NULL);

    x->stream = pw_stream_new_simple(pw_thread_loop_get_loop(x->tloop), "pwout~",
                                     props, /* owned by stream */
                                     &stream_events, x);
    if (!x->stream)
        return -1;

    struct spa_audio_info_raw info;
    spa_zero(info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = (uint32_t)x->ichcount;
    info.rate = (uint32_t)(x->sr > 0 ? x->sr : 48000);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[2];
    uint32_t n_params = 0;

    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_format, SPA_POD_Id(info.format),
        SPA_FORMAT_AUDIO_channels, SPA_POD_Int(info.channels), SPA_FORMAT_AUDIO_rate,
        SPA_POD_Int(info.rate));

    /* Suggest buffer size ~ 1 Pd block of 64 frames (PipeWire may renegotiate) */
    int frames = 64;
    int stride = (int)x->ichcount * (int)sizeof(float);
    int size_bytes = frames * stride;

    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_Int(MAX_BUFFERS), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
        SPA_POD_Int(size_bytes), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride));

    uint32_t flags =
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;

    if (pw_stream_connect(x->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, n_params) < 0) {
        return -1;
    }
    return 0;
}

static int pw_start(t_pw_out_t *x) {
    pw_init(NULL, NULL);

    x->tloop = pw_thread_loop_new("pwout~", NULL);
    if (!x->tloop)
        return -1;

    if (pw_thread_loop_start(x->tloop) != 0)
        return -1;

    pw_thread_loop_lock(x->tloop);
    int ok = pw_setup_stream(x);
    pw_thread_loop_unlock(x->tloop);
    if (ok < 0)
        return -1;

    return 0;
}

static void pw_stop(t_pw_out_t *x) {
    if (x->tloop) {
        pw_thread_loop_lock(x->tloop);
        if (x->stream) {
            pw_stream_disconnect(x->stream);
            pw_stream_destroy(x->stream);
            x->stream = NULL;
        }
        pw_thread_loop_unlock(x->tloop);

        pw_thread_loop_stop(x->tloop);
        pw_thread_loop_destroy(x->tloop);
        x->tloop = NULL;
    }
}

/* ------------- Pd DSP (variable channel count) ------------- */
static t_int *pw_out_perform(t_int *w) {
    t_pw_out_t *x = (t_pw_out_t *)(w[1]);
    int n = (int)(w[2]);

    if ((size_t)n > x->tmp_cap) {
        x->tmp_cap = (size_t)n;
        x->tmp_iobuf = (float *)realloc(x->tmp_iobuf, x->tmp_cap * x->ichcount * sizeof(float));
    }

    /* Interleave all input channels into tmp_iobuf */
    for (int i = 0; i < n; i++) {
        for (unsigned j = 0; j < x->ichcount; j++) {
            t_sample *inX = (t_sample *)(w[3 + j]);
            x->tmp_iobuf[i * x->ichcount + j] = (float)inX[i];
        }
    }

    rb_write(&x->ring, x->tmp_iobuf, (size_t)n, (int)x->ichcount);

    /* nargs = 2 + ichcount, so return w + (nargs + 1) = w + (3 + ichcount) */
    return (w + (3 + x->ichcount));
}

static void pw_out_dsp(t_pw_out_t *x, t_signal **sp) {
    x->sr = (int)sp[0]->s_sr;
    const int nargs = 2 + (int)x->ichcount;
    t_int *sigvec = (t_int *)getbytes((size_t)nargs * sizeof(t_int));
    if (!sigvec)
        return;

    sigvec[0] = (t_int)x;
    sigvec[1] = (t_int)sp[0]->s_n;
    logpost(x, 3, "Vector size %d", sp[0]->s_n);
    for (unsigned j = 0; j < x->ichcount; j++) {
        sigvec[2 + j] = (t_int)sp[j]->s_vec;
    }

    dsp_addv(pw_out_perform, nargs, sigvec);
    freebytes(sigvec, (size_t)nargs * sizeof(t_int));
}

/* ------------- Pd class ------------- */
static void *pw_out_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    t_pw_out_t *x = (t_pw_out_t *)pd_new(pw_out_class);

    unsigned ichcount = 2;
    if (argc > 0 && argv && atom_getfloat(argv) > 0) {
        int requested = (int)atom_getfloat(argv);
        if (requested < 1)
            requested = 1;
        if (requested > MAX_CHANNELS)
            requested = MAX_CHANNELS;
        ichcount = (unsigned)requested;
    }

    /* First inlet is a signal inlet via CLASS_MAINSIGNALIN */
    for (unsigned i = 1; i < ichcount; i++) {
        signalinlet_new(&x->x_obj, 0);
    }

    x->tloop = NULL;
    x->stream = NULL;
    x->tmp_iobuf = NULL;
    x->tmp_cap = 0;
    x->sr = sys_getsr();
    x->ichcount = ichcount;

    /* Ringbuffer: ~100 Pd blocks (64 frames) */
    rb_init(&x->ring, 64 * 100, (int)x->ichcount);

    if (pw_start(x) != 0) {
        logpost(x, 1, "pwout~: failed to start PipeWire; object will output silence");
        return NULL;
    }
    return x;
}

static void pw_out_free(t_pw_out_t *x) {
    pw_stop(x);
    rb_free(&x->ring);
    free(x->tmp_iobuf);
}

void pwout_tilde_setup(void) {
    pw_out_class = class_new(gensym("pwout~"), (t_newmethod)pw_out_new, (t_method)pw_out_free,
                             sizeof(t_pw_out_t), CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(pw_out_class, t_pw_out_t, t_x);
    class_addmethod(pw_out_class, (t_method)pw_out_dsp, gensym("dsp"), A_CANT, 0);
}
