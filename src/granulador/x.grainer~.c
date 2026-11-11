/*grainer by Oscar Pablo Di Liscia
Granular synthesis external with 3D per grain spatialisation for Pure Data
Programa de Investigación: Sistemas Temporales y Sintesis Espacial en el Arte Sonoro
odiliscia@unq.edu.ar
Last version 03-01-2018
glissando of the grains, blocking console messages and grain pause/restart were added by
Damián Anache, 06/01/2016
*/
/*
todo:
Better resampling quality.
Add the list selection for envelopes. (add method in grainer_tilde_any and modify set_oscil)
Improve the data structure (it works, but could be much simpler)
*/

#include <m_pd.h>
#include <math.h>
// #include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static t_class *grainer_tilde_class;

#define PI 3.14159265358979323846264338327
#define PI2 6.28318530717958647692528676654
#define DEG90 1.570796326794896619231321691635
#define DEG45 (DEG90 / 2.)
#define DEG135 DEG90 + DEG45
/*first and second order scalers for Ambisonics encoding, FuMa Set*/
#define ASCAL 0.707106781  /* 1/sqrt(2) */
#define ASCAL2 0.726184377 /* sqrt(135/256)*/
#define ASCAL3 2.598076211 /* sqrt(27/4)*/
#define MONO 1
#define IPAN 2
#define AMBI 4
#define AMBI2 9
#define AMBI3 16
#define MAX_OUTPUTS 17
/*distance*/
#define MIN_DIST 1.
#define MAX_DIST 50.
#define MAXDELAY 0.147058824 /* 50mts/340 mts/seg */
#define SOUND_SPEED 340.
#define MIN_SOUND_SPEED 300.
#define DIST_EXP 1.

/*general misc*/
#define TRUE 1
#define FALSE 0
#define MIN_GAP (t_float)0.001
#define MIN_DUR (t_float)0.001
#define MAX_GRAINS 100
#define MAX_TABLES 24
#define AUDIO_TABLE 1
#define ENVELOPE_TABLE 2
#define REP 0

#define NLSELS 6
#define RND 0
#define CYC 1
#define SHU 2
#define DEVS 0
#define GAPS 1
#define DURS 2
#define PSTS 3
#define PSES 4
#define AUDS 5

#define NOLOOP 0
#define LOOPF 1
#define LOOPFB 2
#define MAX_AUDIO_TABLES 24

// ─────────────────────────────────────
// Macros
/*conversions*/
/*dB value from linear value*/
#define LIN2dB(x) (double)(20. * log10(x))
/*compute oscilator increment from frequency*/
#define ITOF(frec, L, sr) frec *((t_float)L / (t_float)sr)
/*compute an increment of s semitones*/
#define S2INCR(s) (pow(2., (double)s / 12.))
/*convert angles*/
#define DEG2RAD(deg) (deg / 360.) * PI2
#define RAD2DEG(rad) (rad / PI2) * 360.
/*get a random value between 0 and "range"*/
#define RAND_FLOAT(range) range *((t_float)rand() / (t_float)RAND_MAX)
/*get a random value between -range and range*/
#define BIRAND_FLOAT(range) (range * 2. * ((t_float)rand() / (t_float)RAND_MAX)) - range
/*get an int random value between 0 and "range-1"*/
#define RAND_INT(range) rand() % range

// ─────────────────────────────────────
typedef struct {
    double index;
    double incr;
    int fros;
    int ends;
    int durs;
    int LT;           /*loop type */
    int Ldir;         /*loop direction */
    t_float gliss;    /*incr mutiplier for glissandi */
    t_float glissrnd; /*random deviation for glissandi*/
    int nt;           /*this is the table number in the table array*/
} OSCIL;

// ─────────────────────────────────────
// Grain state
typedef struct {
    int size;                 /*grain size in samples*/
    t_float incr;             /*increment for the grain table*/
    t_float amp;              /*grain amplitude*/
    t_float incra;            /*increment for the amp table so it fits the grain duration*/
    t_float azi;              /*azimuth angle in radians*/
    t_float ele;              /*elevation angle in radians*/
    t_float dist;             /*distance in meters*/
    t_float gch[MAX_OUTPUTS]; /*gain values for each channel*/
    int s_count;              /*internal sample counter*/
    OSCIL *gosc;              /*oscilator for the grain*/
    OSCIL *aosc;              /*oscilator for the amplitude func*/
    int delflag;
    t_float *delbuf;  /*delay buffer*/
    int delpo;        /*delay reading pointer*/
    int delsi;        /*delay size in samples*/
    t_float dry;      /*output sample delayed*/
    t_float send;     /*output sample non-delayed*/
    t_float distaten; /*auxiliar variable for distance attenuation*/
} GRAIN;

// ─────────────────────────────────────
// Audio and envelope tables
typedef struct {
    t_symbol *name; // array name
    t_word *table;  // table vector
    int size;       // table size
} TABLE;

// ─────────────────────────────────────
// List selection state
typedef struct {
    int size; // list size
    int type; // data selection type: RND, CYC, SHU
    int cont; // internal counter
    t_float *data;
} LSEL;
// ─────────────────────────────────────
typedef struct _grainer_tilde {
    t_object x_obj;
    t_sample f;
    GRAIN **g; /*a pointer to an array of pointers of GRAIN structures*/
    int ng;    /*number of grain pointers currently allocated*/

    TABLE audio_tables[MAX_AUDIO_TABLES];    /*audio table structure array*/
    TABLE envelope_tables[MAX_AUDIO_TABLES]; /*envelope table structure array*/
    int audio_table_count;                   /*number of available audio tables*/
    int envelope_table_count;                /*number of available envelope tables*/
    int active_audio_table;                  /*index of active audio table*/
    int active_envelope_table;               /*index of active envelope table*/

    int nch;       /*number of audio output channels*/
    int active;    /*active flag*/
    int pause;     /*pause flag*/
    t_float sr;    /*sampling rate*/
    int post_ctrl; /*enables and disables console posts*/

    int dosend; /*enables and disables reverberation send*/

    t_float sound_speed; /*actual sound speed*/
    t_float max_dist;    /*maximal distance*/
    t_float dist_exp;    /*distance exponent*/
    int delflag;
    ///////////////////////////////////////////////////////////
    /*GRAINER PARAMETERS*/
    t_float agr;   /*grain amplitude*/
    t_float agrnd; /*grain amplitude random deviation in percent of the grain amplitude*/
    t_float fgr;   /*grain frequency scaler*/
    t_float fgrnd; /*grain increment random deviation percent*/
    t_float sgr;   /*grain size in secs.*/
    t_float sgrnd; /*grain size random deviation in percent of grain size (0=invariant, 1=100%)*/

    t_float stgr; /*where to start the grain in the table, in seconds*/
    t_float
        stgrnd;   /*starting point random deviation in percent of the table (0=invariant, 1=100%)*/
    t_float engr; /*where to end the grain in the audio table, in seconds table 0=read the table
                     modulo size*/
    t_float engrnd; /*ending point random deviation in percent of the table (0=invariant, 1=100%)*/
    int loop_type;  /*0=noloop 1=forward, 2=forward-backward, use if needed
                                    Note that when loop_type=0, engr and engrnd are not taken in
                       account*/
    int loop_direction; /*internal temporary value for forward-backward loop type*/

    t_float gli;  /*incr mutiplier for glissandi */
    t_float glir; /*incr mutiplier for glissandi random deviation percent*/

    t_float gagr;   /*gap between grains in secs.*/
    t_float gagrnd; /*gap random deviation in percent of the gap 0=invariant, 1=gap, 2=twice, etc.*/
    int gap_samps;  /*actual gap in samples*/
    t_float azgr;   /*azimuth center, in radians 0 (0 Deg.) to PI (90 Deg.) for stereo*/
    t_float azgrnd; /*azimuth center random deviation in degrees*/
    t_float elgr;   /*elevation center, in radians 0 (0 Deg.) to PI (90 Deg.) for stereo*/
    t_float elgrnd; /*elevation center random deviation in degrees*/
    t_float digr;   /*grain distance*/
    t_float digrnd; /*grain distance random deviation*/

    LSEL lsel[NLSELS]; /*list selection structures*/

    int gap_count;            /*gap samples counter*/
    int grain_count_interval; /*how many grains we need to generate to output a bang*/
    int grain_count;          /*grain counter*/
    t_outlet *out_bang;       /*outlet to output a bang each time that gcound grains are generated*/
    t_float osig[MAX_OUTPUTS]; /*array to store the output, may be stereo (intensity panning) or
                                  (Ambisonic B Format) 4, 9 16 channels*/
                               /*WARNING: osig[nch] is used to store the reverb send*/
} t_grainer_tilde;

// ─────────────────────────────────────
// Function declarations
static OSCIL *create_oscillator(int start, int end, int loop_type, int loop_direction,
                                t_float glissando, t_float glissando_random, double increment,
                                t_grainer_tilde *x, int table_type);
