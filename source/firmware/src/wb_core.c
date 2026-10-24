/* SPDX-License-Identifier: GPL-3.0-only */
/* Wonderblue-OS: the song (seven tracks, eight patterns of up to 64 rows, a chain, a chord progression, the sounds and
 * the master), the scale and chord maths, and what the audio interrupt and the main loop share.
 *
 * Tracks: BRK the break (16 slices), KIK, SNR, HAT (synthesised drums), BAS, KEY, SYN (synths: FM, analog or voice).
 * A cell: a note (slice + 1 on BRK, a MIDI note elsewhere, 255 OFF, 0 empty), a velocity 0..15 and an effect:
 * R retrigger n times in the row, G chance (n + 1) / 16, D delay n / 16 of a row, B backwards, P pitch n - 8. */
#define NT 7u
#define NPAT 8u
#define NROW 64u
#define CH 16u                                  /* control rate: envelopes, the sequencer, every 16 frames */
#define NOTE_OFF 255u
enum { T_BRK, T_KIK, T_SNR, T_HAT, T_BAS, T_KEY, T_SYN };
enum { TY_BREAK, TY_KICK, TY_SNARE, TY_HAT, TY_SYNTH };
static const uint8_t TRK_TYPE[NT] = {TY_BREAK, TY_KICK, TY_SNARE, TY_HAT, TY_SYNTH, TY_SYNTH, TY_SYNTH};
static const char *const TRK_ID[NT] = {"BRK", "KIK", "SNR", "HAT", "BAS", "KEY", "SYN"};
static const char *const TRK_FULL[NT] = {"BREAK", "KICK", "SNARE", "HATS", "BASS", "KEYS", "SYNTH 2"};
enum { FX_NONE, FX_R, FX_G, FX_D, FX_B, FX_P, FX_N };
static const char FX_CH[FX_N] = {'.', 'R', 'G', 'D', 'B', 'P'};
static const char *const FX_NAME[FX_N] = {"NONE", "RETRIG", "CHANCE", "DELAY", "REVERSE", "PITCH"};

typedef struct {
    uint8_t n, v, fx, fv;
} cell_t;
typedef struct {
    cell_t c[NT][NROW];
    uint8_t len, pad[3];
} pat_t;
/* a track's sound: what each index means depends on its type */
enum { PB_SRC, PB_PITCH, PB_GATE, PB_TONE };                                   /* break: source, semitones, gate %, tone 0..100 */
enum { PK_TUNE, PK_DECAY, PK_PUNCH, PK_DRIVE };                                /* kick */
enum { PS_TUNE, PS_DECAY, PS_TONE, PS_SNAP, PS_CLAP };                         /* snare */
enum { PH_TUNE, PH_DECAY, PH_TONE, PH_METAL };                                 /* hats */
enum { PY_ENG, PY_RATIO, PY_INDEX, PY_ATT, PY_DEC, PY_SUS, PY_REL, PY_DET, PY_TINE, PY_WAVE, PY_UNI, PY_RESO, PY_FENV,
       PY_GLIDE, PY_VIB, PY_VOWEL, PY_CHORD, PY_OCT, PY_CTR, PY_CTRP, PY_CTRL, NP };   /* synths (the first 16: WB_SYN) */
typedef struct {
    int16_t p[NP];
    uint8_t patch, vol, rev, dly;               /* patch 255: the genre's own */
    int8_t pan;
    uint8_t mute, solo, chance, mutate, drift, density, pad;
} track_t;
#define SONG_MAGIC 0x45554C42u                  /* "BLUE" */
#define SONG_VERSION 1u
enum { RL_BRK, RL_BAS, RL_KEY, RL_SYN, RL_BRK2, RL_BAS2, RL_KEY2, RL_SYN2, RL_K, RL_S, RL_H, RL_K2, RL_S2, RL_H2, RL_N };
typedef struct {
    uint32_t magic, version;
    char name[16];
    uint8_t bpm, swing, key, scale, age, crush, space, pump, mvol, genre, cur, nchain;
    uint8_t chain[16];
    int8_t deg[8];
    uint8_t ndeg, rows, prog, loop;             /* the progression: degrees, rows a chord, which preset (WB_NPROG - 1: custom); loop: the pattern, not the chain */
    uint8_t roll[RL_N], pad1[2];                /* what the genre's roll decided: part styles, drum lanes */
    track_t trk[NT];
    pat_t pat[NPAT];
    uint32_t crc;
} song_t;

#if defined(DINGHY_HOST) || defined(KAWA_WASM)
#define POOL
#else
#define POOL __attribute__((section(".pool")))
#endif
static song_t song POOL;

