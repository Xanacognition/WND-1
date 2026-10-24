/* SPDX-License-Identifier: GPL-3.0-only */
/* GEARFRAME: the style generator (Wonderblue-OS's genre generator, turned to the music of the PlayStation's games).
 * STYLE (SCL, after MAGIC): KNOB 1 a style, KNOB 2 a chord progression (the style's own, or a standard one), KNOB 3
 * the energy; OCT+ twice writes a whole piece into patterns 1-8, chains them and sets the tempo, swing, key, scale,
 * the six tracks' sounds and the SPU reverb's room. Each press rolls a new piece (tempo, key, lanes, the parts'
 * styles and the sounds come from the style's pools).
 *
 *   T1 BRK  a break in 16 slices, or the orchestra (timpani,   T2 DRM  a kit: kicks, snares, claps
 *           an orchestra hit) on the chords' roots              T3 HAT  the same kit's hats
 *   T4 BAS  an SPU bass (slap, fretless, synth)                 T5 KEY  SPU keys: block chords or a melody
 *   T6 SYN  SPU: the keys' partner (strings, choir, flute...)
 *
 * The eight patterns are a piece's sections: A main, B variation, C breakdown, D build, E the other groove, F half-time,
 * G chopped, H outro; the chain plays A B A E C F G D A H. Two bars a pattern, one chord a bar. On an SPU track the
 * ARP trick plays its chord at once (gear_spu.c): the chords here are a sequencer's, not a chip's arpeggios. */

enum { WT_BRK, WT_DRM, WT_HAT, WT_BAS, WT_KEY, WT_SYN };
enum { GB_OFF, GB_STRAIGHT, GB_CHOP, GB_STORM, GB_SPARSE, GB_HALF, GB_ORCH };
enum { GL_ROOTS, GL_WALK, GL_SUB, GL_OFFBEAT, GL_EIGHTHS, GL_ACID };
enum { GK_MOTIF, GK_ARP, GK_LEAD, GK_PADS, GK_STABS, GK_JAZZ };   /* melodies first, then chords */
#define GK_IS_CHORD(m) ((m) >= GK_PADS)

/* drum lanes, 16 steps: x hit, g ghost, o open hat */
enum { LN_NONE, LN_FOUR, LN_BOOM, LN_BOOM2, LN_BOOM3, LN_TRIP, LN_HALF, LN_ONE, LN_GARAGE, LN_GARAGE2, LN_DNB, LN_DNB2,
       LN_ELECTRO, LN_FOOT, LN_MATH, LN_BOSSA, LN_AFRO, LN_FUNK, LN_TRAP, LN_DUB, LN_JERSEY, LN_DRILL,
       LN_BACK, LN_BACKG, LN_HALFS, LN_HALFG, LN_MATHS, LN_BOSSAS, LN_FUNKS, LN_DRILLS,
       LN_H8, LN_H16, LN_HOFF, LN_HOPEN, LN_HSHUF, LN_HSPARSE, LN_HTRAP, LN_HGAR, LN_N };
static const char GN_LANE[LN_N][17] = {
    "................", "x...x...x...x...", "x......x..x.....", "x.......x.x.....", "x.x....x..x.....", "x......x.x......",
    "x.........x.....", "x...............", "x......x...x....", "x.........x..x..", "x.........x.....", "x......x..x.....",
    "x..x..x...x.....", "x..x..x..x..x...", "x..x..x...x.x...", "x..x....x..x....", "x......x..x.x...", "x.x....x..x.....",
    "x......x..x.....", "x.......x.......", "x..x..x.x..x..x.", "x.....x...x..x..",
    "....x.......x...", "....x..g....x..g", "........x.......", "........x......g", "....x..x....x...", "..x..x....x..x..",
    "....x..x.x..x..g", "........x.....x.",
    "x.x.x.x.x.x.x.x.", "xxxxxxxxxxxxxxxx", "..x...x...x...x.", "..o...o...o...o.", "x.xx.xx.xx.xx.x.", "x...x...x...x...",
    "x.x.x.xxx.x.x.xx", "..x.x.x...x.x.x."};

/* Kits (kawa_kits.h): 0 ACOUSTIC, 1 HIP HOP, 2 TECHNO, 3 DUB, 4 LO-FI, 5 GAME BOY, 6 SNES, 7 PERCUSSION. Breaks: 0 SOLEIL
 * (ride, live room), 1 GRELOT (70s funk), 2 TENDU (dry, claps), 3 ABYSSE (ragga jungle), 4 MAMBO (congas), 5 GRENIER
 * (dusty hip hop). Scales (kawa_data.c): 1 MAJOR, 2 MINOR, 6 DORIAN, 7 PHRYGIAN, 8 HARMONIC, 9 JAPANESE. The SPU
 * reverb's rooms (kawa_fx.c): 0 HALL, 1 STUDIO L, 2 STUDIO M, 3 STUDIO S, 4 ROOM, 5 HALF ECHO, 6 SPACE ECHO */
