/* SPDX-License-Identifier: GPL-3.0-only */
/* GEARFRAME: the SPU engine. A PlayStation's sound chip played the music of its games from ADPCM samples in its
 * 512 KiB of RAM, 24 voices, each with its pitch counter, a 4-point Gaussian interpolation and an ADSR. This is
 * that voice, four to a track (6 x 4: the SPU's 24):
 *   - the instruments are SPU ADPCM (gear_spu.h, tools/gen_spu.py): 16-byte blocks of 28 samples, five
 *     prediction filters; the blocks' flags loop the sound (loop start, end + repeat) or end it (end + mute)
 *   - the pitch counter is the SPU's, in Q16 here (the SPU's is Q12): bits 8..15 index the Gaussian table
 *     (SPU_GAUSS, the chip's own), and it never steps more than 4 samples a frame (the SPU's 4000h)
 *   - each voice has its own ADSR (the track's ENV page): a new note lets the last one ring out in its release,
 *     as a game's sequencer did, and the reverb (the SPU's, kawa_fx.c) takes the tails
 *   - CHORD plays two or three voices from one note (pads, stabs, power chords); DETUNE spreads them (alone,
 *     it doubles the note: an ensemble). The ARP trick of a cell plays its chord at once on an SPU track (up to
 *     four notes: a block chord, as a game's sequencer wrote them), not one note after the other
 * Runs in the audio interrupt: integers only, no division per frame. */
#include "gear_spu.h"

#define SPU_NV 4u
enum { P_SPI = 0, P_SPCH, P_SPTUNE, P_SPDET, P_SPINT, P_SPVIB, P_SPCRU, P_SPTON };
enum { SPI_GAUSS, SPI_LINEAR, SPI_RAW, SPI_N };
#define SPU_NCHORD 10u
static const int8_t SPU_CHORD[SPU_NCHORD][3] = {   /* the notes of CHORD over the played one (-1: none) */
    {0, -1, -1}, {0, 12, -1}, {0, 7, -1}, {0, 7, 12}, {0, 4, 7}, {0, 3, 7}, {0, 5, 7}, {0, 4, 11}, {0, 3, 10}, {0, 7, 14}};

typedef struct {
    uint8_t on, follow, stage, inst, flags;    /* follow: the track's note (else the pitch it had when let go) */
    uint16_t addr, loop;                       /* the block playing, the loop's (from the flags: 0xFFFF none yet) */
    uint32_t pos;                              /* Q16 samples into the block, 0 .. 28 << 16 */
    int16_t s[31];                             /* the last 3 samples of the block before, then this one's 28 */
    int16_t old, older;                        /* the ADPCM decoder's history */
    int32_t off;                               /* Q8 semitones: over the track's pitch (follow), or the pitch */
    int32_t env;                               /* the voice's ADSR, Q16 */
    int32_t g;                                 /* its gain at the end of the last chunk, Q15 */
    int32_t mix;                               /* its share of the track (a chord's notes are softer), Q15 */
    uint32_t age;
} spuv_t;
static spuv_t spv[NTRK][SPU_NV];
static uint32_t spu_age;
static int32_t spu_pit[NTRK];                  /* the track's pitch at the last chunk (Q8): the voices let go keep it */
static volatile uint16_t spu_addr[NTRK];        /* ISR: the newest voice's block (the SOUND page's playhead), 0xFFFF none */

static void spu_decode(spuv_t *v)              /* the block at v->addr into s[3..30] (the SPU's decoder) */
{
    static const int32_t K0[5] = {0, 60, 115, 98, 122}, K1[5] = {0, 0, -52, -55, -60};
    const spu_inst_t *in = &SPU_INST[v->inst];
    const uint8_t *b = SPU_DATA + in->off + (uint32_t)v->addr * 16u;
    uint32_t sh = b[0] & 15u, f = (b[0] >> 4) & 7u, i;
    int32_t k0, k1, old = v->old, older = v->older;
    if (sh > 12u) sh = 9u;                      /* (as the chip: 13..15 act as 9) */
    if (f > 4u) f = 4u;
    k0 = K0[f];
    k1 = K1[f];
    v->flags = b[1];
    if (b[1] & 4u) v->loop = v->addr;           /* loop start */
    for (i = 0; i < 28u; i++) {
        int32_t n = (b[2u + (i >> 1)] >> ((i & 1u) * 4u)) & 15, d;
        n = (n ^ 8) - 8;                        /* a signed nibble */
        d = ((n * 4096) >> sh) + ((old * k0 + older * k1 + 32) >> 6);
        d = d > 32767 ? 32767 : d < -32768 ? -32768 : d;
        older = old;
        old = d;
        v->s[3u + i] = (int16_t)d;
    }
    v->old = (int16_t)old;
    v->older = (int16_t)older;
}