/* ---- random numbers: one for the interrupt, one for the main loop */
static uint32_t rnd_isr = 0x9E3779B9u, rnd_main = 0x1234567u;
static uint32_t xr(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static uint32_t rnd(uint32_t n) { return n ? (xr(&rnd_main) >> 8) % n : 0u; }
static int chance_pct(uint32_t p) { return rnd(100) < p; }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- the scale: degree d (0 = the key's root) <-> MIDI note */
static int32_t floordiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int32_t deg_midi_s(int32_t d, uint32_t sc)
{
    int32_t n = WB_SCALE_N[sc % WB_NSCALE], o = floordiv(d, n);
    return (int32_t)song.key + 12 * o + WB_SCALE[sc % WB_NSCALE][d - o * n];
}
static int32_t deg_at_s(int32_t m, uint32_t sc)
{
    int32_t n = WB_SCALE_N[sc % WB_NSCALE], rel = m - (int32_t)song.key, o = floordiv(rel, 12), pc = rel - o * 12, i, idx = 0;
    for (i = 0; i < n; i++)
        if (WB_SCALE[sc % WB_NSCALE][i] <= pc) idx = i;
    return o * n + idx;
}
static int32_t deg_midi(int32_t d) { return deg_midi_s(d, song.scale); }
static int32_t deg_at(int32_t m) { return deg_at_s(m, song.scale); }
static int32_t fold_oct(int32_t n)              /* into range by octaves: it stays in the scale */
{
    while (n > 96) n -= 12;
    while (n < 28) n += 12;
    return n;
}
static int32_t prog_deg_of(const int8_t *deg, uint32_t n, uint32_t rows, uint32_t r) { return deg[(r / (rows ? rows : 16u)) % (n ? n : 1u)]; }
static int32_t prog_deg(uint32_t r) { return prog_deg_of(song.deg, song.ndeg, song.rows, r); }
static uint32_t chord_notes(int32_t m, uint32_t chord, int32_t *out)   /* stacked thirds of the scale over m */
{
    uint32_t k, n = WB_CHORD_N[chord % WB_NCHORD];
    int32_t d = deg_at(m), sh = m - deg_midi(d);
    if (n == 1u) { out[0] = m; return 1; }
    for (k = 0; k < n; k++) out[k] = deg_midi(d + WB_CHORD[chord % WB_NCHORD][k]) + sh;
    return n;
}
/* a chord's name on a degree: "Bm", "F#", "Cdim"; its numeral: "vi", "IV", "vii*" */
static const char *const NOTE_NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void chord_name(int32_t d, char *s, char *rom)
{
    static const char *const R[8] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII"};
    int32_t r = deg_midi(d), third = deg_midi(d + 2) - r, fifth = deg_midi(d + 4) - r, n = WB_SCALE_N[song.scale % WB_NSCALE];
    const char *q = third == 4 && fifth == 7 ? "" : third == 3 && fifth == 7 ? "m" : third == 3 && fifth == 6 ? "dim" : third == 4 && fifth == 8 ? "+" : "5";
    const char *a = NOTE_NM[((r % 12) + 12) % 12], *rr = R[((d % n) + n) % n];
    uint32_t i = 0;
    while (*a) s[i++] = *a++;
    while (*q) s[i++] = *q++;
    s[i] = 0;
    if (rom) {
        i = 0;
        while (*rr) { rom[i++] = (char)(third == 3 ? *rr + ('a' - 'A') : *rr); rr++; }
        if (third == 3 && fifth == 6) rom[i++] = '*';
        rom[i] = 0;
    }
}
static void note_name(uint32_t m, char *s)      /* "C-4", "F#2" */
{
    static const char *const NN[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    int32_t o = (int32_t)m / 12 - 1;
    s[0] = NN[m % 12u][0];
    s[1] = NN[m % 12u][1];
    s[2] = (char)(o < 0 ? '-' : '0' + (o > 9 ? 9 : o));
    s[3] = 0;
}

/* ---- shared with the audio interrupt */
static volatile uint8_t sq_play, sq_req;        /* main -> ISR: 1 start, 2 stop */
static volatile uint8_t sq_pi, sq_row, sq_cue = 0xFF, sq_chainpos;
static volatile uint32_t sq_rowtick;            /* ISR: rows played (the screen follows) */
static volatile uint8_t sq_end_pi = 0xFF;       /* ISR -> main: this pattern just ended (mutation, SIT) */
static volatile uint32_t trig_ms[NT];           /* ISR: when each track last played (the headers flash) */
static volatile uint16_t pad_hit;               /* ISR: pads (white keys) of the selected track that just played */
static volatile uint8_t sel_trk;                /* main: the selected track (its pads light) */
static volatile int8_t pad_oct[NT];
static volatile uint8_t punch_loop, punch_rev, punch_half, punch_scat, punch_dub, punch_wash, punch_lpf, punch_roll;
static volatile uint8_t sit_mute;               /* SIT: the tracks a section leaves out (bit per track) */
static volatile uint8_t rpt_held;               /* note repeat: a pad held with ARP (bit 7: on), its note in rpt_note */
static volatile uint8_t rpt_note, rpt_trk;
static volatile uint8_t rec_on;
static volatile uint32_t audio_us, audio_peak;
static int16_t scope[120];
static volatile uint32_t scope_w;

/* main -> ISR: notes from the pads (a small queue) */
typedef struct {
    uint8_t type, trk, note, vel;
} ev_t;
enum { EV_ON = 1, EV_OFF, EV_ALLOFF, EV_CHORD };
#define EVQ 32u
static ev_t evq[EVQ];
static volatile uint32_t evq_w, evq_r;
static void ev_push(uint32_t type, uint32_t trk, uint32_t note, uint32_t vel)
{
    ev_t *e;
    if (evq_w - evq_r >= EVQ) return;
    e = &evq[evq_w % EVQ];
    e->type = (uint8_t)type;
    e->trk = (uint8_t)trk;
    e->note = (uint8_t)note;
    e->vel = (uint8_t)vel;
    RING_PUBLISH();
    evq_w++;
}

/* ---- what the pads play on each track: slices, drum tunes, the scale's notes over two octaves */
static uint32_t pad_note(uint32_t t, uint32_t i)
{
    static const uint8_t BASE[NT] = {0, 0, 0, 0, 36, 55, 55};
    switch (TRK_TYPE[t]) {
    case TY_BREAK: return i;
    case TY_KICK: case TY_SNARE: return 52u + i;
    case TY_HAT: return i < 12u ? 52u + i : 64u + (i - 12u);
    default: return (uint32_t)clampi(deg_midi(deg_at((int32_t)BASE[t] + song.key) + pad_oct[t] * WB_SCALE_N[song.scale % WB_NSCALE] + (int32_t)i), 12, 120);
    }
}