enum { SC_MAJ = 1, SC_MIN = 2, SC_DOR = 6, SC_PHR = 7, SC_HARM = 8, SC_JAP = 9 };
enum { RV_HALL, RV_STL, RV_STM, RV_STS, RV_ROOM, RV_HALF, RV_SPACE };

typedef struct {
    const char *name, *blurb;
    uint8_t bpm_lo, bpm_hi, swing, scale;
    int8_t prog[4];                             /* scale degrees, one a bar; -1 ends a shorter one */
    uint8_t brk, brk_src, kit, k, k2, s, h, h2; /* T1's part and its break; the kit; two kick lanes, a snare, two hat */
    uint8_t bass, key, rvb;                     /* the bass's and the keys' parts; the SPU reverb's room */
    const char *bass_snd, *key_snd, *key_snd2, *syn_snd, *t1_snd;   /* factory sounds, by their English name */
} genre_t;
#define P4(a, b, c, d) {a, b, c, d}
static const genre_t GENRES[] = {
    {"JRPG BATTLE", "BRASS, STRINGS, DRIVING ROCK", 150, 172, 50, SC_MIN, P4(0, 5, 6, 4), GB_ORCH, 0, 0, LN_BOOM2, LN_ELECTRO, LN_BACK, LN_H8, LN_H16,
     GL_EIGHTHS, GK_STABS, RV_STL, "SLAP BASS", "BRASS SECTION", "HORNS", "STRINGS", "TIMPANI"},
    {"BOSS BATTLE", "CHOIR, HITS, HARMONIC MINOR", 160, 184, 50, SC_HARM, P4(0, 5, 1, 4), GB_ORCH, 0, 0, LN_FOUR, LN_BOOM, LN_BACK, LN_H16, LN_H8,
     GL_EIGHTHS, GK_PADS, RV_HALL, "SYNTH BASS", "CHOIR AAH", "STRINGS", "HORNS", "ORCH HIT"},
    {"WORLD MAP", "HEROIC STRINGS AND FLUTE", 100, 120, 50, SC_MAJ, P4(0, 3, 4, 5), GB_ORCH, 0, 0, LN_HALF, LN_BOOM, LN_HALFS, LN_H8, LN_HSPARSE,
     GL_ROOTS, GK_PADS, RV_HALL, "FRETLESS", "STRINGS", "CHOIR AAH", "WOOD FLUTE", "TIMPANI"},
    {"TOWN", "PIANO, FLUTE, A WALKING BASS", 96, 116, 56, SC_MAJ, P4(0, 5, 1, 4), GB_OFF, 0, 7, LN_BOSSA, LN_BOSSA, LN_BOSSAS, LN_H8, LN_HSHUF,
     GL_WALK, GK_JAZZ, RV_ROOM, "FRETLESS", "PIANO", "E.PIANO", "WOOD FLUTE", "TIMPANI"},
    {"CELTIC FIELD", "FLUTE OVER HARP, DORIAN", 104, 124, 54, SC_DOR, P4(0, 6, 3, 0), GB_OFF, 0, 7, LN_DUB, LN_HALF, LN_HALFS, LN_HSPARSE, LN_H8,
     GL_ROOTS, GK_ARP, RV_HALL, "FRETLESS", "HARP", "HARP", "WOOD FLUTE", "TIMPANI"},
    {"DUNGEON", "BELLS IN THE DARK, TIMPANI", 76, 92, 50, SC_PHR, P4(0, 1, -1, 0), GB_ORCH, 0, 3, LN_ONE, LN_HALF, LN_NONE, LN_HSPARSE, LN_NONE,
     GL_SUB, GK_MOTIF, RV_SPACE, "SYNTH BASS", "CHIMES", "GLASS", "WARM PAD", "TIMPANI"},
    {"SAD THEME", "PIANO AND STRINGS, NO DRUMS", 64, 80, 50, SC_MIN, P4(0, 5, 3, 4), GB_OFF, 0, 0, LN_NONE, LN_NONE, LN_NONE, LN_NONE, LN_NONE,
     GL_SUB, GK_ARP, RV_HALL, "FRETLESS", "PIANO", "HARP", "STRINGS", "TIMPANI"},
    {"TEMPLE", "HARP, FLUTE, JAPANESE SCALE", 76, 92, 50, SC_JAP, P4(0, 3, 0, 4), GB_ORCH, 0, 7, LN_ONE, LN_HALF, LN_NONE, LN_HSPARSE, LN_NONE,
     GL_SUB, GK_MOTIF, RV_HALL, "FRETLESS", "HARP", "WOOD FLUTE", "WARM PAD", "TIMPANI"},
    {"LAST BOSS", "CHOIR AND ORCHESTRA HITS", 140, 156, 50, SC_HARM, P4(0, 5, 3, 4), GB_ORCH, 0, 0, LN_FOUR, LN_BOOM, LN_BACK, LN_H8, LN_H16,
     GL_EIGHTHS, GK_PADS, RV_HALL, "SYNTH BASS", "CHOIR OCTAVE", "CHOIR AAH", "HORNS", "ORCH HIT"},
    {"VICTORY HALL", "FANFARE, BRASS, A MARCH", 118, 132, 50, SC_MAJ, P4(0, 3, 4, 0), GB_ORCH, 0, 0, LN_FOUR, LN_HALF, LN_BACK, LN_H8, LN_H8,
     GL_ROOTS, GK_STABS, RV_HALL, "FRETLESS", "BRASS SECTION", "HORNS", "WOOD FLUTE", "TIMPANI"},
    {"MECH ARENA", "BREAKBEATS, POWER CHORDS, ACID", 150, 170, 50, SC_PHR, P4(0, 1, -1, 0), GB_CHOP, 2, 2, LN_DNB, LN_ELECTRO, LN_BACK, LN_H16, LN_H8,
     GL_ACID, GK_LEAD, RV_STM, "SYNTH BASS", "POWER CHORD", "RAVE STAB", "STRINGS", "ORCH HIT"},
    {"HANGAR", "TRIP-HOP IN THE GARAGE", 84, 96, 56, SC_MIN, P4(0, 0, 5, 4), GB_CHOP, 5, 4, LN_TRIP, LN_HALF, LN_HALFS, LN_HOFF, LN_H8,
     GL_WALK, GK_JAZZ, RV_STL, "FRETLESS", "E.PIANO", "LO-FI PIANO", "WARM PAD", "TIMPANI"},
    {"ORBITAL MECH", "TRANCE BREAKS, CHOIR IN ORBIT", 138, 150, 50, SC_MIN, P4(0, 5, 2, 6), GB_STRAIGHT, 2, 2, LN_FOUR, LN_FOUR, LN_BACK, LN_HOFF, LN_H16,
     GL_OFFBEAT, GK_ARP, RV_SPACE, "SYNTH BASS", "RAVE STAB", "CHIMES", "CHOIR AAH", "ORCH HIT"},
    {"STREET RACER", "PIANO HOUSE, SLAP BASS, 4/4", 128, 136, 52, SC_DOR, P4(0, 3, -1, 0), GB_SPARSE, 1, 2, LN_FOUR, LN_FOUR, LN_BACK, LN_HOPEN, LN_HOFF,
     GL_OFFBEAT, GK_STABS, RV_STS, "SLAP BASS", "PIANO", "RAVE MINOR", "HORNS", "ORCH HIT"},
    {"NIGHT RACER", "JUNGLE AT 165, RAVE STABS", 160, 172, 50, SC_MIN, P4(0, 5, 0, 6), GB_CHOP, 3, 2, LN_DNB, LN_NONE, LN_NONE, LN_NONE, LN_NONE,
     GL_SUB, GK_STABS, RV_HALL, "SYNTH BASS", "RAVE STAB", "STRINGS", "STRINGS", "ORCH HIT"},
    {"ANTIGRAV RACE", "DRUM & BASS, WARM PADS", 170, 176, 50, SC_MIN, P4(0, 5, 2, 6), GB_STRAIGHT, 2, 2, LN_DNB, LN_DNB2, LN_BACK, LN_H8, LN_H16,
     GL_SUB, GK_PADS, RV_HALL, "SYNTH BASS", "WARM PAD", "STRINGS", "CHIMES", "ORCH HIT"},
    {"ACID JAZZ MENU", "SHUFFLED E.PIANO, SLAP BASS", 100, 114, 60, SC_DOR, P4(1, 4, 0, 5), GB_CHOP, 1, 1, LN_FUNK, LN_BOOM3, LN_FUNKS, LN_H16, LN_HSHUF,
     GL_WALK, GK_JAZZ, RV_ROOM, "SLAP BASS", "E.PIANO", "HORNS", "WOOD FLUTE", "ORCH HIT"},
    {"FOGGY TOWN", "SURVIVAL HORROR TRIP-HOP", 76, 90, 54, SC_PHR, P4(0, 1, -1, 0), GB_SPARSE, 5, 4, LN_TRIP, LN_HALF, LN_HALFS, LN_HSPARSE, LN_HOFF,
     GL_SUB, GK_MOTIF, RV_SPACE, "FRETLESS", "LO-FI PIANO", "GLASS", "WARM PAD", "TIMPANI"},
    {"INFILTRATION", "STEALTH: PULSING BASS, STRINGS", 112, 126, 50, SC_MIN, P4(0, 6, 5, 4), GB_OFF, 0, 2, LN_ELECTRO, LN_BOOM, LN_BACK, LN_H16, LN_H8,
     GL_EIGHTHS, GK_PADS, RV_STM, "SYNTH BASS", "STRINGS", "STRING FIFTHS", "CHIMES", "TIMPANI"},
    {"INDUSTRIAL", "GRINDING POWER CHORDS", 116, 132, 50, SC_PHR, P4(0, 1, 0, 6), GB_STRAIGHT, 2, 2, LN_FOUR, LN_BOOM, LN_BACK, LN_H16, LN_HOFF,
     GL_EIGHTHS, GK_LEAD, RV_STM, "SYNTH BASS", "POWER CHORD", "HORNS", "CHOIR AAH", "ORCH HIT"},
    {"CYBER CITY", "ELECTRO, NEON, E.PIANO", 120, 130, 52, SC_MIN, P4(0, 5, 6, 4), GB_SPARSE, 2, 1, LN_ELECTRO, LN_ELECTRO, LN_BACK, LN_H16, LN_H8,
     GL_ACID, GK_STABS, RV_STL, "SYNTH BASS", "E.PIANO", "RAVE MINOR", "HORNS", "ORCH HIT"},
    {"SAVE ROOM", "A QUIET PLACE: BELLS, A PAD", 68, 80, 50, SC_MAJ, P4(0, 3, -1, 0), GB_OFF, 0, 0, LN_NONE, LN_NONE, LN_NONE, LN_NONE, LN_NONE,
     GL_SUB, GK_ARP, RV_SPACE, "FRETLESS", "CHIMES", "HARP", "WARM PAD", "TIMPANI"},
};
#define NGENRE ((uint32_t)(sizeof(GENRES) / sizeof(GENRES[0])))

