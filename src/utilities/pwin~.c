/* pwin~: PipeWire input external with variable channel count (requests 64-frame blocks)
 *
 * Build (Linux):
 *   cc -O2 -std=c11 -fPIC -shared -o pwin~.pd_linux Sources/utilities/pwin~.c \
 *      $(pkg-config --cflags --libs libpipewire-0.3) -I"/usr/include/pd"
 *
 * Notes:
 * - Requests a 64-frame quantum (block size) from PipeWire using:
 *     - node properties (node.latency, node.rate)
 *     - ParamLatency (min/max quantum = 64)
 *     - small ParamBuffers sized for 64 frames
 *   The session manager/graph may still choose a different quantum. On first
 *   process callback, we print a one-time notice if the server did not grant 64.
 * - Float32 interleaved from PipeWire; Pd expects deinterleaved per-channel blocks
 * - Uses pw_thread_loop + pw_stream_new_simple (no pw_context/pw_core needed)
 * - SPSC ringbuffer between PipeWire RT thread (producer) and Pd perform (consumer)
 * - Channel count is configurable via creation arg: [pwin~ <channels>] (default 2)
 */

#include <m_pd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/param.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUFFERS 4
#define MAX_CHANNELS 64
#define DESIRED_BLOCK 64 /* we request 64-sample quantum */

/* -------- Simple SPSC ringbuffer for interleaved float audio (frames) -------- */
typedef struct {
    float *buf;
    size_t cap;       /* samples capacity (interleaved: frames * channels) */
    _Atomic size_t r; /* read pos in frames */
    _Atomic size_t w; /* write pos in frames */
} rb_t;

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

/* Read up to 'frames' frames (interleaved) from ring into dst.
 * Returns number of frames actually read.
 */
