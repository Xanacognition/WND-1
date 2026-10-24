/* SPDX-License-Identifier: GPL-3.0-only */
/* GEARFRAME's style generator on this computer (firmware/src/gear_style.c): every style writes a piece (gn_make, as
 * OCT+ twice on the STYLE page), which then plays through the audio interrupt (app_audio) for 24 s:
 *   - every sound a style names is a factory sound (none falls back to the first by mistake)
 *   - the piece: eight patterns with notes, a chain of ten, the tempo in the style's range, the reverb's room set
 *   - the mix: each track's level (the mean of its block peaks while it plays), the master's peak, and how much of
 *     the time the limiter holds it down (a style whose mix is pinned to the limiter fails)
 * Out: build/style/<n>_<name>.wav (24 s each, stereo). Exit 1 on a failure. */
#define main kawa_host_main
#include "../firmware/src/kawa.c"
#undef main
#include <math.h>

#define BLK 256u
#define SECS 24u
static int32_t out[2u * BLK];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL  " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void block(void)
{
    uint32_t i;
    for (i = 0; i < 2u * BLK; i++) out[i] = 0;
    app_audio(out, BLK);
}

static int name_ok(const char *en)              /* a factory sound by exactly this name */
{
    uint32_t k = gn_preset_en(en);
    const char *a = PRESETS[k].name[LANG_EN];
    while (*a && *a == *en) { a++; en++; }
    return !*a && !*en && PRESETS[k].engine == E_SPU;   /* (and an SPU one: a name the FM shares found the FM once) */
}

int main(void)
{
    uint32_t g, i, t;
    static int16_t wav[2u * 44100u * SECS];
    theme_set(0);
    keys_init();
    prj_new();
    for (g = 0; g < NGENRE; g++) {
        const genre_t *s = &GENRES[g];
        double lv[NTRK] = {0}, mpk = 0, held = 0;
        uint32_t nb = 0, notes = 0, p, r, w = 0, lim = 0;
        char path[96];
        FILE *f;
        CHECK(name_ok(s->bass_snd) && name_ok(s->key_snd) && name_ok(s->key_snd2) && name_ok(s->syn_snd) && name_ok(s->t1_snd),
              "%s: a sound that is not an SPU factory sound", s->name);
        sq_req = 2;
        block();
        gn_genre = (uint8_t)g;
        gn_prog = 0;
        gn_energy = 8;
        fm1_ms += 7919u * (g + 1u);             /* (each piece its own dice) */
        gn_make();
        ev_push(EV_KILL, NTRK, 0, 0, 0, 0);
        for (p = 0; p < 8u; p++)
            for (r = 0; r < NROW; r++)
                for (t = 0; t < NTRK; t++)
                    notes += prj.pat[p].c[r][t].note != 0;
        CHECK(notes > 60u, "%s: only %u notes", s->name, notes);
        CHECK(prj.song_len == 10u && prj.mode == 1u, "%s: the chain", s->name);
        CHECK(prj.bpm >= s->bpm_lo && prj.bpm <= s->bpm_hi, "%s: tempo %u", s->name, prj.bpm);
        CHECK(prj.grv.pad[0] == s->rvb, "%s: the reverb's room", s->name);
        sq_start_pat = 0;
        sq_start_pos = 0;
        sq_req = 1;
        for (i = 0; i < 44100u * SECS / BLK; i++) {
            uint32_t k;
            block();
            for (t = 0; t < NTRK; t++) lv[t] += level[t];
            nb++;
            if (lim_g < 26000) held++;           /* (the limiter more than 2 dB down) */
            for (k = 0; k < 2u * BLK; k++) {
                double a = fabs((double)out[k]);
                if (a > mpk) mpk = a;
                if (w < 2u * 44100u * SECS) wav[w++] = (int16_t)clampi(out[k], -32767, 32767);
                if (a >= 32700) lim++;
            }
        }
        printf("%-15s %3u BPM %-10s", s->name, prj.bpm, RVB[s->rvb].name);
        for (t = 0; t < NTRK; t++) printf(" %5.0f", lv[t] / nb);
        printf("  peak %5.0f  limited %4.1f %%\n", mpk, 100.0 * held / nb);
        CHECK(held / nb < 0.5, "%s: the limiter holds the mix down %.0f %% of the time", s->name, 100.0 * held / nb);
        snprintf(path, sizeof path, "build/style/%02u_%s.wav", g + 1u, s->name);
        for (i = 12u; path[i]; i++) if (path[i] == ' ' || path[i] == '&' || path[i] == '.') path[i] = '_';
        path[str_len(path) - 4u] = '.';
        f = fopen(path, "wb");
        if (f) {
            uint32_t b = w * 2u, h[11] = {0x46464952u, 36u + b, 0x45564157u, 0x20746D66u, 16u, 0x00020001u, 44100u,
                                          44100u * 4u, 0x00100004u, 0x61746164u, b};
            fwrite(h, 4, 11, f);
            fwrite(wav, 2, w, f);
            fclose(f);
        }
    }
    printf("(the tracks: T1 BRK/ORCH, T2 DRM, T3 HAT, T4 BAS, T5 KEY, T6 SYN; mean block peaks)\n");
    printf("style_test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