/* standard progressions (KNOB 2): the scale each was written for; STYLE keeps the style's own */
typedef struct {
    const char *name;
    int8_t deg[4];
    uint8_t scale;                              /* 0: the style's */
} gprog_t;
static const gprog_t GPROGS[] = {
    {"STYLE", {0, 0, 0, 0}, 0},           {"AXIS POP", {0, 4, 5, 3}, SC_MAJ},  {"SAD POP", {5, 3, 0, 4}, SC_MAJ},
    {"DOO-WOP 50S", {0, 5, 3, 4}, SC_MAJ}, {"ROYAL ROAD", {3, 4, 2, 5}, SC_MAJ}, {"JAZZ II-V-I", {1, 4, 0, 0}, SC_MAJ},
    {"EPIC MINOR", {0, 5, 2, 6}, SC_MIN}, {"ANDALUSIAN", {0, 6, 5, 4}, SC_MIN}, {"TRIP NOIR", {0, 0, 5, 4}, SC_MIN},
    {"HAZY DORIAN", {0, 3, 5, 4}, SC_DOR}, {"TWO CHORDS", {0, 3, -1, 0}, SC_DOR},
};
#define NGPROG ((uint32_t)(sizeof(GPROGS) / sizeof(GPROGS[0])))

static uint8_t gn_genre, gn_prog, gn_energy = 8, gn_armed;
static uint32_t gn_armed_ms;
static int8_t gn_deg[4];
static uint8_t gn_ndeg;