static int spu_next(spuv_t *v)                 /* the block played to its end: the flags say where to go (0: the end) */
{
    const spu_inst_t *in = &SPU_INST[v->inst];
    v->s[0] = v->s[28];
    v->s[1] = v->s[29];
    v->s[2] = v->s[30];
    if (v->flags & 1u) {                        /* loop end: repeat from the loop, or end + mute */
        if (!(v->flags & 2u) || v->loop == 0xFFFFu) return 0;
        v->addr = v->loop;
    } else if ((uint32_t)v->addr + 1u < in->nblk)
        v->addr++;
    else
        return 0;
    spu_decode(v);
    return 1;
}

static void spu_release(uint32_t t)            /* the track's note is let go: its voices into their release */
{
    uint32_t i;
    for (i = 0; i < SPU_NV; i++) {
        spuv_t *v = &spv[t][i];
        if (!v->on || !v->follow) continue;
        v->follow = 0;
        v->off += spu_pit[t];                   /* (from here it keeps its own pitch) */
        if (v->stage && v->stage < 4u) v->stage = 4;
    }
}

static void spu_cut(uint32_t t)                /* CUT: a fast release (~6 ms) */
{
    uint32_t i;
    spu_release(t);
    for (i = 0; i < SPU_NV; i++)
        if (spv[t][i].on) spv[t][i].stage = 5;
}

static void spu_kill(uint32_t t)
{
    uint32_t i;
    for (i = 0; i < SPU_NV; i++)
        spv[t][i].on = 0;
    spu_addr[t] = 0xFFFFu;
}

static spuv_t *spu_voice(uint32_t t)           /* a free voice, or the quietest of those ringing out */
{
    spuv_t *best = 0;
    uint32_t i;
    for (i = 0; i < SPU_NV; i++) {
        spuv_t *v = &spv[t][i];
        if (!v->on) return v;
        if (v->follow) continue;
        if (!best || v->env < best->env || (v->env == best->env && v->age < best->age)) best = v;
    }
    return best ? best : &spv[t][0];
}

/* a note: its voices (the pitch is the track's: track_chunk passes it each chunk). arp: a cell's ARP chord (CHORDS),
 * played at once, or -1: the sound's CHORD */
static void spu_on(uint32_t t, int32_t arp)
{
    const int8_t *p = prj.snd[t].p;
    const int8_t *ch = SPU_CHORD[(uint32_t)clampi(p[P_SPCH], 0, (int32_t)SPU_NCHORD - 1)];
    uint32_t n = 0, i, inst = (uint32_t)clampi(p[P_SPI], 0, (int32_t)SPU_N - 1);
    int32_t det = clampi(p[P_SPDET], 0, 15) * 3, offs[4];
    static const int32_t MIX[5] = {32767, 32767, 24000, 19500, 17000};
    spu_release(t);
    if (arp >= 0 && arp < 14) {
        for (i = 0; i < (uint32_t)CHORDS[arp].n && i < 4u; i++) offs[n++] = CHORDS[arp].off[i] * 256;
    } else
        for (i = 0; i < 3u; i++)
            if (ch[i] >= 0) offs[n++] = ch[i] * 256;
    if (n == 1u && det) {                       /* DETUNE alone: the note twice, apart */
        offs[1] = det;
        offs[0] = -det;
        n = 2;
    } else
        for (i = 1; i < n; i++)
            offs[i] += (i & 1u) ? det : -det;
    for (i = 0; i < n; i++) {
        spuv_t *v = spu_voice(t);
        uint32_t k;
        v->on = 1;
        v->follow = 1;
        v->inst = (uint8_t)inst;
        v->addr = 0;
        v->loop = 0xFFFFu;
        v->pos = 0;
        v->old = v->older = 0;
        for (k = 0; k < 3u; k++) v->s[k] = 0;
        spu_decode(v);
        v->off = offs[i];
        v->stage = 1;
        v->env = 0;
        if (p[P_ATT] == 0) v->env = 65535, v->stage = 2;
        v->g = 0;
        v->mix = MIX[n];
        v->age = ++spu_age;
    }
}

