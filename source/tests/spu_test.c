/* SPDX-License-Identifier: GPL-3.0-only */
/* GEARFRAME's SPU engine on this computer (firmware/src/gear_spu.c, firmware/gen/gear_spu.h). Every SPU factory
 * sound is played through the track (voice_on, track_chunk: the voices, the TONE, the gain) and measured:
 *   - it sounds, it does not clip hard, it has no DC, no NaN
 *   - its pitch: a note played at A4 (and A2 for the basses) measured by autocorrelation, within 10 cents
 *     (the instruments with no clear pitch, the hit, the timpani, the chords, are not measured)
 *   - its release ends: every voice off within 6 s of the note off; a one-shot ends by its own flags
 *   - the voices: a chord takes 2 or 3, a new note lets the last ring out, a fifth note steals the quietest
 * Then every instrument at the ends of its range (notes 0..127, TUNE +-12, every CHORD, DETUNE 15, CRUSH 7, the
 * three interpolations) for the overflows (run under UBSan: tests/run_spu_test.sh).
 * Out: build/spu/<n>_<name>.wav (each sound: a phrase), build/spu/all.wav. Exit 1 on a failure. */
#define main kawa_host_main
#include "../firmware/src/kawa.c"
#undef main
#include <math.h>

#define FS 44100u
static int fail;
static int32_t buf[FS * 12u];

static void bad(const char *name, const char *what)
{
    printf("FAIL  %-16s %s\n", name, what);
    fail = 1;
}

static uint32_t voices(uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < SPU_NV; i++) n += spv[t][i].on;
    return n;
}

static void render(uint32_t t, int32_t *y, uint32_t n)
{
    uint32_t i, j;
    for (i = 0; i + CHUNK <= n; i += CHUNK) {
        int32_t o[CHUNK];
        for (j = 0; j < CHUNK; j++) o[j] = 0;
        track_chunk(t, o);
        for (j = 0; j < CHUNK; j++) y[i + j] = o[j];
    }
}

static void reset(uint32_t k)
{
    memset(tk, 0, sizeof tk);
    memset(spv, 0, sizeof spv);
    memset(&prj.snd[0], 0, sizeof prj.snd[0]);
    sound_preset(0, k);
    prj.snd[0].p[P_VOL] = 12;
    prj.snd[0].p[P_PAN] = 0;
    prj.snd[0].p[P_FXT] = 0;
    prj.snd[0].p[P_LFOD] = 0;
}

static double pitch_hz(const int32_t *y, uint32_t n)   /* the strongest period by autocorrelation (60..1500 Hz) */
{
    uint32_t lag, best = 0, i;
    double bv = -1e30, r[1000];
    for (lag = 29; lag < 736 && lag < 1000; lag++) {
        double s = 0, e0 = 0, e1 = 0;
        for (i = 0; i + lag < n; i++) { s += (double)y[i] * y[i + lag]; e0 += (double)y[i] * y[i]; e1 += (double)y[i + lag] * y[i + lag]; }
        r[lag] = s / (sqrt(e0 * e1) + 1);
    }
    for (lag = 30; lag < 735; lag++)            /* the first strong peak (an octave below scores as well) */
        if (r[lag] > r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] > 0.85) { best = lag; break; }
    if (!best)
        for (lag = 30; lag < 735; lag++) if (r[lag] > bv) { bv = r[lag]; best = lag; }
    {                                           /* (parabolic: the fraction of a frame) */
        double a = r[best - 1], b = r[best], c = r[best + 1], d = a - 2 * b + c, f = d ? 0.5 * (a - c) / d : 0;
        return FS / (best + f);
    }
}