static uint32_t gn_preset_en(const char *en)   /* a factory sound by its English name */
{
    uint32_t k;
    for (k = 0; k < NPRESET; k++) {
        const char *a = PRESETS[k].name[LANG_EN], *b = en;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return k;
    }
    return 0;
}
static int32_t gn_chord_deg(uint32_t pi, uint32_t bar) { return gn_deg[(pi * 2u + bar) % gn_ndeg]; }
static uint32_t gn_note(uint32_t base, int32_t deg)   /* a scale degree from the root under base */
{
    uint32_t n = scale_degree(deg, base);
    while (n > 100u) n -= 12u;
    while (n < 28u) n += 12u;
    return n;
}
static uint32_t gn_chord_fx(int32_t deg, uint32_t base, int rich)   /* the arpeggio (CHORDS) of the triad on a degree */
{
    uint32_t r = scale_degree(deg, base), third = scale_degree(deg + 2, base) - r, fifth = scale_degree(deg + 4, base) - r;
    uint32_t sev = scale_degree(deg + 6, base) - r;
    if (third == 3u && fifth == 6u) return 8u;                  /* DIM */
    if (!rich) return third == 3u ? 1u : 0u;                     /* MIN, MAJ */
    if (third == 3u) return 7u;                                  /* MI7 */
    return sev == 11u ? 6u : 5u;                                 /* MA7, 7 */
}
static void gn_put(uint32_t p, uint32_t r, uint32_t t, uint32_t note, uint32_t vol, uint32_t fx, int32_t fxv)
{
    if (r < NROW) put(p, r, t, note, vol > 15u ? 15u : vol < 1u ? 1u : vol, fx, fxv);
}
static int gn_has(uint32_t p, uint32_t r, uint32_t t) { return r < NROW && prj.pat[p].c[r][t].note; }