static size_t rb_read(rb_t *rb, float *dst, size_t frames, int channels) {
    if (!rb->buf || rb->cap == 0)
        return 0;
    size_t cap_frames = rb->cap / (size_t)channels;
    size_t r = atomic_load_explicit(&rb->r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&rb->w, memory_order_acquire);
    size_t avail = (w + cap_frames - r) % cap_frames;
    size_t todo = frames < avail ? frames : avail;

    if (todo == 0)
        return 0;

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

/* Write up to 'frames' frames (interleaved) from src into ring.
 * Returns number of frames actually written.
 */
static size_t rb_write(rb_t *rb, const float *src, size_t frames, int channels) {
    if (!rb->buf || rb->cap == 0)
        return 0;
    size_t cap_frames = rb->cap / (size_t)channels;
    size_t r = atomic_load_explicit(&rb->r, memory_order_acquire);
    size_t w = atomic_load_explicit(&rb->w, memory_order_relaxed);
    size_t freef = (r + cap_frames - w) % cap_frames;
    if (freef == 0)
        freef = cap_frames;
    if (freef == cap_frames)
        freef--; /* keep one slot free to distinguish full/empty */
    size_t todo = frames < freef ? frames : freef;

    if (todo == 0)
        return 0;

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

/* ------------------------------- Object ------------------------------- */
typedef struct _pw_in_t {
    t_object x_obj;
    t_sample t_x; /* not used (no signal inlets), but required if CLASS_MAINSIGNALIN is used */

    /* PipeWire */
    struct pw_thread_loop *tloop;
    struct pw_stream *stream;
    struct spa_hook stream_listener;

    /* Buffering */
    rb_t ring;        /* interleaved FIFO from PW->Pd */
    float *tmp_iobuf; /* temp interleaved block for deinterleave into Pd */
    size_t tmp_cap;   /* frames capacity for tmp_iobuf */

    unsigned ochcount; /* number of output channels (Pd outlets / PipeWire channels) */
    int sr;            /* requested sample rate */

    /* Blocksize negotiation info */
    unsigned desired_block;    /* what we request (64) */
    int printed_mismatch_once; /* one-time notice flag */
    int printed_match_once;    /* one-time success flag */
} t_pw_in_t;

static t_class *pw_in_class;

/* --------------------------- PipeWire callbacks --------------------------- */
static void pw_on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    (void)data;
    (void)id;
    (void)param;
}

static void pw_on_process(void *data) {
    /* Producer: write capture frames into ringbuffer */
    t_pw_in_t *x = (t_pw_in_t *)data;
    if (!x || !x->stream)
        return;

    struct pw_buffer *b = pw_stream_dequeue_buffer(x->stream);
    if (!b)
        return;

    struct spa_buffer *buf = b->buffer;
    if (!buf) {
        pw_stream_queue_buffer(x->stream, b);
        return;
    }
    if (buf->n_datas <= 0) {
        pw_stream_queue_buffer(x->stream, b);
        return;
    }
    if (buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(x->stream, b);
        return;
    }

    /* Determine pointer and size safely, honour chunk->offset/size if present */
    void *base = buf->datas[0].data;
    size_t maxsize = (size_t)buf->datas[0].maxsize;
    size_t chunk_offset = 0;
    size_t chunk_size = 0;
    if (buf->datas[0].chunk) {
        chunk_offset = (size_t)buf->datas[0].chunk->offset;
        chunk_size = (size_t)buf->datas[0].chunk->size;
    }

    /* Validate offset */
    if (chunk_offset > maxsize) {
        pw_stream_queue_buffer(x->stream, b);
        return;
    }

    size_t nbytes = chunk_size ? chunk_size : (maxsize - chunk_offset);
    if (nbytes == 0) {
        pw_stream_queue_buffer(x->stream, b);
        return;
    }

    const void *data_ptr = (const char *)base + chunk_offset;

    uint32_t stride = (uint32_t)x->ochcount * sizeof(float);
    uint32_t nframes = (stride > 0) ? (uint32_t)(nbytes / stride) : 0;

    /* One-time notice: whether our 64-frame request was honoured */
    if (nframes > 0) {
        if ((unsigned)nframes == x->desired_block) {
            if (!x->printed_match_once) {
                post("pwin~: server honoured requested blocksize: %u frames", nframes);
                x->printed_match_once = 1;
            }
        } else {
            if (!x->printed_mismatch_once) {
                post("pwin~: requested %u frames but server provides %u frames (notice)",
                     x->desired_block, nframes);
                x->printed_mismatch_once = 1;
            }
        }
    }

    if (nframes > 0) {
        /* write as interleaved float frames */
        rb_write(&x->ring, (const float *)data_ptr, (size_t)nframes, (int)x->ochcount);
    }

    pw_stream_queue_buffer(x->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = pw_on_param_changed,
    .process = pw_on_process,
};

/* ---------------- PipeWire setup/teardown using thread-loop --------------- */
static int pw_setup_stream(t_pw_in_t *x) {
    /* Build properties before creating the stream */
    char rate_prop[32];
    unsigned sr = (unsigned)(x->sr > 0 ? x->sr : 48000);
    snprintf(rate_prop, sizeof(rate_prop), "%u/1", sr);

    char latency_prop[32];
    /* node.latency is a fraction "frames/sample_rate" */
    snprintf(latency_prop, sizeof(latency_prop), "%u/%u", x->desired_block, sr);

    struct pw_properties *props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Production", NULL);

    /* Request 64-frame quantum at the selected sample rate */
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_prop); /* e.g., "64/48000" */
    pw_properties_set(props, PW_KEY_NODE_RATE, rate_prop);       /* e.g., "48000/1" */

    x->stream = pw_stream_new_simple(pw_thread_loop_get_loop(x->tloop), "pwin~",
                                     props, /* owned by stream */
                                     &stream_events, x);
    if (!x->stream)
        return -1;

    /* Desired format: float32, interleaved, N channels */
    struct spa_audio_info_raw info;
    spa_zero(info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = (uint32_t)x->ochcount;
    info.rate = sr;

    /* Build format + latency + small buffers */
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[3];
    uint32_t n_params = 0;

    /* Format pod */
    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_format, SPA_POD_Id(info.format),
        SPA_FORMAT_AUDIO_channels, SPA_POD_Int((int)info.channels), SPA_FORMAT_AUDIO_rate,
        SPA_POD_Int((int)info.rate));

    /* Latency pod: request a 64-frame quantum */
    struct spa_latency_info lat;
    spa_zero(lat);
    lat.direction = SPA_DIRECTION_INPUT;
    lat.min_quantum = (float)x->desired_block;
    lat.max_quantum = (float)x->desired_block;

    uint8_t lb[256];
    struct spa_pod_builder lbuilder = SPA_POD_BUILDER_INIT(lb, sizeof(lb));
    const struct spa_pod *latpod = spa_latency_build(&lbuilder, SPA_PARAM_Latency, &lat);
    params[n_params++] = latpod;

    /* Small buffers matching 64 frames (server may still choose differently) */
    int frames = (int)x->desired_block;
    int stride = (int)x->ochcount * (int)sizeof(float);
    int size_bytes = frames * stride;

    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_Int(MAX_BUFFERS), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
        SPA_POD_Int(size_bytes), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride));

    uint32_t flags =
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;

    if (pw_stream_connect(x->stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, n_params) < 0) {
        return -1;
    }
    return 0;
}

static int pw_start(t_pw_in_t *x) {
    x->tloop = pw_thread_loop_new("pwin~", NULL);
    if (!x->tloop)
        return -1;

    if (pw_thread_loop_start(x->tloop) != 0)
        return -1;

    /* Create PW objects while holding the loop lock */
    pw_thread_loop_lock(x->tloop);
    int ok = pw_setup_stream(x);
    pw_thread_loop_unlock(x->tloop);
    if (ok < 0)
        return -1;

    return 0;
}