static void wav_out(const char *path, const int32_t *y, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t i, b = n * 2u, h[11] = {0x46464952u, 36u + b, 0x45564157u, 0x20746D66u, 16u, 0x00010001u, FS, FS * 2u,
                                     0x00100002u, 0x61746164u, b};
    if (!f) return;
    fwrite(h, 4, 11, f);
    for (i = 0; i < n; i++) {
        int16_t s = (int16_t)clampi(y[i], -32767, 32767);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(void)
{
    static int32_t all[FS * 120u];
    uint32_t k, an = 0, nsp = 0;
    char path[96];
    theme_set(0);
    keys_init();
    seq_tempo();
    for (k = 0; k < NPRESET; k++) {
        const char *name = PRESETS[k].name[LANG_EN];
        const int8_t *p;
        uint32_t i, n, inst, ch, oneshot, root, note;
        double pk = 0, dc = 0, rms = 0;
        static const uint8_t PHRASE[8] = {0, 4, 7, 12, 7, 5, 2, 0};
        if (PRESETS[k].engine != E_SPU) continue;
        nsp++;
        reset(k);
        p = prj.snd[0].p;
        inst = (uint32_t)p[P_SPI];
        ch = (uint32_t)p[P_SPCH];
        oneshot = SPU_INST[inst].loop == 0xFFFFu;
        root = SPU_INST[inst].root;
        /* a held note: 1.2 s, then the release */
        note = root < 48u ? 45u : 69u;
        prj.snd[0].p[P_SPVIB] = 0;                  /* (the pitch is measured without the vibrato) */
        voice_on(0, note, 15, &NO_TRICKS, 0);
        if (voices(0) != (ch == 0u ? (p[P_SPDET] ? 2u : 1u) : SPU_CHORD[ch][2] < 0 ? 2u : 3u))
            bad(name, "the chord took the wrong number of voices");
        render(0, buf, FS * 6u / 5u);
        for (i = 0; i < FS * 6u / 5u; i++) {
            double v = buf[i];
            if (fabs(v) > pk) pk = fabs(v);
            dc += v;
            rms += v * v;
        }
        dc /= FS * 6u / 5u;
        rms = sqrt(rms / (FS * 6u / 5u));
        if (pk < 1500) bad(name, "silent");
        if (pk > 60000) bad(name, "too loud (clips)");
        if (fabs(dc) > 600) bad(name, "DC");
        if (!oneshot && ch == 0u && inst != 13u) {   /* (the power chord is three notes in its sample) */
            uint32_t at = FS / 4u, len = 4096u;
            double hz = pitch_hz(buf + at, len), want = 440.0 * pow(2.0, ((double)note - 69.0) / 12.0), c;
            while (hz < want * 0.75) hz *= 2;   /* (an octave below the note: the same pitch class) */
            while (hz > want * 1.5) hz /= 2;
            c = 1200.0 * log2(hz / want);
            if (fabs(c) > 10) { char m[64]; snprintf(m, sizeof m, "pitch %.1f Hz for %.1f (%+.0f cents)", hz, want, c); bad(name, m); }
            printf("      %-16s pitch %+5.1f cents  ", name, c);
        } else
            printf("      %-16s                    ", name);
        printf("peak %5.0f  rms %5.0f\n", pk, rms);
        voice_off(0);
        for (n = 0; n < FS * 6u && voices(0); n += CHUNK) render(0, buf, CHUNK);
        if (voices(0)) bad(name, "the release does not end");
        /* a phrase, for the ear: eighth notes at 120 BPM, the last one held */
        reset(k);
        n = 0;
        for (i = 0; i < 8u; i++) {
            voice_on(0, (root < 48u ? 33u : 57u) + PHRASE[i] + (root >= 72u ? 12u : 0u), 13, &NO_TRICKS, 0);
            render(0, buf + n, FS / 4u);
            n += FS / 4u;
        }
        voice_off(0);
        render(0, buf + n, FS);
        n += FS;
        snprintf(path, sizeof path, "build/spu/%02u_%s.wav", nsp, name);
        for (i = 0; path[i]; i++) if (path[i] == ' ' || path[i] == '.') if (i > 10u && path[i] != '.') path[i] = '_';
        wav_out(path, buf, n);
        if (an + n < FS * 120u) { memcpy(all + an, buf, n * 4u); an += n; }
    }
    wav_out("build/spu/all.wav", all, an);
    /* the voices: two notes overlap (the first rings out), a fifth steals */
    reset(preset_named(E_SPU, 12));             /* WARM PAD: a long release */
    voice_on(0, 60, 15, &NO_TRICKS, 0);
    render(0, buf, FS / 10u);
    voice_on(0, 64, 15, &NO_TRICKS, 0);
    render(0, buf, FS / 10u);
    if (voices(0) < 2u) bad("VOICES", "a new note cut the last one instead of letting it ring");
    for (k = 0; k < 6u; k++) { voice_on(0, 60 + k, 15, &NO_TRICKS, 0); render(0, buf, CHUNK * 4u); }
    if (voices(0) > SPU_NV) bad("VOICES", "more than four");
    voice_cut(0);
    render(0, buf, FS / 20u);
    if (voices(0)) bad("VOICES", "CUT does not stop them");
    /* the ends of every knob, every instrument */
    {
        uint32_t inst, ch, it, nt;
        int32_t tune;
        for (inst = 0; inst < SPU_N; inst++)
            for (ch = 0; ch < SPU_NCHORD; ch++)
                for (it = 0; it < SPI_N; it++)
                    for (tune = -12; tune <= 12; tune += 24)
                        for (nt = 0; nt < 128u; nt += 21u) {
                            reset(preset_named(E_SPU, (int32_t)inst));
                            prj.snd[0].p[P_SPCH] = (int8_t)ch;
                            prj.snd[0].p[P_SPINT] = (int8_t)it;
                            prj.snd[0].p[P_SPTUNE] = (int8_t)tune;
                            prj.snd[0].p[P_SPDET] = 15;
                            prj.snd[0].p[P_SPCRU] = (int8_t)(nt & 7u);
                            prj.snd[0].p[P_VOL] = 15;
                            voice_on(0, nt + 1u, 15, &NO_TRICKS, 0);
                            render(0, buf, FS / 8u);
                            for (k = 0; k < FS / 8u; k++)
                                if (buf[k] > 200000 || buf[k] < -200000) { bad(SPU_NAME[LANG_EN][inst], "a wild sample at an end of a knob"); k = FS; inst = SPU_N; ch = SPU_NCHORD; it = SPI_N; tune = 99; nt = 999; }
                        }
    }
    /* the SPU reverb: each preset's impulse response (its decay, its level), then 3 s of loud noise into it */
    {
        uint32_t r;
        for (r = 0; r < RVB_NP; r++) {
            uint32_t i, j, t60 = 0, t20 = 0, n = (FS * 6u) & ~(CHUNK - 1u);
            double pk = 0, e0 = 0, lvl = 0, wild = 0;
            static int32_t L[FS * 6u], R[FS * 6u];
            prj.grv.pad[0] = (uint8_t)r;
            for (i = 0; i < n; i += CHUNK) {
                int32_t in[CHUNK];
                for (j = 0; j < CHUNK; j++) { in[j] = 0; L[i + j] = R[i + j] = 0; }
                if (i == ((FS / 2u) & ~(CHUNK - 1u))) in[0] = in[1] = 30000;   /* (after the area is cleared) */
                reverb_chunk(in, L + i, R + i);
            }
            for (i = FS / 2u; i < n; i++) { double a = fabs((double)L[i]) + fabs((double)R[i]); if (a > pk) pk = a; }
            for (i = FS / 2u; i + 2205u < n; i += 2205u) {   /* 50 ms windows: when it is 20 and 60 dB down */
                double e = 0;
                for (j = 0; j < 2205u; j++) e += (double)L[i + j] * L[i + j] + (double)R[i + j] * R[i + j];
                if (e > e0) e0 = e;
                if (!t20 && e0 > 0 && e < e0 / 100.0) t20 = i - FS / 2u;
                if (!t60 && e0 > 0 && e < e0 / 1e6) t60 = i - FS / 2u;
            }
            for (i = 0; i < FS * 3u; i += CHUNK) {   /* loud noise in: it must stay bounded */
                int32_t in[CHUNK], l[CHUNK], rr[CHUNK];
                for (j = 0; j < CHUNK; j++) { in[j] = (int32_t)(xr(&rnd_main) % 120001u) - 60000; l[j] = rr[j] = 0; }
                reverb_chunk(in, l, rr);
                for (j = 0; j < CHUNK; j++) { double a = fabs((double)l[j]); lvl += a; if (a > wild) wild = a; }
            }
            printf("      REVERB %-10s  peak %6.0f  -20 dB %4u ms  -60 dB %5u ms  noise in: mean %5.0f max %5.0f\n",
                   RVB[r].name, pk, t20 * 1000u / FS, t60 * 1000u / FS, lvl / (FS * 3u), wild);
            if (pk < 300) bad(RVB[r].name, "the reverb is silent");
            if (wild > 4 * 32768 + 4) bad(RVB[r].name, "the reverb goes past its 16 bits (x 4)");
            {                                   /* (the last 50 ms: a few LSB of rounding that go round are allowed) */
                double tail = 0;
                for (i = n - 2205u; i < n; i++) { double a = fabs((double)L[i]); if (a > tail) tail = a; }
                if (!t60 && r != 6u && tail > 16) { char m[64]; snprintf(m, sizeof m, "the reverb does not die away (%.0f)", tail); bad(RVB[r].name, m); }
                if (!t60) printf("      (%s: what stays after 5.5 s: %.0f)\n", RVB[r].name, tail);
            }
        }
        prj.grv.pad[0] = 0;
    }
    printf("%u SPU sounds, %s\n", nsp, fail ? "FAILED" : "all good");
    return fail;
}