/* the break: a straight reading with some beats moved, chops, a storm, a sparse one, half time */
static void gn_break(uint32_t pi, uint32_t mode, uint32_t en)
{
    static const uint8_t ORDER[6][4] = {{0, 1, 2, 3}, {0, 1, 0, 3}, {0, 2, 1, 3}, {0, 1, 3, 2}, {0, 3, 2, 1}, {0, 1, 2, 1}};
    uint32_t b, i;
    for (b = 0; b < 2u; b++) {
        uint32_t o = b * 16u;
        if (mode == GB_STRAIGHT || mode == GB_CHOP) {
            const uint8_t *ord = ORDER[mode == GB_STRAIGHT && rnd(2) ? 0 : rnd(6)];
            for (i = 0; i < 16u; i++) {
                uint32_t beat = i / 4u, from = ord[beat];
                if (mode == GB_CHOP && (i & 1u) && rnd(7) == 0u) continue;
                if (beat && rnd(32) < en * (mode == GB_CHOP ? 2u : 1u)) from = rnd(4);
                gn_put(pi, o + i, WT_BRK, from * 4u + (i & 3u) + 1u, i % 4u ? (i & 1u ? 10u : 12u) : 14u, 0, 0);
            }
            if (b && rnd(16) < en + 4u) {
                gn_put(pi, o + 14u, WT_BRK, rnd(16) + 1u, 12, FX_ROLL, (int32_t)rnd(3));
                gn_put(pi, o + 15u, WT_BRK, rnd(16) + 1u, 11, FX_ROLL, (int32_t)(1u + rnd(3)));
            }
        } else if (mode == GB_STORM) {
            for (i = 0; i < 16u; i++) {
                uint32_t r = rnd(100), fx = 0;
                int32_t v = 0;
                if (rnd(12) == 0u) continue;
                if (r < 12u + en * 2u) { fx = FX_ROLL; v = (int32_t)rnd(5); }
                else if (r < 20u + en) fx = FX_REV;
                else if (r < 24u + en / 2u) { fx = FX_PITCH; v = rnd(2) ? 12 : -5; }
                gn_put(pi, o + i, WT_BRK, (rnd(2) ? i : rnd(16)) + 1u, i % 4u ? 9u + rnd(5) : 15u, fx, v);
            }
        } else if (mode == GB_SPARSE) {
            for (i = 0; i < 16u; i++)
                if (i % 8u == 0u || rnd(48) < en) gn_put(pi, o + i, WT_BRK, i + 1u, 8u + rnd(5), 0, 0);
        } else if (mode == GB_HALF) {
            for (i = 0; i < 16u; i += 2u) gn_put(pi, o + i, WT_BRK, b * 8u + i / 2u + 1u, i % 8u ? 11u : 14u, 0, 0);
        }
    }
}
/* the orchestra (T1, an SPU timpani or orchestra hit): the chord's root on the bar, a roll into the next section */
static void gn_orch(uint32_t pi, uint32_t v, uint32_t en, int hit)
{
    uint32_t b, base = (hit ? 48u : 36u) + prj.root;
    for (b = 0; b < 2u; b++) {
        uint32_t o = b * 16u;
        int32_t d = gn_chord_deg(pi, b);
        if (v == 2u || v == 7u) {                /* the breakdown and the outro: the first bar only */
            if (!b) gn_put(pi, o, WT_BRK, gn_note(base, d), 11, 0, 0);
            continue;
        }
        gn_put(pi, o, WT_BRK, gn_note(base, d), 14, 0, 0);
        if (hit) {                              /* the hits: on the one, and pushed before the next bar */
            if (rnd(16) < en + 2u) gn_put(pi, o + 6u, WT_BRK, gn_note(base, d), 12, 0, 0);
            if (b && rnd(16) < en) gn_put(pi, o + 14u, WT_BRK, gn_note(base, gn_chord_deg(pi, 2u)), 13, 0, 0);
        } else {                                /* the timpani: the fifth on three, a roll at the end */
            if (rnd(16) < en + 4u) gn_put(pi, o + 8u, WT_BRK, gn_note(base, d + 4), 11, 0, 0);
            if (b && (v == 3u || v == 6u || rnd(16) < en))
                gn_put(pi, o + 12u, WT_BRK, gn_note(base, d), 12, FX_ROLL, (int32_t)(2u + rnd(3)));
        }
    }
}