static uint32_t spu_step(const spu_inst_t *in, int32_t pit)   /* Q16 samples a frame for a pitch (Q8 semitones) */
{
    int32_t d = pit - (int32_t)in->root * 256, k = d >> 8;
    uint32_t r;
    if (k < -36) return (uint32_t)(((uint64_t)in->step * semi_ratio(-36)) >> 16);
    r = semi_ratio(k > 36 ? 36 : k);
    r = (uint32_t)(((uint64_t)r * FINE[((uint32_t)d & 255u) >> 2]) >> 30);
    r = (uint32_t)(((uint64_t)in->step * r) >> 16);
    return r > (4u << 16) ? 4u << 16 : r;       /* the SPU's ceiling: 4 samples a frame */
}

static int spu_env(spuv_t *v, const int8_t *p)  /* the voice's ADSR, once a chunk (0: it is done) */
{
    int32_t tgt;
    switch (v->stage) {
    case 1:
        v->env += ENV_RATE[p[P_ATT] & 15];
        if (v->env >= 65535) { v->env = 65535; v->stage = 2; }
        break;
    case 2:
        tgt = p[P_SUS] * 4369;
        v->env -= ENV_RATE[p[P_DEC] & 15] >> 1;
        if (v->env <= tgt) { v->env = tgt; v->stage = 3; }
        break;
    case 4:
        v->env -= ENV_RATE[p[P_REL] & 15] >> 1;
        if (v->env <= 0) return 0;
        break;
    case 5:
        v->env -= 6000;
        if (v->env <= 0) return 0;
        break;
    }
    return v->env > 0 || v->stage == 1;
}

/* every voice of track t for one chunk, added into raw; pit: the track's pitch now (Q8). 0: none sounds */
static int spu_chunk(uint32_t t, int32_t pit, int32_t *raw)
{
    const int8_t *p = prj.snd[t].p;
    uint32_t i, j, mode = (uint32_t)clampi(p[P_SPINT], 0, SPI_N - 1), newest = 0, any = 0;
    int32_t m = crush_mask(tk[t].crush);
    pit += p[P_SPTUNE] * 256;
    spu_pit[t] = pit;
    if (cpu_shed >= 2u && mode == SPI_GAUSS) mode = SPI_LINEAR;   /* (the load governor: kawa_seq.c) */
    for (j = 0; j < SPU_NV; j++) {
        spuv_t *v = &spv[t][j];
        const spu_inst_t *in;
        uint32_t step, pos;
        int32_t g0, g1, dg, g, e;
        if (!v->on) continue;
        if (cpu_shed >= 3u && !v->follow) { v->on = 0; continue; }
        if (!spu_env(v, p)) { v->on = 0; continue; }
        in = &SPU_INST[v->inst];
        step = spu_step(in, v->follow ? pit + v->off : v->off);
        e = (int32_t)(((uint32_t)v->env * (uint32_t)v->env) >> 17);   /* Q15, the same curve as the tracks' */
        g0 = v->g;
        g1 = (int32_t)(((int64_t)e * v->mix) >> 15);
        dg = (g1 - g0) / (int32_t)CHUNK;
        g = g0;
        pos = v->pos;
        for (i = 0; i < CHUNK; i++) {
            uint32_t k = pos >> 16;
            const int16_t *s = v->s + k;
            int32_t y;
            if (mode == SPI_GAUSS) {
                uint32_t gi = (pos >> 8) & 255u;
                y = ((SPU_GAUSS[255u - gi] * s[0]) >> 15) + ((SPU_GAUSS[511u - gi] * s[1]) >> 15) +
                    ((SPU_GAUSS[256u + gi] * s[2]) >> 15) + ((SPU_GAUSS[gi] * s[3]) >> 15);
            } else if (mode == SPI_LINEAR)
                y = s[2] + (((s[3] - s[2]) * (int32_t)((pos >> 1) & 0x7FFFu)) >> 15);
            else
                y = s[3];
            raw[i] += ((y & m) * g) >> 15;
            g += dg;
            pos += step;
            if (pos >= (28u << 16)) {
                pos -= 28u << 16;
                if (!spu_next(v)) { v->on = 0; break; }
            }
        }
        v->pos = pos;
        v->g = g1;
        if (v->on) {
            any = 1;
            if (v->follow && (!newest || v->age > spv[t][newest - 1u].age)) newest = j + 1u;
        }
    }
    spu_addr[t] = newest ? spv[t][newest - 1u].addr : 0xFFFFu;
    return (int)any;
}