static t_float read_audio_table(OSCIL *oscillator, t_float amplitude, t_grainer_tilde *x);
static t_float read_envelope_table(OSCIL *oscillator, t_float amplitude, t_grainer_tilde *x);
static void grainer_tilde_tables(t_grainer_tilde *x, t_symbol *name, int which, char *type);
static t_float randomize_value(t_float base, t_float random_deviation);
static void compute_gap(t_grainer_tilde *x);
static void compute_channel_gains(t_grainer_tilde *x, GRAIN *grain);
static GRAIN *create_grain(t_grainer_tilde *x);
static void synthesize_grain(GRAIN *grain, t_grainer_tilde *x);
static void initialize_grainer(t_grainer_tilde *x);
static void process_grains(GRAIN ***grains, t_grainer_tilde *x);
static void print_grainer_state(t_grainer_tilde *x);
static void format_float_array(int size, t_float *data, char *string);
static void reset_gap(t_grainer_tilde *x);
static void set_selection_list(t_grainer_tilde *x, t_int argc, t_atom *argv, int list_index,
                               char *description);
static t_float select_list_value(t_grainer_tilde *x, int list_index);
static void grainer_tilde_free(t_grainer_tilde *x);

// ─────────────────────────────────────
/*Returns a float value from a list according the selection type set (RANDOM, CYCLE, SHUFFLE)*/
/*Warning! In the case of a list with pairs of values (start/dur) it returns the index...NOT the
 * value*/