/* the kit (T2): kicks from the kick lane, snares and claps from the snare lane; the hats (T3) from the hat lane */
static void gn_drums(uint32_t pi, const char *kl, const char *sl, const char *hl, uint32_t en, int last_fill)
{
    uint32_t b, i;
    for (b = 0; b < 2u; b++) {
        uint32_t o = b * 16u;
        for (i = 0; i < 16u; i++) {
            if (sl[i] == 'x') gn_put(pi, o + i, WT_DRM, 3, 13, 0, 0);                    /* SNR */
            else if (sl[i] == 'g') gn_put(pi, o + i, WT_DRM, 3, 5, 0, 0);
            if (kl[i] == 'x') gn_put(pi, o + i, WT_DRM, 1, i % 4u ? 12u : 14u, 0, 0);    /* KIK (a kick wins its row) */
            else if (!gn_has(pi, o + i, WT_DRM) && !(i & 1u) && rnd(80) < en) gn_put(pi, o + i, WT_DRM, 1, 9, 0, 0);
            if (hl[i] == 'x') gn_put(pi, o + i, WT_HAT, 5, i % 4u == 0u ? 12u : (i & 1u) ? 7u : 10u, 0, 0);   /* CHH */
            else if (hl[i] == 'o') gn_put(pi, o + i, WT_HAT, 6, 10, 0, 0);                                  /* OHH */
            else if (hl[0] != '.' && rnd(100) < en) gn_put(pi, o + i, WT_HAT, 5, 6, FX_ROLL, 0);
        }
        if (b && last_fill && sl[4] != '.' && rnd(16) < en + 4u) gn_put(pi, o + 15u, WT_DRM, 3, 10, FX_ROLL, (int32_t)rnd(3));
    }
}
static void gn_bass(uint32_t pi, uint32_t mode, const char *kl, uint32_t en)
{
    uint32_t b, i, base = 36u + prj.root;
    for (b = 0; b < 2u; b++) {
        uint32_t o = b * 16u;
        int32_t d = gn_chord_deg(pi, b);
        switch (mode) {
        case GL_ROOTS:
            for (i = 0; i < 16u; i++)
                if (kl[i] == 'x' || i == 0u) {
                    gn_put(pi, o + i, WT_BAS, gn_note(base, d + (i && rnd(5) == 0u ? 4 : 0)), 12, 0, 0);
                    if (i + 3u < 16u && kl[i + 1u] != 'x' && kl[i + 2u] != 'x' && kl[i + 3u] != 'x' && rnd(3)) gn_put(pi, o + i + 3u, WT_BAS, NOTE_OFF, 0, 0, 0);
                }
            break;
        case GL_WALK: {
            static const uint8_t ROW[5] = {0, 6, 8, 11, 14};
            static const int8_t STEP[5] = {0, 2, 4, 5, 1};
            for (i = 0; i < 5u; i++)
                if (!i || rnd(16) < 9u + en / 2u) gn_put(pi, o + ROW[i], WT_BAS, gn_note(base, d + STEP[i] + (i == 4u && rnd(2) ? -2 : 0)), i ? 10u : 13u, 0, 0);
            break;
        }
        case GL_SUB:
            gn_put(pi, o, WT_BAS, gn_note(base, d), 13, 0, 0);
            if (rnd(2)) gn_put(pi, o + 10u, WT_BAS, gn_note(base, d + (rnd(2) ? 4 : 0)), 10, 0, 0);
            break;
        case GL_OFFBEAT:
            for (i = 2; i < 16u; i += 4u) gn_put(pi, o + i, WT_BAS, gn_note(base, d + (rnd(6) == 0u ? 7 : 0)), 12, 0, 0);
            break;
        case GL_EIGHTHS:
            for (i = 0; i < 16u; i += 2u) gn_put(pi, o + i, WT_BAS, gn_note(base, d + (i == 12u && rnd(3) == 0u ? 4 : 0)), i % 4u ? 10u : 13u, 0, 0);
            break;
        default: {                              /* ACID: 16ths, accents, octaves, slides */
            static const int8_t A[8] = {0, 0, 0, 7, 2, 4, -7, 1};
            for (i = 0; i < 16u; i++) {
                if (i && rnd(16) >= 8u + en / 2u) { if (rnd(3) == 0u) gn_put(pi, o + i, WT_BAS, NOTE_OFF, 0, 0, 0); continue; }
                gn_put(pi, o + i, WT_BAS, gn_note(base, d + A[rnd(8)]), rnd(3) ? 9u : 15u, rnd(5) == 0u ? FX_GLIDE : 0u, 6);
            }
        }
        }
    }
}
/* KEY (T5) or SYN (T6): chords (an arpeggio of the chord, as the trackers play them) or a melody over the chords */
static void gn_keys(uint32_t pi, uint32_t t, uint32_t mode, uint32_t base_up, uint32_t en)
{
    static const uint8_t RH[6][6] = {{0, 3, 6, 10, 99}, {0, 2, 4, 7, 10, 12}, {0, 6, 8, 14, 99}, {2, 5, 8, 11, 14, 99},
                                     {0, 3, 7, 10, 13, 99}, {0, 4, 6, 8, 12, 99}};
    static const int8_t ARP[4][8] = {{0, 2, 4, 7, 4, 2, 0, 2}, {0, 4, 2, 7, 0, 4, 2, 9}, {7, 4, 2, 0, 2, 4, 7, 9}, {0, 2, 4, 2, 0, 4, 7, 4}};
    uint32_t b, i, base = (GK_IS_CHORD(mode) ? 55u : 60u + base_up) + prj.root;
    const uint8_t *rh = RH[rnd(6)];
    const int8_t *ap = ARP[rnd(4)];
    int8_t md[6];
    int32_t d = (int32_t)rnd(3) * 2;
    for (i = 0; i < 6u; i++) { md[i] = (int8_t)d; d = clampi(d + (int32_t)rnd(6) - 2, -3, 9); }   /* one motif, every bar */
    for (b = 0; b < 2u; b++) {
        uint32_t o = b * 16u;
        int32_t c = gn_chord_deg(pi, b);
        int rich = rnd(16) < en + 4u;
        switch (mode) {
        case GK_PADS:
            gn_put(pi, o, t, gn_note(base, c), 9, FX_ARP, (int32_t)gn_chord_fx(c, base, rich));
            break;
        case GK_STABS: {
            static const uint8_t R[4][4] = {{0, 10, 99}, {3, 10, 99}, {0, 6, 12, 99}, {2, 6, 10, 14}};
            const uint8_t *r = R[rnd(4)];
            for (i = 0; i < 4u && r[i] < 16u; i++) {
                gn_put(pi, o + r[i], t, gn_note(base, c), 11, FX_ARP, (int32_t)gn_chord_fx(c, base, rich));
                if (r[i] + 2u < 16u && !gn_has(pi, o + r[i] + 2u, t)) gn_put(pi, o + r[i] + 2u, t, NOTE_OFF, 0, 0, 0);
            }
            break;
        }
        case GK_JAZZ: {
            static const uint8_t R[3][3] = {{0, 7, 10}, {0, 3, 10}, {2, 7, 14}};
            const uint8_t *r = R[rnd(3)];
            for (i = 0; i < 3u; i++) {
                gn_put(pi, o + r[i], t, gn_note(base, c + (i && rnd(3) == 0u ? 2 : 0)), 9u + rnd(4), FX_ARP, (int32_t)gn_chord_fx(c, base, 1));
                if (r[i] + 3u < 16u && !gn_has(pi, o + r[i] + 3u, t)) gn_put(pi, o + r[i] + 3u, t, NOTE_OFF, 0, 0, 0);
            }
            break;
        }
        case GK_MOTIF:
            for (i = 0; i < 6u && rh[i] < 16u; i++) {
                int32_t dd = md[i];
                if (b && (i == 5u || rh[i + 1u] >= 16u)) dd = rnd(2) ? 0 : 4;   /* the second bar lands home */
                gn_put(pi, o + rh[i], t, gn_note(base, c + dd), i ? 9u + rnd(3) : 12u, 0, 0);
            }
            break;
        case GK_ARP:
            for (i = 0; i < 16u; i += 2u)
                if (!i || rnd(16) < 11u + en / 3u) gn_put(pi, o + i, t, gn_note(base, c + ap[i / 2u]), i % 4u ? 8u : 11u, 0, 0);
            break;
        default: {                              /* LEAD: a few long notes, stepping */
            static const uint8_t R[4][3] = {{0, 8, 99}, {0, 6, 12}, {4, 12, 99}, {2, 8, 14}};
            const uint8_t *r = R[rnd(4)];
            int32_t dd = (int32_t)rnd(3) * 2;
            for (i = 0; i < 3u && r[i] < 16u; i++) {
                if (i) dd = clampi(dd + (int32_t)rnd(5) - 2, -2, 9);
                gn_put(pi, o + r[i], t, gn_note(base, c + dd), 9u + rnd(4), rnd(4) == 0u ? FX_VIB : 0u, 5);
            }
        }
        }
    }
}