static void pw_stop(t_pw_in_t *x) {
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

/* ------------------------------ Pd DSP (variable outs) -------------------------------- */
static t_int *pw_in_perform(t_int *w) {
    t_pw_in_t *x = (t_pw_in_t *)(intptr_t)w[1];
    int n = (int)w[2];

    if ((size_t)n > x->tmp_cap) {
        /* allocate interleaved tmp buffer of n frames */
        size_t need = (size_t)n * (size_t)x->ochcount;
        float *tmp = (float *)realloc(x->tmp_iobuf, need * sizeof(float));
        if (!tmp) {
            /* allocation failed: output zeros to keep Pd stable */
            for (unsigned ch = 0; ch < x->ochcount; ch++) {
                t_sample *outX = (t_sample *)(intptr_t)w[3 + ch];
                for (int i = 0; i < n; i++)
                    outX[i] = 0;
            }
            return (w + (3 + x->ochcount));
        }
        x->tmp_iobuf = tmp;
        x->tmp_cap = (size_t)n;
    }

    /* Pull interleaved from ringbuffer; zero-fill on underrun */
    size_t got = rb_read(&x->ring, x->tmp_iobuf, (size_t)n, (int)x->ochcount);
    if (got < (size_t)n) {
        memset(x->tmp_iobuf + got * x->ochcount, 0, (n - (int)got) * x->ochcount * sizeof(float));
    }

    /* Deinterleave to Pd signal outlets:
       output vectors are at w[3 + ch], ch in [0, ochcount) */
    for (unsigned ch = 0; ch < x->ochcount; ch++) {
        t_sample *outX = (t_sample *)(intptr_t)w[3 + ch];
        for (int i = 0; i < n; i++) {
            outX[i] = (t_sample)x->tmp_iobuf[i * x->ochcount + ch];
        }
    }

    /* nargs = 2 + ochcount => return w + (nargs + 1) = w + (3 + ochcount) */
    return (w + (3 + x->ochcount));
}

static void pw_in_dsp(t_pw_in_t *x, t_signal **sp) {
    /* take sr from first signal vector */
    x->sr = (int)sp[0]->s_sr;

    /* nargs = x + n + ochcount output vectors */
    const int nargs = 2 + (int)x->ochcount;

    t_int *sigvec = (t_int *)getbytes((size_t)nargs * sizeof(t_int));
    if (!sigvec)
        return;

    sigvec[0] = (t_int)(intptr_t)x;
    sigvec[1] = (t_int)sp[0]->s_n;
    for (unsigned ch = 0; ch < x->ochcount; ch++) {
        sigvec[2 + ch] = (t_int)(intptr_t)sp[ch]->s_vec;
    }

    dsp_addv(pw_in_perform, nargs, sigvec);
    freebytes(sigvec, (size_t)nargs * sizeof(t_int));
}

/* --- Pd object creation --- */
static void *pw_in_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    t_pw_in_t *x = (t_pw_in_t *)pd_new(pw_in_class);

    unsigned ochcount = 2;
    if (argc > 0 && argv && atom_getfloat(argv) > 0) {
        int requested = (int)atom_getfloat(argv);
        if (requested < 1)
            requested = 1;
        if (requested > MAX_CHANNELS)
            requested = MAX_CHANNELS;
        ochcount = (unsigned)requested;
    }

    /* Create N signal outlets */
    for (unsigned i = 0; i < ochcount; i++) {
        outlet_new(&x->x_obj, &s_signal);
    }

    x->tloop = NULL;
    x->stream = NULL;
    x->tmp_iobuf = NULL;
    x->tmp_cap = 0;
    x->sr = 48000;
    x->ochcount = ochcount;

    x->desired_block = DESIRED_BLOCK;
    x->printed_mismatch_once = 0;
    x->printed_match_once = 0;

    /* Ringbuffer: ~16 Pd blocks (64 frames) to keep latency modest */
    rb_init(&x->ring, DESIRED_BLOCK * 16, (int)x->ochcount);

    if (pw_start(x) != 0) {
        post("pwin~: failed to start PipeWire; object will output silence");
        /* keep object alive; perform will output zeros on underrun */
    }
    return x;
}

static void pw_in_free(t_pw_in_t *x) {
    pw_stop(x);
    rb_free(&x->ring);
    free(x->tmp_iobuf);
}

void pwin_tilde_setup(void) {
    /* Initialize PipeWire once per process */
    pw_init(NULL, NULL);

    pw_in_class = class_new(gensym("pwin~"), (t_newmethod)pw_in_new, (t_method)pw_in_free,
                            sizeof(t_pw_in_t), CLASS_DEFAULT, A_GIMME, 0);

    /* Not strictly needed (no signal inlet), but keeps t_x defined */
    CLASS_MAINSIGNALIN(pw_in_class, t_pw_in_t, t_x);
    class_addmethod(pw_in_class, (t_method)pw_in_dsp, gensym("dsp"), A_CANT, 0);
}