static t_float select_list_value(t_grainer_tilde *x, int what) {
    int u, index, i, add, size;
    t_float s;

    // just for pairs or single values
    if (what == PSES) {
        size = x->lsel[what].size / 2;
        add = 2;
    } else {
        size = x->lsel[what].size;
        add = 1;
    }

    switch (x->lsel[what].type) {
    case RND: {
        index = RAND_INT(size) * add;
        break;
    }
    case CYC: {
        index = x->lsel[what].cont;
        x->lsel[what].cont += add;
        if (x->lsel[what].cont == x->lsel[what].size)
            x->lsel[what].cont = 0;
        break;
    }
    case SHU: {
        if (x->lsel[what].cont == 0) { /*shuffle array content values every "size" times*/

            for (i = 0; i < size; i++) {
                u = RAND_INT(size) * add;
                s = x->lsel[what].data[i * add];
                x->lsel[what].data[i * add] = x->lsel[what].data[u];
                x->lsel[what].data[u] = s;
                if (what == PSES) {
                    s = x->lsel[what].data[i * add + 1];
                    x->lsel[what].data[i * add + 1] = x->lsel[what].data[u + 1];
                    x->lsel[what].data[u + 1] = s;
                }
            }
        }
        index = x->lsel[what].cont;
        x->lsel[what].cont += add;
        if (x->lsel[what].cont == x->lsel[what].size)
            x->lsel[what].cont = 0;
        break;
    }
    }
    if (what != PSES)
        return (x->lsel[what].data[index]);
    else
        return ((float)index);
}
// ─────────────────────────────────────
static void set_selection_list(t_grainer_tilde *x, t_int argc, t_atom *argv, int what,
                               char *whats) {
    int i;
    t_float aux;
    t_symbol *temp;

    if (argc == 0) {
        x->lsel[what].type = 0;
        x->lsel[what].size = 0;
        x->lsel[what].cont = 0;
        if (x->post_ctrl)
            post(" grainer~: %s list unset", whats);
        return;
    }

    // check if the cycling or shuffle selection methods are requested and set the type accordingly
    temp = atom_getsymbol(&argv[argc - 1]);
    post(" grainer~: %s ", temp->s_name);
    if (strcmp(temp->s_name, "float") == 0) {
        x->lsel[what].type = RND;
        x->lsel[what].size = argc;
        x->lsel[what].cont = 0;
    } else {
        x->lsel[what].size = argc - 1;
        x->lsel[what].cont = 0;
        if (strcmp(temp->s_name, "CYC") == 0)
            x->lsel[what].type = CYC;
        if (strcmp(temp->s_name, "SHU") == 0)
            x->lsel[what].type = SHU;
    }

    // if the list is larger than MAX_TABLES (24 elements), then do extra memory allocation, if
    // needed
    if (x->lsel[what].size > MAX_AUDIO_TABLES) {

        if (x->lsel[what].data)
            free(x->lsel[what].data);
        x->lsel[what].data = (t_float *)getbytes(sizeof(t_float) * x->lsel[what].size);
    }
    // load the data in the corresponding array
    for (i = 0; i < x->lsel[what].size; ++i) {
        if (what == DEVS) {
            x->lsel[what].data[i] = (t_float)atom_getfloat(&argv[i]);
        } else {
            x->lsel[what].data[i] = (t_float)fabs(atom_getfloat(&argv[i]));
        }
        if (what == AUDS) { /*must ensure that the tables requested were previously allocated*/
            aux = fabs(atom_getfloat(&argv[i]));
            if (!x->audio_tables[(int)aux].size) {
                x->lsel[what].size = 0;
                post(" grainer~: non allocated audio table (%d) requested, use the "
                     "'audio_table' method to allocate it first",
                     (int)aux);
                return;
                x->lsel[what].data[i] = aux;
            }
        }
    }

    if (x->post_ctrl)
        post(" grainer~: %s list set size=%d stype=(%d)", whats, x->lsel[what].size,
             x->lsel[what].type);

    return;
}
// ─────────────────────────────────────
static void grainer_tilde_activate_if_ready(t_grainer_tilde *x) {
    if (x->g) { /*must keep previous setup parameters*/
        if (x->audio_table_count && x->envelope_table_count) {
            x->active = 1;
            x->pause = 0;
            return;
        }
    } else {
        if (x->audio_table_count && x->envelope_table_count) {
            /*allocate first grain*/
            x->g = (GRAIN **)malloc(sizeof(GRAIN *));
            x->ng = 1;
            x->g[0] = (GRAIN *)create_grain(x);
            x->active = 1;
            x->pause = 0;
            post(" grainer~: initialized");
        }
    }
    return;
}
// ─────────────────────────────────────
static void grainer_tilde_use_audio_table(t_grainer_tilde *x, t_float f) {
    int index = (int)f;

    if (!x->audio_tables[index].size) {
        post(" grainer~: non allocated audio table requested, use the 'audio_table' method to "
             "allocate it first");
        return;
    }
    x->active_audio_table = index;
    /*if random selection of tables by a list was active, set it inactive forcing the list size to
     * 0*/
    if (x->lsel[AUDS].size) {
        x->lsel[AUDS].size = 0;
        post(" grainer~: list selection of grain tables unset ");
    }
    return;
}
// ─────────────────────────────────────
static void grainer_tilde_use_envelope_table(t_grainer_tilde *x, t_float f) {
    int index = (int)f;

    if (!x->envelope_tables[index].size) {
        post(" grainer~: non allocated envelope table requested, use the 'envelope_table' method "
             "to allocate it first");
        return;
    }
    x->active_envelope_table = index;
    return;
}
// ─────────────────────────────────────
static void grainer_tilde_message(t_grainer_tilde *x, t_symbol *string, t_int argc, t_atom *argv) {
    int i;
    t_float f;
    t_symbol *temp2;

    /*stop*/
    if (strcmp(string->s_name, "stop") == 0) {
        x->active = 0;
        post(" grainer~: stopped");
        return;
    }
    /*start*/
    if (strcmp(string->s_name, "start") == 0) {
        if (x->g && x->audio_table_count && x->envelope_table_count) {
            x->active = 1;
            post(" grainer~: active");
        }
        return;
    }

    /*audio table*/
    if (strcmp(string->s_name, "audio_table") == 0) {
        temp2 = atom_getsymbol(&argv[0]);
        grainer_tilde_tables(x, temp2, AUDIO_TABLE, "audio");
        return;
    }

    /*envelope table*/
    if (strcmp(string->s_name, "envelope_table") == 0) {
        temp2 = atom_getsymbol(&argv[0]);
        grainer_tilde_tables(x, temp2, ENVELOPE_TABLE, "envelope");
        return;
    }

    if (strcmp(string->s_name, "grain_count") == 0) {
        f = atom_getfloat(&argv[0]);
        x->grain_count = (int)f;
        x->grain_count_interval = x->grain_count;
        post(" grainer~: grain count set to %d", x->grain_count);
        return;
    }

    /*pause*/
    if (strcmp(string->s_name, "pause") == 0) {
        f = atom_getfloat(&argv[0]);
        i = (int)f;
        if (i != 1. && i != 0.) {
            post(" grainer~: unknown pause value %i (only 0 or 1 are available)", i);
            return;
        }
        x->pause = i;
        if (x->post_ctrl) {
            if (i)
                post(" grainer~: paused");
            else
                post(" grainer~: resumed");
        }
        return;
    }

    /*seed*/
    if (strcmp(string->s_name, "seed") == 0) {
        f = atom_getfloat(&argv[0]);
        if (x->post_ctrl)
            post(" grainer~: seeding with %f", f);
        srand(f);
        return;
    }

    /*print*/
    if (strcmp(string->s_name, "print") == 0) {
        print_grainer_state(x);
        return;
    }

    /*grain gap*/
    if (strcmp(string->s_name, "gap") == 0) {
        f = fabs(atom_getfloat(&argv[0]));
        x->gagr = (f < MIN_GAP ? MIN_GAP : f);
        return;
    }
    /*grain gap random deviation*/
    if (strcmp(string->s_name, "gap_random_deviation") == 0) {
        x->gagrnd = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain size*/
    if (strcmp(string->s_name, "duration") == 0) {
        f = fabs(atom_getfloat(&argv[0]));
        x->sgr = (f < MIN_DUR ? MIN_DUR : f);
        return;
    }
    /*grain size random deviation*/
    if (strcmp(string->s_name, "duration_random_deviation") == 0) {
        x->sgrnd = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain frequency scaler (increment)*/
    if (strcmp(string->s_name, "playback_rate") == 0) {
        x->fgr = atom_getfloat(&argv[0]);
        return;
    }
    /*grain frequency scaler (increment)random deviation*/
    if (strcmp(string->s_name, "playback_rate_random_deviation") == 0) {
        x->fgrnd = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain table starting point (seconds)*/
    if (strcmp(string->s_name, "start_position") == 0) {
        x->stgr = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain table starting point random deviation*/
    if (strcmp(string->s_name, "start_position_random_deviation") == 0) {
        x->stgrnd = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain table ending point (seconds)*/
    if (strcmp(string->s_name, "end_position") == 0) {
        x->engr = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain table ending point random deviation*/
    if (strcmp(string->s_name, "end_position_random_deviation") == 0) {
        x->engrnd = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*loop type*/
    if (strcmp(string->s_name, "loop_type") == 0) {
        f = atom_getfloat(&argv[0]);
        if (f > 2. || f < 0.) {
            post(" grainer~: unknown loop_type option %f (only 0, 1 or 2 are available)", f);
            return;
        }
        x->loop_type = (int)f;
        x->loop_direction = 0;
        if (x->post_ctrl)
            post(" grainer~: loop type set to: %d", (int)f);
        return;
    }
    /*glissando added by Damián Anache, 06/01/2016*/
    if (strcmp(string->s_name, "glissando") == 0) {
        f = atom_getfloat(&argv[0]);
        if (f < 0.) {
            post(" grainer~: wrong gliss value %f (only positive values are allowed)", f);
            return;
        }
        x->gli = f;
        if (x->post_ctrl)
            post(" grainer~: new glissandi multiplier: %f", f);
        return;
    }
    /*glissando random deviation added by Damián Anache, 06/01/2016*/
    if (strcmp(string->s_name, "glissando_random_deviation") == 0) {
        f = atom_getfloat(&argv[0]);
        if (f < 0.) {
            post(" grainer~: wrong gliss random deviation value %f (only positive values are "
                 "allowed)",
                 f);
            return;
        }
        x->glir = f;
        if (x->post_ctrl)
            post(" grainer~: new glissandi random deviation: %f", f);
        return;
    }
    /*grain amp */
    if (strcmp(string->s_name, "amplitude") == 0) {
        x->agr = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain amp random deviation*/
    if (strcmp(string->s_name, "amplitude_random_deviation") == 0) {
        x->agrnd = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain azimuth */
    if (strcmp(string->s_name, "azimuth") == 0) {
        x->azgr = DEG2RAD(atom_getfloat(&argv[0]));
        return;
    }
    /*grain azimuth random deviation*/
    if (strcmp(string->s_name, "azimuth_random_deviation") == 0) {
        x->azgrnd = DEG2RAD(atom_getfloat(&argv[0]));
        return;
    }
    /*grain elevation */
    if (strcmp(string->s_name, "elevation") == 0) {
        x->elgr = DEG2RAD(atom_getfloat(&argv[0]));
        return;
    }
    /*grain elevation random deviation*/
    if (strcmp(string->s_name, "elevation_random_deviation") == 0) {
        x->elgrnd = DEG2RAD(atom_getfloat(&argv[0]));
        return;
    }
    /*grain distance */
    if (strcmp(string->s_name, "distance") == 0) {
        x->digr = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*grain distance random deviation*/
    if (strcmp(string->s_name, "distance_random_deviation") == 0) {
        x->digrnd = fabs(atom_getfloat(&argv[0]));
        return;
    }
    /*********************************************************************************/
    /*grain frequency deviation list*/
    if (strcmp(string->s_name, "pitch_list") == 0) {

        set_selection_list(x, argc, argv, DEVS, "pitch");
        for (i = 0; i < x->lsel[0].size; ++i) {
            x->lsel[0].data[i] = S2INCR(x->lsel[0].data[i]);
        }
        return;
    }
    /*gap values selection list*/
    if (strcmp(string->s_name, "gap_list") == 0) {
        set_selection_list(x, argc, argv, GAPS, "gap");
        return;
    }
    /*dur values selection list*/
    if (strcmp(string->s_name, "duration_list") == 0) {
        set_selection_list(x, argc, argv, DURS, "duration");
        x->lsel[PSES].size = 0;
        return;
    }
    /*file access time values selection list*/
    if (strcmp(string->s_name, "position_list") == 0) {
        set_selection_list(x, argc, argv, PSTS, "audio table access times");
        x->lsel[PSES].size = 0;
        return;
    }
    /*file access time values and dur selection list*/
    /*the number of elements must be in pairs time dur, time dur...etc.*/
    if (strcmp(string->s_name, "position_duration_list") == 0) {
        /*
                if(argc % 2 != 0) {
                        if(x->post_ctrl)
                                post(" grainer~: warning, final duration value is missing, ommiting
           last access time value"); aux=1;
                }
                if(argc-aux > 0 && argc-aux <= 3 ) {
                        if(x->post_ctrl)
                                post(" grainer~: list must have at least two pairs of time and
           duration values \n no action taken"); return;
                }
        */
        set_selection_list(x, argc, argv, PSES, "audio table access and size times");
        /*set durs and acces times list to 0*/
        x->lsel[DURS].size = x->lsel[PSTS].size = 0;
        return;
    }
    /*audio table numbers selection list*/
    if (strcmp(string->s_name, "audio_table_list") == 0) {
        set_selection_list(x, argc, argv, AUDS, "audio table");
        return;
    }
    /*********************************************************************************/
    /*gap_restart added by Damián Anache, 06/01/2016*/
    if (strcmp(string->s_name, "reset_gap") == 0) {
        reset_gap(x);
        return;
    }

    /*block_console_messages added by Damián Anache, 06/01/2016*/
    if (strcmp(string->s_name, "verbose") == 0) {
        f = atom_getfloat(&argv[0]);
        i = (int)f;
        if (i != 1. && i != 0.) {
            post("grainer~: unknown verbose value %i (only 0 or 1 are available)", i);
            return;
        }
        x->post_ctrl = i;
        if (i)
            post("grainer~: console posts are enable");
        else
            post("grainer~: console posts are disable");
        return;
    }

    /*sound speed*/
    if (strcmp(string->s_name, "sound_speed") == 0) {
        x->sound_speed = atom_getfloat(&argv[0]);
        if (x->sound_speed <= MIN_SOUND_SPEED) {
            post("grainer~: ilegal sound speed requested (%f m/s), minimal is (%fm/s) using "
                 "default value (340 m/s)",
                 x->sound_speed, MIN_SOUND_SPEED);
            x->sound_speed = SOUND_SPEED;
        } else
            post("grainer~: sound speed changed, now is %f m/s", x->sound_speed);
        return;
    }
    /*distance exponent*/
    if (strcmp(string->s_name, "distance_exponent") == 0) {
        x->dist_exp = atom_getfloat(&argv[0]);
        if (x->dist_exp <= 0.) {
            post("grainer~: illegal distance exponent requested (%f), using default value (1)",
                 x->dist_exp);
            x->dist_exp = 1.;
        } else
            post("grainer~: distance exponent changed, now is %f", x->dist_exp);
        return;
    }
    /*delay on-off*/
    if (strcmp(string->s_name, "distance_delay") == 0) {
        f = atom_getfloat(&argv[0]);
        i = (int)f;
        if (i != 1. && i != 0.) {
            post("grainer~: unknown distance_delay value %i (only 0 or 1 are available)", i);
            return;
        }
        x->delflag = i;
        if (i)
            post("grainer~: distance delay simulation on");
        else
            post("grainer~: distance delay simulation off");
        return;
    }

    return;
}
// ─────────────────────────────────────
static void grainer_tilde_tables(t_grainer_tilde *x, t_symbol *name, int which, char *type) {
    t_garray *a;
    int size = 0, i, chflag = TRUE, tloc = 0;
    int *table_count, *active_table;
    TABLE *tables;

    if (which == AUDIO_TABLE) {
        table_count = &x->audio_table_count;
        active_table = &x->active_audio_table;
        tables = x->audio_tables;
    } else {
        table_count = &x->envelope_table_count;
        active_table = &x->active_envelope_table;
        tables = x->envelope_tables;
    }

    if (!(a = (t_garray *)pd_findbyclass(name, garray_class))) {
        if (*name->s_name) {
            pd_error(x, " grainer~: ERROR, %s: no such array", name->s_name);
            tables[*active_table].size = 0;
            return;
        }
    }

    /*first check if it is an already used array name*/
    for (i = 0; i < *table_count; i++) {
        if (strcmp(tables[i].name->s_name, name->s_name) == 0) {
            *active_table = i;
            chflag = FALSE;
            break;
        }
    }
    if (chflag && *table_count == MAX_AUDIO_TABLES) {
        if (x->post_ctrl)
            post(" grainer~: maximal number of audio tables (%d) reached, no action taken ",
                 MAX_AUDIO_TABLES);
        return;
    }

    /*if random selection of tables by a list was active, set it inactive forcing the list size to
     * 0*/
    if (which == AUDIO_TABLE) {
        if (x->lsel[AUDS].size) {
            x->lsel[AUDS].size = 0;
            post(" grainer~: list selection of grain tables unset ");
        }
    }

    if (*table_count && !chflag) {
        tloc = *active_table;
    }
    if (chflag) { /*allocate a new table (set active table to the last one and increment the number
                     of tables)*/
        tloc = *table_count;
    }

    size = garray_npoints(a);
    if (!garray_getfloatwords(a, &size, &tables[tloc].table)) {
        pd_error(x, " grainer~ ERROR, %s: bad template for grainer~", name->s_name);
        tables[tloc].size = 0;
        return;
    }

    tables[tloc].size = size;
    tables[tloc].name = name;

    if (chflag) {
        *active_table = *table_count;
        *table_count += 1;
        if (x->post_ctrl)
            post(" grainer~: new %s table loaded is %s, %d samples ", type,
                 tables[tloc].name->s_name, tables[tloc].size);
        if (x->post_ctrl)
            post("              the number of active %s tables is %d, the current active table is "
                 "%d (%s) ",
                 type, *table_count, *active_table, tables[tloc].name->s_name);

    } else {
        if (x->post_ctrl)
            post(" grainer~: warning, %s table %s, overwritten %d samples ", type,
                 tables[tloc].name->s_name, tables[tloc].size);
        if (x->post_ctrl)
            post("              the number of active %s tables is %d, the current active table is "
                 "%d (%s) ",
                 type, *table_count, *active_table, tables[tloc].name->s_name);
    }

    grainer_tilde_activate_if_ready(x);
    garray_usedindsp(a);

    return;
}
// ─────────────────────────────────────
static t_int *grainer_tilde_perform_ambi3(t_int *w) {
    t_grainer_tilde *x = (t_grainer_tilde *)(w[1]);
    t_sample *W = (t_sample *)(w[2]);
    t_sample *X = (t_sample *)(w[3]);
    t_sample *Y = (t_sample *)(w[4]);
    t_sample *Z = (t_sample *)(w[5]);
    t_sample *R = (t_sample *)(w[6]);
    t_sample *S = (t_sample *)(w[7]);
    t_sample *T = (t_sample *)(w[8]);
    t_sample *U = (t_sample *)(w[9]);
    t_sample *V = (t_sample *)(w[10]);
    t_sample *K = (t_sample *)(w[11]);
    t_sample *L = (t_sample *)(w[12]);
    t_sample *M = (t_sample *)(w[13]);
    t_sample *N = (t_sample *)(w[14]);
    t_sample *O = (t_sample *)(w[15]);
    t_sample *P = (t_sample *)(w[16]);
    t_sample *Q = (t_sample *)(w[17]);

    t_sample *rsend = (t_sample *)(w[18]);
    int n = (int)(w[19]);

    if (!x->audio_table_count || !x->envelope_table_count || !x->active)
        goto zero;

    while (n--) {
        process_grains(&x->g, x);
        *W++ = x->osig[0];
        *X++ = x->osig[1];
        *Y++ = x->osig[2];
        *Z++ = x->osig[3];
        *R++ = x->osig[4];
        *S++ = x->osig[5];
        *T++ = x->osig[6];
        *U++ = x->osig[7];
        *V++ = x->osig[8];
        *K++ = x->osig[9];
        *L++ = x->osig[10];
        *M++ = x->osig[11];
        *N++ = x->osig[12];
        *O++ = x->osig[13];
        *P++ = x->osig[14];
        *Q++ = x->osig[15];
        *rsend++ = x->osig[16];
    }
    return (w + 20);
zero:
    while (n--) {
        *W++ = 0.;
        *X++ = 0.;
        *Y++ = 0.;
        *Z++ = 0.;
        *R++ = 0.;
        *S++ = 0.;
        *T++ = 0.;
        *U++ = 0.;
        *V++ = 0.;
        *K++ = 0.;
        *M++ = 0.;
        *N++ = 0.;
        *L++ = 0.;
        *O++ = 0.;
        *P++ = 0.;
        *Q++ = 0.;
        *rsend++ = 0.;
    }
    return (w + 20);
}
// ─────────────────────────────────────
static t_int *grainer_tilde_perform_ambi2(t_int *w) {
    t_grainer_tilde *x = (t_grainer_tilde *)(w[1]);
    t_sample *W = (t_sample *)(w[2]);
    t_sample *X = (t_sample *)(w[3]);
    t_sample *Y = (t_sample *)(w[4]);
    t_sample *Z = (t_sample *)(w[5]);
    t_sample *R = (t_sample *)(w[6]);
    t_sample *S = (t_sample *)(w[7]);
    t_sample *T = (t_sample *)(w[8]);
    t_sample *U = (t_sample *)(w[9]);
    t_sample *V = (t_sample *)(w[10]);

    t_sample *rsend = (t_sample *)(w[11]);
    int n = (int)(w[12]);

    if (!x->audio_table_count || !x->envelope_table_count || !x->active)
        goto zero;

    while (n--) {
        process_grains(&x->g, x);
        *W++ = x->osig[0];
        *X++ = x->osig[1];
        *Y++ = x->osig[2];
        *Z++ = x->osig[3];
        *R++ = x->osig[4];
        *S++ = x->osig[5];
        *T++ = x->osig[6];
        *U++ = x->osig[7];
        *V++ = x->osig[8];
        *rsend++ = x->osig[9];
    }
    return (w + 13);
zero:

    while (n--) {
        *W++ = 0.;
        *X++ = 0.;
        *Y++ = 0.;
        *Z++ = 0.;
        *R++ = 0.;
        *S++ = 0.;
        *T++ = 0.;
        *U++ = 0.;
        *V++ = 0.;
        *rsend++ = 0.;
    }
    return (w + 13);
}
// ─────────────────────────────────────
static t_int *grainer_tilde_perform_ambi(t_int *w) {
    t_grainer_tilde *x = (t_grainer_tilde *)(w[1]);
    t_sample *W = (t_sample *)(w[2]);
    t_sample *X = (t_sample *)(w[3]);
    t_sample *Y = (t_sample *)(w[4]);
    t_sample *Z = (t_sample *)(w[5]);
    t_sample *rsend = (t_sample *)(w[6]);
    int n = (int)(w[7]);

    if (!x->audio_table_count || !x->envelope_table_count || !x->active)
        goto zero;

    while (n--) {
        process_grains(&x->g, x);
        *W++ = x->osig[0];
        *X++ = x->osig[1];
        *Y++ = x->osig[2];
        *Z++ = x->osig[3];
        *rsend++ = x->osig[4];
    }
    return (w + 8);
zero:
    while (n--) {
        *W++ = 0.;
        *X++ = 0.;
        *Y++ = 0.;
        *Z++ = 0.;
        *rsend++ = 0.;
    }
    return (w + 8);
}
// ─────────────────────────────────────
static t_int *grainer_tilde_perform_ipan(t_int *w) {
    t_grainer_tilde *x = (t_grainer_tilde *)(w[1]);

    t_sample *out1 = (t_sample *)(w[2]);
    t_sample *out2 = (t_sample *)(w[3]);
    t_sample *rsend = (t_sample *)(w[4]);
    int n = (int)(w[5]);

    if (!x->audio_table_count || !x->envelope_table_count || !x->active)
        goto zero;

    while (n--) {
        process_grains(&x->g, x);
        *out1++ = x->osig[0];
        *out2++ = x->osig[1];
        *rsend++ = x->osig[2];
    }
    return (w + 6);

zero:
    while (n--) {
        *out1++ = 0.;
        *out2++ = 0.;
        *rsend++ = 0.;
    }
    return (w + 6);
}
// ─────────────────────────────────────
static t_int *grainer_tilde_perform_mono(t_int *w) {
    t_grainer_tilde *x = (t_grainer_tilde *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    t_sample *rsend = (t_sample *)(w[3]);
    int n = (int)(w[4]);

    if (!x->audio_table_count || !x->envelope_table_count || !x->active)
        goto zero;

    while (n--) {
        process_grains(&x->g, x);
        *out++ = x->osig[0];
        *rsend++ = x->osig[1];
    }
    return (w + 5);

zero:
    while (n--) {
        *out++ = 0.;
        *rsend++ = 0.;
    }
    return (w + 5);
}
// ─────────────────────────────────────
static void grainer_tilde_dsp(t_grainer_tilde *x, t_signal **sp) {
    int i, size;
    t_garray *a;

    /*it seems that we do not have other way of checking whether an array is alive*/
    for (i = 0; i < x->audio_table_count; i++) {
        if (!(a = (t_garray *)pd_findbyclass(x->audio_tables[i].name, garray_class))) {
            /*if a table was deleted we set its size to 0, so that the oscillator that is using it
             * will know*/
            x->audio_tables[i].size = 0;
            return;
        }
        size = garray_npoints(a);
        if (size < x->audio_tables[i].size) {
            x->audio_tables[i].size = size; /*did the user overwrite the audio table?*/
        }
    }
    for (i = 0; i < x->envelope_table_count; i++) { /*ditto...*/
        if (!(a = (t_garray *)pd_findbyclass(x->envelope_tables[i].name, garray_class))) {
            x->envelope_tables[i].size = 0;
            return;
        }
        size = garray_npoints(a);
        if (size < x->envelope_tables[i].size) {
            x->envelope_tables[i].size = size; /*ditto..*/
        }
    }
    /*now, if everything seems to be OK, we call the corresponding perform function*/
    switch (x->nch) {
    case (MONO): { /* mono */
        dsp_add(grainer_tilde_perform_mono, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
        break;
    }
    case (IPAN): { /* stereo intensity panning */
        dsp_add(grainer_tilde_perform_ipan, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
                sp[0]->s_n);
        break;
    }
    case (AMBI): { /* ambisonic B format, first order */
        dsp_add(grainer_tilde_perform_ambi, 7, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
                sp[3]->s_vec, sp[4]->s_vec, sp[0]->s_n);
        break;
    }
    case (AMBI2): { /* ambisonic B format, second order */
        dsp_add(grainer_tilde_perform_ambi2, 12, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
                sp[3]->s_vec, sp[4]->s_vec, sp[5]->s_vec, sp[6]->s_vec, sp[7]->s_vec, sp[8]->s_vec,
                sp[9]->s_vec, sp[0]->s_n);
        break;
    }
    case (AMBI3): { /* ambisonic B format, third order */
        dsp_add(grainer_tilde_perform_ambi3, 19, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
                sp[3]->s_vec, sp[4]->s_vec, sp[5]->s_vec, sp[6]->s_vec, sp[7]->s_vec, sp[8]->s_vec,
                sp[9]->s_vec, sp[10]->s_vec, sp[11]->s_vec, sp[12]->s_vec, sp[13]->s_vec,
                sp[14]->s_vec, sp[15]->s_vec, sp[16]->s_vec, sp[0]->s_n);
        break;
    }
    default: {
        break;
    }
    }
}
// ─────────────────────────────────────
static void *grainer_tilde_new(t_floatarg f) {
    int i;

    t_grainer_tilde *x = (t_grainer_tilde *)pd_new(grainer_tilde_class);

    initialize_grainer(x);

    x->nch = (int)f;

    /* AUDIO OUTLETS*/
    if (x->nch != MONO && x->nch != IPAN && x->nch != AMBI && x->nch != AMBI2 && x->nch != AMBI3) {
        x->nch = IPAN; /*default is stereo intensity panning*/
    }
    for (i = 0; i < x->nch + 1; ++i) {
        /*there will be nch+1 signal outlets because the nth+1 outlet is for the reverb send*/
        outlet_new(&x->x_obj, &s_signal);
    }

    x->out_bang = outlet_new(&x->x_obj, &s_bang); /*for bang each time n grains are generated*/

    switch (x->nch) {
    case (MONO): {
        post(" grainer~: mono output set ");
        break;
    }
    case (IPAN): {
        post(" grainer~: stereo output set (Intensity Panning)");
        break;
    }
    case (AMBI): {
        post(" grainer~: output set to First order Ambisonic B format \n signal outlets: W, X, Y, "
             "Z, Reverb_send");
        break;
    }
    case (AMBI2): {
        post(" grainer~: output set to Second order Ambisonic B format \n signal outlets: W, X, Y, "
             "Z, R, S, T, U, V, Reverb_send");
        break;
    }
    case (AMBI3): {
        post(" grainer~: output set to Third order Ambisonic B format \n signal outlets: W, X, Y, "
             "Z, R, S, T, U, V, K, L, M, N, O, P, Q, Reverb_send");
        break;
    }
    default: {
        break;
    }
    }
    return (void *)x;
}

// ─────────────────────────────────────
static void grainer_tilde_free(t_grainer_tilde *x) {
    int i;
    for (i = 0; i < x->ng; ++i) {
        if (x->g[i]->delbuf)
            freebytes(x->g[i]->delbuf, sizeof(t_float) * x->g[i]->delsi);
    }
    for (i = 0; i < NLSELS; ++i) {
        free(x->lsel[i].data);
    }
    if (x->g)
        freebytes(x->g, sizeof(GRAIN) * x->ng);
}

// ─────────────────────────────────────
static OSCIL *create_oscillator(int start, int end, int LT, int Ldir, t_float gliss,
                                t_float glissrnd, double incr, t_grainer_tilde *x, int which) {
    OSCIL *oscil;

    oscil = (OSCIL *)malloc(sizeof(OSCIL));

    if (which == AUDIO_TABLE) { /*audio oscillator*/
        oscil->incr = incr;
        oscil->index = (double)start;            /*initial phase in samples*/
        oscil->fros = start;                     /*initial read point in samples*/
        oscil->ends = end;                       /*ending read point in samples*/
        oscil->durs = oscil->ends - oscil->fros; /*loop size in samples*/
        oscil->LT = LT;                          /*loop type, see the macros for this*/
        oscil->Ldir = Ldir;                      /*loop direction*/
        /*step size for reaching (incr * gliss value) at the end of the grain*/
        oscil->gliss = ((incr * (gliss + BIRAND_FLOAT(glissrnd))) - incr) / (x->sgr * x->sr);
        /*set grain table */
        if (!x->lsel[AUDS].size) {
            oscil->nt = x->active_audio_table; /*current active table number*/
        } else {
            oscil->nt = (int)select_list_value(x, AUDS); /*random selection*/
        }

    } else { /*envelope oscillator*/
        oscil->index = 0.;
        oscil->incr = incr;
        oscil->nt = x->active_envelope_table; /*table number*/
    }
    return (oscil);
}

// ─────────────────────────────────────
static t_float read_audio_table(OSCIL *oscil, t_float amp, t_grainer_tilde *x) {
    /*table-lookup oscillator with loops forward and forward-backward*/
    t_float output = 0.;
    int curr, next;

    /*check if the table requested remains alive*/
    if (x->audio_tables[oscil->nt].size == 0) {
        return (0.);
    }

    /*put the oscillator index in range according its loop type*/
    switch (oscil->LT) {
    case NOLOOP: {
        while (oscil->index >= (double)x->audio_tables[oscil->nt].size)
            oscil->index -= x->audio_tables[oscil->nt].size;
        while (oscil->index < 0.)
            oscil->index += (double)x->audio_tables[oscil->nt].size;
        curr = (int)oscil->index;
        next = (int)oscil->index == x->audio_tables[oscil->nt].size - 1 ? 0 : (int)oscil->index + 1;
        break;
    }
    case LOOPF: {
        /*this checking is necessary as the user may overwrite the active table and change its
         * size*/
        if (oscil->fros >= x->audio_tables[oscil->nt].size || oscil->fros < 0 ||
            oscil->ends >= x->audio_tables[oscil->nt].size || oscil->ends < 0) {
            oscil->LT = NOLOOP;
            x->loop_type = NOLOOP;
            return (0.);
        }
        while (oscil->index >= (double)oscil->ends)
            oscil->index -= (double)oscil->durs;
        while (oscil->index < (double)oscil->fros)
            oscil->index += (double)oscil->durs;
        curr = (int)oscil->index;
        next = (int)oscil->index == oscil->ends - 1 ? oscil->fros : (int)oscil->index + 1;
        break;
    }
    case LOOPFB: {
        /*this checking is necessary as the user may have overwritten the active table and changed
         * its size*/
        if (oscil->fros >= x->audio_tables[oscil->nt].size || oscil->fros < 0 ||
            oscil->ends >= x->audio_tables[oscil->nt].size || oscil->ends < 0) {
            oscil->LT = NOLOOP;
            x->loop_type = NOLOOP;
            return (0.);
        }
        if (oscil->Ldir == 0) {
            if (oscil->index + oscil->incr >= oscil->ends - 1.)
                oscil->Ldir = 1;
        } else {
            if (oscil->index - oscil->incr <= oscil->fros)
                oscil->Ldir = 0;
        }
        while (oscil->index >= (double)oscil->ends)
            oscil->index -= (double)oscil->durs;
        while (oscil->index < (double)oscil->fros)
            oscil->index += (double)oscil->durs;
        curr = (int)oscil->index;
        next = (int)oscil->index == oscil->ends - 1 ? oscil->fros : (int)oscil->index + 1;
        break;
    }
    default: {
        break;
    }
    }
    /*get the right sample value by linear interpolation*/
    output = x->audio_tables[oscil->nt].table[curr].w_float +
             ((oscil->index - (double)curr) * (x->audio_tables[oscil->nt].table[next].w_float -
                                               x->audio_tables[oscil->nt].table[curr].w_float));

    if (oscil->Ldir)
        oscil->index -= oscil->incr;
    else
        oscil->index += oscil->incr;

    if (oscil->gliss != 1.) {
        oscil->incr += oscil->gliss;
    }
    return (amp * output);
}

// ─────────────────────────────────────
static t_float read_envelope_table(OSCIL *oscil, t_float amp, t_grainer_tilde *x) {
    /*this is a simple table-lookup oscillator*/
    t_float output = 0.;
    int curr, next;

    if (x->envelope_tables[oscil->nt].size == 0) {
        return (0.);
    }

    while (oscil->index >= (double)x->envelope_tables[oscil->nt].size)
        oscil->index -= (double)x->envelope_tables[oscil->nt].size;
    while (oscil->index < 0.)
        oscil->index += (double)x->envelope_tables[oscil->nt].size;
    curr = (int)oscil->index;
    next =
        (int)oscil->index == x->envelope_tables[oscil->nt].size - 1 ? curr : (int)oscil->index + 1;

    output = x->envelope_tables[oscil->nt].table[curr].w_float +
             ((oscil->index - (double)curr) * (x->envelope_tables[oscil->nt].table[next].w_float -
                                               x->envelope_tables[oscil->nt].table[curr].w_float));
    oscil->index += oscil->incr;

    return (amp * output);
}

// ─────────────────────────────────────
static t_float randomize_value(t_float base, t_float rdev) {
    t_float val;
    if (rdev <= 0.)
        return (base);
    val = base + BIRAND_FLOAT(rdev);
    val = val <= 0. ? base : val;
    return (val);
}

// ─────────────────────────────────────
static void compute_channel_gains(t_grainer_tilde *x, GRAIN *g) {
    /*auxiliar variables*/
    t_float cosazi, sinazi, cosele, sinele, sin2ele, coselepow2, azi2;
    t_float sinpow2t5, coselepow3, azi3, sinelepow2, sinpow2t5m1;

    /*compute angles and gains for each output channel according the output type*/
    if (x->nch != MONO) {

        /*compute grain azimuth angle*/
        g->azi = x->azgr + BIRAND_FLOAT(x->azgrnd);

        /*do we have stereo output?*/
        if (x->nch == IPAN) {
            g->azi = g->azi < DEG45 ? DEG45 : g->azi; /*keep in range*/
            g->azi = g->azi > DEG135 ? DEG135 : g->azi;
            g->azi -= DEG45;
            g->gch[0] = sin(g->azi); /*pre calculate gains for each channel*/
            g->gch[1] = cos(g->azi); /*according to intensity panning*/
            return;
        }

        /*If we are here, it is because we need the Ambisonics setup*/
        /*compute and check angles first*/
        /*keep in range for Ambisonic azimuth*/
        g->azi -= DEG90;
        while (g->azi < 0.)
            g->azi += PI2; /*wrap around azimuth angle if needed*/
        while (g->azi > PI2)
            g->azi -= PI2;
        /*compute grain elevation angle*/
        g->ele = x->elgr + BIRAND_FLOAT(x->elgrnd);
        while (g->ele < 0.)
            g->ele += PI2; /*wrap around elevation angle if needed*/
        while (g->ele > PI2)
            g->ele -= PI2;

        /*pre-compute the gains for each ambisonic first order B Format signal
        we use here the FuMa channel ordering with SN3D normalization factors for encoding
        See http://www.blueripplesound.com/b-format
        first, pre-compute cosine and sine of elevation and azimuth, to save operations
        */
        cosazi = cos(g->azi);
        sinazi = sin(g->azi);
        cosele = cos(g->ele);
        sinele = sin(g->ele);
        /*
        W	0	sqrt(1/2)
        X	1	cos(A)cos(E)
        Y	1	sin(A)cos(E)
        Z	1	sin(E)
        */
        g->gch[0] = ASCAL;             /*W signal*/
        g->gch[1] = (cosazi * cosele); /*X signal*/
        g->gch[2] = (sinazi * cosele); /*Y signal*/
        g->gch[3] = sinele;            /*Z signal*/
        if (x->nch == AMBI)
            return;

        /*Ditto, pre-compute and add the remaining gains for each ambisonic second order B Format
         * signal*/
        /*first, pre-compute some factors to save operations*/
        azi2 = g->azi * 2.;
        sin2ele = sin(g->ele * 2.);
        coselepow2 = cosele * cosele;
        sinelepow2 = sinele * sinele;
        /*
        R	2	(1/2)(3sin(E)sin(E)-1)
        S	2	cos(A)sin(2E)
        T	2	sin(A)sin(2E)
        U	2	cos(2A)cos(E)cos(E)
        V	2	sin(2A)cos(E)cos(E)
        */
        g->gch[4] = (3. * sinelepow2 - 1.) / 2.; /*R signal*/
        g->gch[5] = (cosazi * sin2ele);          /*S signal*/
        g->gch[6] = (sinazi * sin2ele);          /*T signal*/
        g->gch[7] = (cos(azi2) * coselepow2);    /*U signal*/
        g->gch[8] = (sin(azi2) * coselepow2);    /*V signal*/
        if (x->nch == AMBI2)
            return;

        /*Ditto, pre-compute and add the remaining gains for each ambisonic third order B Format
         * signal*/
        /*first, pre-compute some factors to save operations*/
        sinpow2t5 = sinelepow2 * 5.;
        sinpow2t5m1 = sinpow2t5 - 1.;
        coselepow3 = coselepow2 * 2.;
        azi3 = 3. * g->azi;

        /*
        K	3	(1/2)sin(E)(5sin(E)sin(E)-3)
        L	3	sqrt(135/256)cos(A)cos(E)(5sin(E)sin(E)-1)
        M	3	sqrt(135/256)sin(A)cos(E)(5sin(E)sin(E)-1)
        N	3	sqrt(27/4)cos(2A)sin(E)cos(E)cos(E)
        O	3	sqrt(27/4)sin(2A)sin(E)cos(E)cos(E)
        P	3	cos(3A)cos(E)cos(E)cos(E)
        Q	3	sin(3A)cos(E)cos(E)cos(E)
        */
        g->gch[9] = (0.5 * sinele * (sinpow2t5 - 3.));     /*K signal*/
        g->gch[10] = (ASCAL2 * (g->gch[1]) * sinpow2t5m1); /*L signal*/
        g->gch[11] = (ASCAL2 * (g->gch[2]) * sinpow2t5m1); /*M signal*/
        g->gch[12] = (ASCAL3 * (g->gch[7]) * sinele);      /*N signal*/
        g->gch[13] = (ASCAL3 * (g->gch[8]) * sinele);      /*O signal*/
        g->gch[14] = (cos(azi3) * coselepow3);             /*P signal*/
        g->gch[15] = (sin(azi3) * coselepow3);             /*Q signal*/
        return;
    }

    else {
        g->gch[0] = 1.; /*mono needs no angles*/
    }
    return;
}

// ─────────────────────────────────────
static GRAIN *create_grain(t_grainer_tilde *x) {
    t_float from = 0., end = 0.,
            tabledur = (t_float)x->audio_tables[x->active_audio_table].size / x->sr, gli = x->gli,
            glir = x->glir;
    t_float faux;
    /*the loop type must be a local variable of each grain oscillator,
    otherwise when changed, the oscillators of the current active grains
    this may produce a serious crash.*/
    int loop_type = x->loop_type, loop_direction = x->loop_direction, aux = 0, i;
    GRAIN *g;

    /*allocate memory for the pointer*/
    g = (GRAIN *)malloc(sizeof(GRAIN));

    /*compute grain size in samples*/
    if (!x->lsel[PSES].size) {

        faux = x->lsel[DURS].size ? randomize_value(select_list_value(x, DURS), x->sgrnd)
                                  : randomize_value(x->sgr, x->sgrnd);
        if (faux == 0.)
            faux = MIN_DUR;
        if (faux < 0.)
            faux *= -1.;
        g->size = floor(faux * (t_float)x->sr);
        /*compute phase offset for the grain table lookup from stgr*/
        if (x->lsel[PSTS].size) {
            from = randomize_value(select_list_value(x, PSTS), x->stgrnd);
            loop_type = NOLOOP; /*loop type not available in list selection mode*/
            loop_direction = 0;
        } else {
            from = (x->stgr + BIRAND_FLOAT(x->stgrnd));
        }
    } else {
        aux = (int)select_list_value(x, PSES);
        faux = randomize_value(x->lsel[PSES].data[aux + 1], x->sgrnd);
        if (faux == 0.)
            faux = MIN_DUR;
        if (faux < 0.)
            faux *= -1.;
        g->size = floor(faux * (t_float)x->sr);
        from = randomize_value(x->lsel[PSES].data[aux], x->stgrnd);
        loop_type = NOLOOP; /*overwrite the loop type variable here*/
        loop_direction = 0;
    }

    /*check here some possibly wrong conditions*/
    /*from can't be less than zero neither >= tabledur*/
    if (from < 0. || from >= tabledur)
        from = 0.;
    /*compute end for the grain table lookup from engr*/
    end = (x->engr + BIRAND_FLOAT(x->engrnd));
    /*end can't be <= 0. neither >=tabledur, neither <=from*/
    /*if any of there conditions are met, we set it to zero and set the loop
    flag to NO_LOOP*/
    if (end <= 0. || end > tabledur || end <= from) {
        end = tabledur;     /*not sure if this is really needed*/
        loop_type = NOLOOP; /*overwrite the loop type variable here*/
        loop_direction = 0;
    }

    /*compute increment to -possibly- change the frequency of the grain*/
    if (x->lsel[0].size) {
        g->incr = x->fgr * randomize_value(select_list_value(x, DEVS), x->fgrnd);
    } else {
        g->incr = randomize_value(x->fgr, x->fgrnd);
    }

    /*compute the increment for the envelope oscil so as it fits the grain duration*/
    g->incra = (t_float)x->envelope_tables[x->active_envelope_table].size / (t_float)g->size;

    /*compute grain amp*/
    g->amp = randomize_value(x->agr, x->agrnd);

    /*compute grain distance*/
    g->dist = randomize_value(x->digr, x->digrnd);
    g->dist = (g->dist < MIN_DIST ? MIN_DIST : g->dist);
    g->dist = (g->dist > MAX_DIST ? MAX_DIST : g->dist);
    g->distaten = pow(g->dist, x->dist_exp); /*apply distance exponent for amplitude attenuation*/

    /*compute delay parameters and allocate memory for its buffer*/
    g->delbuf = NULL; /*must initialize!*/
    if (x->dosend) {
        if (x->delflag) {
            g->delsi = (floor)((g->dist / x->sound_speed) * x->sr);
            g->delpo = 0;
            g->delbuf = (t_float *)malloc(g->delsi * sizeof(t_float));
            if (g->delbuf) {
                for (i = 0; i < g->delsi; i++) {
                    g->delbuf[i] = 0.;
                }
                g->delflag = TRUE;
            } else {
                post(" grainer~: could not allocate delay buffer. Delay suppressed");
                g->delflag = FALSE;
                x->delflag = FALSE; /*this is only because something REALLY bad may be happening*/
            }
        } else {
            g->delflag = 0;
        }
    }

    /*compute gain factors according to the angular panning technique*/
    compute_channel_gains(x, g);

    /*initialize the sample counter and the total samps used*/
    /*must increase it by delsize samples*/
    g->s_count = g->size;
    if (g->delflag) {
        g->s_count += g->delsi;
    }
    /*set the two needed oscilators*/
    g->gosc = create_oscillator(floor(from * x->sr), floor(end * x->sr) - 1, loop_type,
                                loop_direction, gli, glir, (double)g->incr, x, AUDIO_TABLE);
    g->aosc = create_oscillator(0, 0, NOLOOP, 0, 1., 0., (double)g->incra, x, ENVELOPE_TABLE);

    /*bang at the configured grain interval, if requested*/
    if (x->grain_count_interval) {
        if (!--x->grain_count) {
            outlet_bang(x->out_bang);
            x->grain_count = x->grain_count_interval;
        }
    }
    return (g);
}

// ─────────────────────────────────────
static void synthesize_grain(GRAIN *g, t_grainer_tilde *x) {

    if (--g->s_count) {
        if (g->delsi <= 0 || !g->delflag) { /*delay is OFF*/
            g->send = read_audio_table(g->gosc, read_envelope_table(g->aosc, g->amp, x), x);
            g->dry = g->send / g->distaten;
            return;
        } else { /*delay is ON*/
            if (g->s_count > g->delsi) {
                g->send = read_audio_table(g->gosc, read_envelope_table(g->aosc, g->amp, x),
                                           x); /*send signal with neither amp scaling nor delay*/
                if (++g->delpo >= g->delsi)
                    g->delpo = 0;
                g->dry = g->delbuf[g->delpo] /
                         g->distaten; /*dry signal with both distance scaling and delay*/
                g->delbuf[g->delpo] = g->send;
                return;
            } else {
                if (++g->delpo >= g->delsi)
                    g->delpo = 0;
                g->dry = g->delbuf[g->delpo] / g->distaten;
                g->delbuf[g->delpo] = 0.;
                return;
            }
        }
    }
    return;
}

// ─────────────────────────────────────
static void compute_gap(t_grainer_tilde *x) {
    t_float aux;

    aux = x->lsel[GAPS].size ? randomize_value(select_list_value(x, GAPS), x->gagrnd)
                             : randomize_value(x->gagr, x->gagrnd);

    if (aux == 0.)
        aux = MIN_GAP;
    if (aux < 0.)
        aux = -aux;

    x->gap_samps = floor(aux * (t_float)x->sr);

    return;
}

// ─────────────────────────────────────
static void initialize_grainer(t_grainer_tilde *x) {
    int i;

    x->g = 0;
    x->audio_table_count = 0;
    x->envelope_table_count = 0;
    x->active_audio_table = 0;
    x->active_envelope_table = 0;

    for (i = 0; i < MAX_AUDIO_TABLES; ++i) {
        x->audio_tables[i].size = 0;
        x->envelope_tables[i].size = 0;
    }

    for (i = 0; i < NLSELS; ++i) {
        x->lsel[i].size = 0;
        x->lsel[i].type = 0;
        x->lsel[i].cont = 0;
        // the lists for data values selection are initially of lenght MAX_TABLES (24 elements)
        // we'll further enlarge them if it is needed
        x->lsel[i].data = (t_float *)getbytes(MAX_TABLES * sizeof(t_float));
    }

    x->active = 0;
    x->pause = 0;
    x->sr = sys_getsr();
    x->post_ctrl = 1;

    x->sound_speed = SOUND_SPEED;
    x->max_dist = MAX_DIST;
    x->dist_exp = DIST_EXP;
    x->dosend = 1;

    /*set default values*/
    x->gagr = 0.05;
    x->gagrnd = 0.0; /*gap between grains and its random deviation*/
    x->agr = 1.;
    x->agrnd = 0.0; /*grain amplitude and its random deviation*/
    x->fgr = 1.;
    x->fgrnd = 0.0; /*grain frequency and its random deviation*/
    x->sgr = 0.1;
    x->sgrnd = 0.0; /*grain dur and its random deviation*/
    x->stgr = 0.0;
    x->stgrnd = 0.0; /*grain starting point on the table and its random deviation*/
    x->engr = 0.0;
    x->engrnd = 0.0; /*grain ending point on the table and its random deviation*/

    x->loop_type = NOLOOP; /*loop type*/
    x->loop_direction = 0; /*loop direction, aux variable*/
    x->gli = 1.0;          /*glissando parameters*/
    x->glir = 0.0;

    x->delflag = TRUE;

    if (x->nch < AMBI) {
        x->azgr = DEG2RAD(90.); /*azimut angle of the grains and its random deviation*/
        x->azgrnd = 0.0;
    } else {           /*Ambisonics angles convention are different*/
        x->azgr = 0.0; /*azimut angle of the grains and its random deviation*/
        x->azgrnd = 0.0;
        x->elgr = 0.0; /*elevation angle of the grains and its random deviation*/
        x->elgrnd = 0.0;
    }
    x->digr = 1.0; /*distance of the grains and its random deviation*/
    x->digrnd = 0.0;

    /*set the grain counter to zero*/
    x->grain_count = x->grain_count_interval = 0;
    /*compute the gap for the first grain*/
    compute_gap(x);
    x->gap_count = x->gap_samps;

    return;
}

// ─────────────────────────────────────
static inline void process_grains(GRAIN ***g, t_grainer_tilde *x) {
    int i, k;
    int new = FALSE;

    /*first, check if grainer is not paused*/
    if (x->pause == 0) {
        /*then, check if we need to create a new grain*/
        if (!x->gap_count--) {            /*we do!*/
            for (i = 0; i < x->ng; ++i) { /*try to find out an unused GRAIN pointer*/
                if (!(*g)[i]->s_count) {  /*gotcha*/
                    if ((*g)[i]->delflag)
                        free((*g)[i]->delbuf);
                    free((*g)[i]);             /*free it*/
                    (*g)[i] = create_grain(x); /*use its pointer to create new grain*/
                    new = TRUE;
                    break;
                }
            }
            /*if we did not found an unused GRAIN pointer, we need a new one*/
            if (!new) {
                x->ng++;                                                 /*increment grain counter*/
                (*g) = (GRAIN **)realloc((*g), sizeof(GRAIN *) * x->ng); /*realloc memory*/
                (*g)[x->ng - 1] = create_grain(x);                       /*a new grain has born!*/
            }
            /*compute the samples of a new gap and set gap counter*/
            compute_gap(x);
            x->gap_count = x->gap_samps;
        }
    }

    /*now, compute the active grains*/
    /*mute first the output variables*/
    for (k = 0; k < x->nch + 1; k++) {
        x->osig[k] = 0.;
    }
    /*compute the output from the sum of active grains and do spatial distribution*/
    for (i = 0; i < x->ng; i++) {
        if ((*g)[i]->s_count) { /*check if the grain is alive and decrement its sample counter*/
            synthesize_grain((*g)[i], x); /*synthesize the grain*/
            if (x->dosend) {
                x->osig[x->nch] +=
                    (*g)[i]->send; /*put the plain output in the reverb send, if requested*/
            }
            for (k = 0; k < x->nch;
                 k++) { /*distribute the grain signal over the available output channels*/
                x->osig[k] += (*g)[i]->dry * ((*g)[i]->gch[k]);
            }
        }
    }
    return;
}

// ─────────────────────────────────────
static void print_grainer_state(t_grainer_tilde *x) {
    char *str = NULL;
    int i;

    str = (char *)malloc(1024 * sizeof(char));

    post("\ngrainer~ actual parameters **********************************");

    if (x->active)
        post("  grainer is active \t\t\t");
    else
        post("  grainer is stopped \t\t\t");
    if (x->pause)
        post("  state: paused \t\t\t");
    else
        post("  state: running \t\t\t");

    post("  sampling rate \t\t\t%f", x->sr);

    post("  Allocated audio tables \t\t\t%d", x->audio_table_count);
    if (x->audio_table_count) {
        post("  Last active audio table \t\t\t%s",
             x->audio_tables[x->active_audio_table].name->s_name);
        for (i = 0; i < x->audio_table_count; i++) {
            if (x->audio_tables[i].size)
                post("  Audio table   \t\t\t#%d is %s (%d samples)", i,
                     x->audio_tables[i].name->s_name, x->audio_tables[i].size);
            else
                post("  Deleted Audio table   \t\t\t#%d is %s", i, x->audio_tables[i].name->s_name);
        }
    }
    post("  Allocated envelope tables \t\t\t%d", x->envelope_table_count);
    if (x->envelope_table_count) {
        post("  Last active envelope table \t\t\t%s",
             x->envelope_tables[x->active_envelope_table].name->s_name);

        for (i = 0; i < x->envelope_table_count; i++) {
            if (x->envelope_tables[i].size)
                post("  Envelope table   \t\t\t#%d is %s (%d samples)", i,
                     x->envelope_tables[i].name->s_name, x->envelope_tables[i].size);
            else
                post("  Deleted envelope table   \t\t\t#%d is %s", i,
                     x->envelope_tables[i].name->s_name);
        }
    }

    post("\n  grain size (segs.) \t\t\t%f", x->sgr);
    post("  size  rnd. dev. \t\t\t%f", x->sgrnd);
    /*
    if(x->durtsize) {
            charray_print(x->durtsize, x->durt, str);
            post("  Discrete values mode selection for grain size set");
            post("  Array size: \t\t\t%d \n  Array values:\t\t\t%s", x->durtsize, str);
    }

    post("\n  gap  size in segs. \t\t\t%f",   x->gagr);
    post("  gap size in samples \t\t\t%d",   x->gap_samps);
    post("  gap size rnd. dev. \t\t\t%f",   x->gagrnd);

    if(x->gaptsize) {
            charray_print(x->gaptsize, x->gapt, str);
            post("  Discrete values mode selection for gap times set");
            post("  Array size: \t\t\t%d \n  Array values:\t\t\t%s", x->gaptsize, str);
    }

    post("\n  table start \t\t\t%f",   x->stgr);
    post("  table start rnd. dev. \t\t\t%f",   x->stgrnd);
    if(x->ptrtsize) {
            charray_print(x->ptrtsize, x->ptrt, str);
            post("  Discrete values mode selection for grain starting times set");
            post("  Array size: \t\t\t%d \n  Array values:\t\t\t%s", x->ptrtsize, str);
    }
    */
    post("\n  table end \t\t\t%f", x->engr);
    post("  table end rnd. dev. \t\t\t%f", x->engrnd);
    switch (x->loop_type) {
    case NOLOOP:
        post("  Loop Type: \t\t\tNo Loop");
        break;
    case LOOPF:
        post("  Loop Type: \t\t\tForward Loop");
        break;
    case LOOPFB:
        post("  Loop Type: \t\t\tForward-Backward Loop");
        break;
    }

    post("\n  freq. scaler \t\t\t%f", x->fgr);
    post("  freq. rnd. dev. \t\t\t%f", x->fgrnd);
    /*
    if(x->devtsize) {
            charray_print(x->devtsize, x->devt, str);
            post("  Discrete values mode selection for frequency deviation set");
            post("  Array size: \t\t\t%d \n  Array values:\t\t\t%s", x->devtsize, str);
    }
    */
    post("\n  amp. scaler \t\t\t%f", x->agr);
    post("  amp. rnd. dev. \t\t\t%f", x->agrnd);

    if (x->nch == MONO) {
        post("\n  output type \t\t\tMono");
    }
    if (x->nch == IPAN) {
        post("\n  output type \t\t\tStereo IP");
    }
    if (x->nch == AMBI) {
        post("\n  output type \t\t\tAmbisonic first Order B Format");
    }

    post("  grain dist. \t\t\t%f", x->digr);
    post("  grain dist. rnd. dev. \t\t\t%f", x->digrnd);
    if (x->nch == IPAN || x->nch >= AMBI) {
        post("  azimuth center \t\t\t%f", RAD2DEG(x->azgr));
        post("  az. rnd. dev. \t\t\t%f", RAD2DEG(x->azgrnd));
        if (x->nch >= AMBI) {
            post("  elevation center \t\t\t%f", RAD2DEG(x->elgr));
            post("  el. rnd. dev. \t\t\t%f", RAD2DEG(x->elgrnd));
        }
    }

    if (str) {
        free(str);
    }

    post("  ***************************************************************");
}

// ─────────────────────────────────────
static void format_float_array(int size, t_float *data, char *str) {
    int i;
    char *num;

    num = (char *)malloc(32 * sizeof(char));
    *str = 0;
    for (i = 0; i < size; ++i) {
        *num = 0;
        sprintf(num, "%8.5f", fabs(data[i]));
        strcat(str, num);
    }
    free(num);
    return;
}

// ─────────────────────────────────────
static void reset_gap(t_grainer_tilde *x) /*added by Damián Anache, 06/01/2016*/
{
    x->gap_count = x->gap_samps;
    if (x->post_ctrl)
        post(" grainer~ gap counter is restarted");
}

// ─────────────────────────────────────
void setup_x0x2egrainer_tilde(void) {
    grainer_tilde_class = class_new(gensym("x.grainer~"), (t_newmethod)grainer_tilde_new,
                                    (t_method)grainer_tilde_free, sizeof(t_grainer_tilde),
                                    CLASS_DEFAULT, A_DEFFLOAT, 0);

    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_use_audio_table,
                    gensym("use_audio_table"), A_DEFFLOAT, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_use_envelope_table,
                    gensym("use_envelope_table"), A_DEFFLOAT, 0);

    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("start"), A_GIMME,
                    0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("stop"), A_GIMME,
                    0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("audio_table"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("envelope_table"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("grain_count"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("pause"), A_GIMME,
                    0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("seed"), A_GIMME,
                    0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("print"), A_GIMME,
                    0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("gap"), A_GIMME,
                    0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("gap_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("duration"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("duration_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("playback_rate"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("playback_rate_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("start_position"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("start_position_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("end_position"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("end_position_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("loop_type"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("glissando"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("glissando_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("amplitude"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("amplitude_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("azimuth"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("azimuth_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("elevation"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("elevation_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("distance"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("distance_random_deviation"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("pitch_list"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("gap_list"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("duration_list"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("position_list"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("position_duration_list"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("audio_table_list"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("reset_gap"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("verbose"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("sound_speed"),
                    A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message,
                    gensym("distance_exponent"), A_GIMME, 0);
    class_addmethod(grainer_tilde_class, (t_method)grainer_tilde_message, gensym("distance_delay"),
                    A_GIMME, 0);

    post("[x.grainer~] v1.2 March 2022 | Pablo Di Liscia with the collaboration of Damian "
         "Anache UNQ, Argentina");
}