/* one section of the song into pattern pi (A..H = 0..7) */
static void gn_section(uint32_t pi, const genre_t *g, uint32_t v, uint32_t en, const uint8_t *km)
{
    uint32_t r, t, brk = g->brk, bass = g->bass, key = km[0], syn = km[2];
    const char *kl = GN_LANE[g->k], *sl = GN_LANE[g->s], *hl = GN_LANE[g->h];
    int chords = GK_IS_CHORD(km[0]), drums = 1, fill = 0;
    pat_t *p = &prj.pat[pi];
    for (r = 0; r < NROW; r++)
        for (t = 0; t < NTRK; t++) memset(&p->c[r][t], 0, sizeof(cell_t));
    memset(p->live, 0, sizeof p->live);
    p->len = 32;
    p->pad[0] = 0;
    switch (v) {
    case 1: en = en + 3u > 15u ? 15u : en + 3u; break;                                   /* B */
    case 2: drums = 0; if (brk != GB_OFF && brk != GB_ORCH) brk = GB_SPARSE; bass = GL_SUB; key = chords ? GK_PADS : km[1]; syn = chords ? 99u : GK_PADS; break;
    case 3: en = 15; fill = 1; break;                                                    /* D */
    case 4: kl = GN_LANE[g->k2]; hl = GN_LANE[g->h2]; key = km[1]; syn = km[3]; break;   /* E */
    case 5:                                                                              /* F */
        kl = GN_LANE[LN_HALF]; sl = GN_LANE[LN_HALFS]; hl = GN_LANE[LN_HSPARSE];
        if (brk != GB_OFF && brk != GB_ORCH) brk = GB_HALF;
        if (!chords) key = GK_LEAD;
        break;
    case 6: en = en + 4u > 15u ? 15u : en + 4u; if (brk != GB_OFF && brk != GB_ORCH) brk = g->brk == GB_STORM ? GB_STORM : GB_CHOP; fill = 1; break;
    case 7: drums = 0; if (brk != GB_OFF && brk != GB_ORCH) brk = GB_SPARSE; bass = GL_SUB; key = chords ? GK_PADS : km[1]; syn = 99u; break;   /* H */
    }
    if (g->brk == GB_ORCH) gn_orch(pi, v, en, g->t1_snd[0] == 'O');
    else if (brk != GB_OFF) gn_break(pi, brk, en);
    if (drums) gn_drums(pi, kl, sl, hl, en, fill || v == 0u);
    else if (v == 7u) gn_drums(pi, GN_LANE[LN_NONE], GN_LANE[LN_NONE], GN_LANE[LN_HSPARSE], 0, 0);
    gn_bass(pi, bass, kl, en);
    gn_keys(pi, WT_KEY, key, 0, en);
    if (syn != 99u) gn_keys(pi, WT_SYN, syn, GK_IS_CHORD(syn) ? 0u : 12u, en);
}

/* the whole song: the project's settings, the sounds, eight sections, the chain */
static void gn_make(void)
{
    const genre_t *g = &GENRES[gn_genre % NGENRE];
    const gprog_t *pr = &GPROGS[gn_prog % NGPROG];
    static const uint8_t CHAIN[10] = {0, 1, 0, 4, 2, 5, 6, 3, 0, 7};
    static const uint8_t VOL[NTRK] = {11, 10, 9, 11, 12, 11};  /* the drums sit under the music (the kick at about half) */
    uint8_t km[4];
    uint32_t i, en = gn_energy;
    rnd_main ^= fm1_ms * 0x9E3779B1u + gn_genre;
    if (!rnd_main) rnd_main = 1;
    sq_req = sq_play ? 2 : 0;                   /* (it stops: every pattern changes) */
    ev_push(EV_ALLOFF, NTRK, 0, 0, 0, 0);
    /* the progression and the scale */
    if (pr->scale) { for (i = 0; i < 4u; i++) gn_deg[i] = pr->deg[i]; prj.scale = pr->scale; }
    else { for (i = 0; i < 4u; i++) gn_deg[i] = g->prog[i]; prj.scale = g->scale; }
    for (gn_ndeg = 0; gn_ndeg < 4u && gn_deg[gn_ndeg] >= 0; gn_ndeg++) {}
    if (!gn_ndeg) { gn_deg[0] = 0; gn_ndeg = 1; }
    prj.root = (uint8_t)rnd(12);
    prj.lock = 1;
    prj.bpm = (uint16_t)(g->bpm_lo + rnd((uint32_t)(g->bpm_hi - g->bpm_lo) + 1u));
    prj.swing = (uint8_t)clampi((int32_t)g->swing + (int32_t)rnd(5) - 2, 50, 75);
    prj.grv.on = 0;
    /* the sounds: the kit first (it leaves the FM track free), then the FM keys */
    sound_preset(WT_DRM, preset_named(E_KIT, g->kit));
    sound_preset(WT_HAT, preset_named(E_KIT, g->kit));
    sound_preset(WT_BRK, g->brk == GB_ORCH || g->brk == GB_OFF ? gn_preset_en(g->t1_snd) : preset_named(E_SAMPLE, g->brk_src));
    sound_preset(WT_BAS, gn_preset_en(g->bass_snd));
    sound_preset(WT_SYN, gn_preset_en(g->syn_snd));
    sound_preset(WT_KEY, gn_preset_en(rnd(3) ? g->key_snd : g->key_snd2));
    for (i = 0; i < NTRK; i++) {               /* the SPU's tracks drenched, as they were: the reverb did the room */
        prj.snd[i].p[P_VOL] = (int8_t)VOL[i];
        prj.snd[i].p[P_ECHO] = (int8_t)(i == WT_KEY ? 2 : i == WT_SYN ? 1 : 0);
        prj.snd[i].p[P_VERB] = (int8_t)(i == WT_BRK ? (g->brk == GB_ORCH ? 7 : 3) : i == WT_BAS ? 2 : i >= WT_KEY ? 7 + (i == WT_SYN) * 2 : 3);
    }
    prj.grv.pad[0] = g->rvb;
    if (g->brk != GB_ORCH && g->brk != GB_OFF) prj.snd[WT_BRK].p[P_VOL] = 8;   /* (a break is a whole kit: hot) */
    /* the parts' styles: the style's, and another for the sections that turn */
    km[0] = g->key;
    if (GK_IS_CHORD(g->key)) { km[1] = (uint8_t)(GK_PADS + rnd(3)); km[2] = (uint8_t)rnd(3); km[3] = (uint8_t)rnd(3); }
    else { km[1] = (uint8_t)rnd(3); km[2] = (uint8_t)(GK_PADS + rnd(3)); km[3] = GK_PADS; }
    for (i = 0; i < 8u; i++) gn_section(i, g, i, en, km);
    prj.song_len = 10;
    for (i = 0; i < 10u; i++) { prj.song[i].pat = CHAIN[i]; prj.song[i].rep = 1; }
    prj.mode = 1;
    {                                           /* its name: the style's first word, the key, a number */
        char *s = prj.name;
        const char *a = g->name;
        uint32_t n = 0, k = rnd(90) + 10u;
        memset(prj.name, 0, sizeof prj.name);
        while (*a && *a != ' ' && *a != '-' && n < 10u) s[n++] = *a++;
        s[n++] = ' ';
        s[n++] = NOTE_NAME[prj.root][0];
        if (NOTE_NAME[prj.root][1] == '#') s[n++] = '#';
        s[n++] = ' ';
        s[n++] = (char)('0' + k / 10u);
        s[n++] = (char)('0' + k % 10u);
    }
    brushes_reset();
    touched();
}
