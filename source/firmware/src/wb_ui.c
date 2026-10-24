/* SPDX-License-Identifier: GPL-3.0-only */
/* Wonderblue-OS: the panel and the screen, as the browser version laid out for the FM-1.
 *
 *   SCREEN         the tracker grid (or the page's own view), the status line, the four knobs, F1..F4
 *   KNOB 1..4      the page's knobs          SELECT  the cursor row      ALGORITHM  the track
 *   PRESETS        SND: the sound, SNG: the genre, CHD: the progression, else the pattern
 *   WHITE KEYS     the 16 pads (slices, drum tunes, notes in the scale; chords on CHD)
 *   BLACK 1..7     the track: BRK KIK SNR HAT BAS KEY SYN        BLACK 8..11  F1..F4
 *   SAVE SNG  HOME TRK  EDIT SND  SCL CHD  LFO GEN  ENV MIX  GLO MST (again: the page's next knobs)
 *   FX + white     punch-ins while held      SEQ + white 1..8  pattern A..H (9: loop / chain)
 *   ARP + pad      note repeat               PLAY, REC, HOME + PLAY: SIT      OCT- / OCT+  the pads' octave
 * The store: sixteen slots of 16 KiB from STORE_LO, slot 0 the autosave. */

enum { PG_SNG, PG_TRK, PG_SND, PG_CHD, PG_GEN, PG_MIX, PG_MST, PG_N };
static const char *const PG_NAME[PG_N] = {"SNG", "TRK", "SND", "CHD", "GEN", "MIX", "MST"};
static const char *const PG_HINT[PG_N] = {"SAVE, LOAD, NEW SONG", "NOTES AND EFFECTS", "PICK AND SHAPE THE SOUND",
                                          "PADS SET THE CHORDS", "CHANCE, MUTATION, DRIFT", "LEVEL, PAN, SENDS",
                                          "TEMPO, SWING, TAPE, CRUSH"};
static const uint8_t PG_BTN[PG_N] = {BTN_SAVE, BTN_HOME, BTN_EDIT, BTN_SEL, BTN_LFO, BTN_ENV, BTN_GLO};

/* ---- Wonderblue's colours: charcoal, amber for what plays, blue for what is selected, teal for the chords */
#define C_BG RGB(20, 21, 25)
#define C_PANEL RGB(33, 34, 40)
#define C_BEAT RGB(28, 29, 35)
#define C_TXT RGB(214, 210, 200)
#define C_MUTED RGB(98, 100, 112)
#define C_FAINT RGB(58, 60, 70)
#define C_AMBER RGB(255, 172, 44)
#define C_BLUE RGB(84, 152, 255)
#define C_TEAL RGB(72, 206, 186)
#define C_REDC RGB(240, 84, 84)
static const uint16_t TRK_COL[NT] = {RGB(255, 150, 60), RGB(240, 96, 96), RGB(246, 132, 190), RGB(240, 214, 90),
                                     RGB(96, 160, 255), RGB(72, 206, 186), RGB(176, 132, 255)};
static const uint16_t KNOB_C[4] = {RGB(255, 96, 96), RGB(255, 204, 64), RGB(96, 226, 120), RGB(88, 164, 255)};

/* ---- the keys: white and black, in order */
#define KEY0 53u                                /* key 0 = F3 */
static uint8_t KEY_OF_WHITE[16], KEY_OF_BLACK[11];
static int8_t WHITE_OF_KEY[27], BLACK_OF_KEY[27];
static void keys_init(void)
{
    uint32_t k, w = 0, b = 0;
    for (k = 0; k < 27u; k++) {
        WHITE_OF_KEY[k] = BLACK_OF_KEY[k] = -1;
        if ((0x54Au >> ((KEY0 + k) % 12u)) & 1u) { BLACK_OF_KEY[k] = (int8_t)b; KEY_OF_BLACK[b++] = (uint8_t)k; }
        else { WHITE_OF_KEY[k] = (int8_t)w; KEY_OF_WHITE[w++] = (uint8_t)k; }
    }
}

/* ---- the UI's state */
static uint8_t page = PG_TRK, sub, cur_row, chd_slot, slot_sel = 1, genre_pick, prog_sel;
static uint8_t last_note[NT] = {0, 60, 60, 60, 36, 60, 60};
static uint8_t arm_f = 0xFF;                    /* the F button waiting for its second press (SNG) */
static uint32_t arm_ms, toast_ms, frame_ms, dirty_ms, prog_ms, aud_off_ms, flash_ms[NT];
static uint8_t dirty, prog_due, aud_off_t = 0xFF;
static char toast_s[32];
static char knob_lab[4][16];
static uint32_t prev_keys, punch_keys, pad_keys;
static uint32_t taps[4], ntap;
static uint8_t led_want[41];

static void toast(const char *s)
{
    uint32_t i = 0;
    while (s[i] && i < 29u) { toast_s[i] = s[i]; i++; }
    toast_s[i] = 0;
    toast_ms = fm1_ms ? fm1_ms : 1u;
}
static void touch(void) { dirty = 1; dirty_ms = fm1_ms; }
static pat_t *P(void) { return &song.pat[song.cur % NPAT]; }
static uint32_t plen(void) { uint32_t l = P()->len; return l && l <= NROW ? l : 16u; }
static uint32_t trk(void) { return sel_trk % NT; }
static int quiet(void)
{
    uint32_t i;
    for (i = 0; i < NV; i++) if (vo[i].kind != K_FREE) return 0;
    return 1;
}
static void audition(uint32_t t, uint32_t n)    /* a short note on a track (the knobs, HEAR) */
{
    ev_push(EV_ON, t, n, 12);
    aud_off_t = (uint8_t)t;
    aud_off_ms = fm1_ms + (TRK_TYPE[t] == TY_SYNTH ? 450u : 0u);
}

/* ================================================================ the store */
#define SLOT_SIZE 0x4000u
#define NSLOT 16u
typedef char song_fits_a_slot[sizeof(song_t) <= SLOT_SIZE ? 1 : -1];
static char slot_name[NSLOT][16];
static uint8_t slot_ok[NSLOT];
#if defined(DINGHY_HOST) || defined(KAWA_WASM)
static uint8_t host_flash[NSLOT][SLOT_SIZE];
static uint32_t store_gen;
static int store_read(uint32_t k, uint32_t off, void *p, uint32_t n) { memcpy(p, host_flash[k] + off, n); return 0; }
static int store_write(uint32_t k, const void *p, uint32_t n) { memcpy(host_flash[k], p, n); store_gen++; return 0; }
static int store_erase(uint32_t k) { memset(host_flash[k], 0xFF, SLOT_SIZE); store_gen++; return 0; }
#else
static uint32_t slot_at(uint32_t k) { return STORE_LO + k * SLOT_SIZE; }
static int store_read(uint32_t k, uint32_t off, void *p, uint32_t n) { return st_read(slot_at(k) + off, p, n); }
static int store_erase(uint32_t k)
{
    uint32_t s, took;
    int rc;
    if (!flash_ok) return -1;
    for (s = 0; s < SLOT_SIZE; s += 0x1000u)
        if ((rc = fl_erase4k(slot_at(k) + s, &took)) != 0) return rc;
    return 0;
}
static int store_write(uint32_t k, const void *p, uint32_t n)
{
    int rc;
    if ((rc = store_erase(k)) != 0) return rc;
    if ((rc = fl_write(slot_at(k) + 8u, (const uint8_t *)p + 8u, n - 8u)) != 0)   /* the magic last: a cut save stays */
        return rc;                                                               /* invalid */
    return fl_write(slot_at(k), (const uint8_t *)p, 8u);
}
#endif
typedef char store_fits[STORE_LO + NSLOT * SLOT_SIZE <= STORE_HI ? 1 : -1];
static uint32_t crc_step(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c;
}
#define SONG_BODY ((uint32_t)((uint8_t *)&song.crc - (uint8_t *)&song))
static int slot_valid(uint32_t k, char *name)   /* the slot holds a song (its CRC checked a piece at a time) */
{
    uint8_t buf[256];
    uint32_t h[2], c = 0xFFFFFFFFu, off, crc;
    if (store_read(k, 0, h, 8) || h[0] != SONG_MAGIC || h[1] != SONG_VERSION) return 0;
    for (off = 0; off < SONG_BODY; off += sizeof buf) {
        uint32_t n = SONG_BODY - off < sizeof buf ? SONG_BODY - off : sizeof buf;
        if (store_read(k, off, buf, n)) return 0;
        c = crc_step(c, buf, n);
        if (off == 0u && name) { memcpy(name, buf + 8, 15); name[15] = 0; }
    }
    if (store_read(k, SONG_BODY, &crc, 4)) return 0;
    return ~c == crc;
}
static void slots_scan(void)
{
    uint32_t k;
    for (k = 0; k < NSLOT; k++) {
        slot_ok[k] = (uint8_t)slot_valid(k, slot_name[k]);
        if (!slot_ok[k]) slot_name[k][0] = 0;
    }
}
static int song_save(uint32_t k)
{
    song.magic = SONG_MAGIC;
    song.version = SONG_VERSION;
    song.crc = ~crc_step(0xFFFFFFFFu, (const uint8_t *)&song, SONG_BODY);
    if (store_write(k, &song, sizeof song)) return -1;
    slot_ok[k] = 1;
    memcpy(slot_name[k], song.name, 16);
    slot_name[k][15] = 0;
    return 0;
}
static int song_load(uint32_t k)
{
    if (!slot_valid(k, 0)) return -1;
    sq_req = 2;
    if (store_read(k, 0, &song, sizeof song)) return -1;
    seed_of = 0xFF;
    undo_ok = 0;
    sit_mute = 0;
    return 0;
}

/* ================================================================ the knobs
 * a sound parameter: its place in p[], its range and how it reads */
enum { F_INT, F_ST, F_MS, F_PCT, F_X10, F_X100, F_CHORD, F_RATIO, F_WAVE, F_UNI, F_CT, F_VOWEL, F_CTR, F_SYN, F_BRK, F_HZ,
       F_SOUND, F_OCT };
typedef struct {
    const char *lab;
    int8_t idx;                                 /* -1: the factory sound */
    int16_t lo, hi;
    uint8_t fmt, rnd;                           /* rnd: RAND may change it */
} pd_t;
#define PD_END {0, 0, 0, 0, 0, 0}
static const pd_t SP_KICK[2][4] = {{{"SOUND", -1, 0, WB_NKICK - 1, F_SOUND, 0}, {"TUNE", PK_TUNE, -12, 12, F_ST, 1}, {"DECAY", PK_DECAY, 100, 1500, F_MS, 1}, {"PUNCH", PK_PUNCH, 0, 100, F_PCT, 1}},
                                   {{"DRIVE", PK_DRIVE, 0, 100, F_PCT, 1}, PD_END, PD_END, PD_END}};
static const pd_t SP_SNARE[2][4] = {{{"SOUND", -1, 0, WB_NSNARE - 1, F_SOUND, 0}, {"TUNE", PS_TUNE, -12, 12, F_ST, 1}, {"DECAY", PS_DECAY, 50, 600, F_MS, 1}, {"TONE", PS_TONE, 0, 100, F_PCT, 1}},
                                    {{"SNAP", PS_SNAP, 0, 100, F_PCT, 1}, {"CLAP", PS_CLAP, 0, 100, F_PCT, 1}, PD_END, PD_END}};
static const pd_t SP_HAT[2][4] = {{{"SOUND", -1, 0, WB_NHAT - 1, F_SOUND, 0}, {"TUNE", PH_TUNE, -12, 12, F_ST, 1}, {"DECAY", PH_DECAY, 20, 400, F_MS, 1}, {"TONE", PH_TONE, 0, 100, F_PCT, 1}},
                                  {{"METAL", PH_METAL, 0, 100, F_PCT, 1}, PD_END, PD_END, PD_END}};
static const pd_t SP_BRK[1][4] = {{{"BREAK", PB_SRC, 0, BRK_N - 1, F_BRK, 0}, {"PITCH", PB_PITCH, -12, 12, F_ST, 1}, {"GATE", PB_GATE, 10, 200, F_PCT, 1}, {"TONE", PB_TONE, 0, 100, F_HZ, 1}}};
static const pd_t SP_FM[3][4] = {
    {{"SOUND", -1, 0, WB_NSYN - 1, F_SOUND, 0}, {"BRIGHT", PY_INDEX, 0, 80, F_X10, 1}, {"DECAY", PY_DEC, 50, 4000, F_MS, 1}, {"CHORD", PY_CHORD, 0, WB_NCHORD - 1, F_CHORD, 0}},
    {{"RATIO", PY_RATIO, 0, WB_NRATIO - 1, F_RATIO, 1}, {"ATTACK", PY_ATT, 2, 3000, F_MS, 1}, {"REL", PY_REL, 20, 4000, F_MS, 1}, {"DETUNE", PY_DET, 0, 30, F_CT, 1}},
    {{"TINE", PY_TINE, 0, 100, F_PCT, 1}, {"SUS", PY_SUS, 0, 100, F_PCT, 1}, {"OCTAVE", PY_OCT, -2, 2, F_OCT, 0}, PD_END}};
static const pd_t SP_VA[4][4] = {
    {{"SOUND", -1, 0, WB_NSYN - 1, F_SOUND, 0}, {"BRIGHT", PY_INDEX, 0, 80, F_X10, 1}, {"DECAY", PY_DEC, 50, 4000, F_MS, 1}, {"CHORD", PY_CHORD, 0, WB_NCHORD - 1, F_CHORD, 0}},
    {{"ATTACK", PY_ATT, 2, 3000, F_MS, 1}, {"REL", PY_REL, 20, 4000, F_MS, 1}, {"DETUNE", PY_DET, 0, 40, F_CT, 1}, {"RESO", PY_RESO, 5, 180, F_X10, 1}},
    {{"F.ENV", PY_FENV, 0, 400, F_X100, 1}, {"UNISON", PY_UNI, 1, 3, F_UNI, 1}, {"WAVE", PY_WAVE, 0, 3, F_WAVE, 1}, PD_END},
    {{"SWOOP", PY_GLIDE, -12, 12, F_ST, 1}, {"SUS", PY_SUS, 0, 100, F_PCT, 1}, {"OCTAVE", PY_OCT, -2, 2, F_OCT, 0}, {"VIB", PY_VIB, 0, 60, F_CT, 1}}};
static const pd_t SP_VOX[3][4] = {
    {{"SOUND", -1, 0, WB_NSYN - 1, F_SOUND, 0}, {"LEVEL", PY_INDEX, 0, 80, F_X10, 1}, {"DECAY", PY_DEC, 50, 4000, F_MS, 1}, {"CHORD", PY_CHORD, 0, WB_NCHORD - 1, F_CHORD, 0}},
    {{"ATTACK", PY_ATT, 2, 3000, F_MS, 1}, {"REL", PY_REL, 20, 4000, F_MS, 1}, {"DETUNE", PY_DET, 0, 40, F_CT, 1}, {"VOWEL", PY_VOWEL, 0, 4, F_VOWEL, 1}},
    {{"SWOOP", PY_GLIDE, -12, 12, F_ST, 1}, {"SUS", PY_SUS, 0, 100, F_PCT, 1}, {"OCTAVE", PY_OCT, -2, 2, F_OCT, 0}, {"VIB", PY_VIB, 0, 60, F_CT, 1}}};
static const pd_t SP_CTR[4] = {{"CTR", PY_CTR, 0, WB_NCTR - 1, F_CTR, 0}, {"C.SND", PY_CTRP, 0, WB_NSYN - 1, F_SYN, 0}, {"C.LVL", PY_CTRL, 0, 100, F_PCT, 0}, PD_END};

static uint32_t snd_pages(uint32_t t, const pd_t (**pg)[4])   /* the track's knob pages (and the counter voice's on KEY) */
{
    switch (TRK_TYPE[t]) {
    case TY_BREAK: *pg = SP_BRK; return 1;
    case TY_KICK: *pg = SP_KICK; return 2;
    case TY_SNARE: *pg = SP_SNARE; return 2;
    case TY_HAT: *pg = SP_HAT; return 2;
    default:
        switch (song.trk[t].p[PY_ENG]) {
        case 1: *pg = SP_VA; return 4;
        case 2: *pg = SP_VOX; return 3;
        default: *pg = SP_FM; return 3;
        }
    }
}
static uint32_t snd_npages(uint32_t t) { const pd_t (*pg)[4]; return snd_pages(t, &pg) + (t == T_KEY ? 1u : 0u); }
static const pd_t *snd_pd(uint32_t t, uint32_t s, uint32_t i)
{
    const pd_t (*pg)[4];
    uint32_t n = snd_pages(t, &pg);
    s %= snd_npages(t);
    if (s >= n) return SP_CTR[i].lab ? &SP_CTR[i] : 0;
    return pg[s][i].lab ? &pg[s][i] : 0;
}

static void pd_fmt(char *o, const pd_t *d, int32_t v, uint32_t t)
{
    char *e;
    o[0] = 0;
    switch (d->fmt) {
    case F_ST: e = fmt_s(o, v); str_cat(e, " ST"); break;
    case F_MS:
        if (v < 1000) { e = fmt_u(o, (uint32_t)v, 0); str_cat(e, "MS"); }
        else { e = fmt_u(o, (uint32_t)v / 1000u, 0); *e++ = '.'; e = fmt_u(e, (uint32_t)v % 1000u / 10u, 2); str_cat(e, "S"); }
        break;
    case F_PCT: e = fmt_u(o, (uint32_t)v, 0); str_cat(e, "%"); break;
    case F_X10: e = fmt_u(o, (uint32_t)v / 10u, 0); *e++ = '.'; fmt_u(e, (uint32_t)v % 10u, 1); break;
    case F_X100: e = fmt_u(o, (uint32_t)v / 100u, 0); *e++ = '.'; fmt_u(e, (uint32_t)v % 100u, 2); break;
    case F_CHORD: str_cat(o, WB_CHORD_NAME[clampi(v, 0, WB_NCHORD - 1)]); break;
    case F_RATIO: {
        uint32_t r = (uint32_t)v;
        e = fmt_u(o, r / 100u, 0);
        if (r % 100u) { *e++ = '.'; e = fmt_u(e, r % 100u, r % 10u ? 2u : 0u); if (!(r % 10u)) { e[-1] = 0; e = fmt_u(e - 1, r % 100u / 10u, 0); } }
        str_cat(e, ":1");
        break;
    }
    case F_WAVE: str_cat(o, (const char *const[]){"SAW", "SQUARE", "TRI", "SINE"}[clampi(v, 0, 3)]); break;
    case F_UNI: e = fmt_u(o, (uint32_t)v, 0); str_cat(e, "X"); break;
    case F_CT: e = fmt_u(o, (uint32_t)v, 0); str_cat(e, " CT"); break;
    case F_VOWEL: str_cat(o, (const char *const[]){"A", "E", "I", "O", "U"}[clampi(v, 0, 4)]); break;
    case F_CTR: str_cat(o, WB_CTR_NAME[clampi(v, 0, WB_NCTR - 1)]); break;
    case F_SYN: str_cat(o, WB_SYN_NAME[clampi(v, 0, WB_NSYN - 1)]); break;
    case F_BRK: str_cat(o, BRK_NAME[clampi(v, 0, BRK_N - 1)]); break;
    case F_HZ: { uint32_t f = 180u + (uint32_t)(v * v) * 2u; if (v >= 98) str_cat(o, "OPEN"); else { e = fmt_u(o, f / 100u * 100u, 0); str_cat(e, "HZ"); } break; }
    case F_SOUND: str_cat(o, patch_name(t, (uint32_t)v)); break;
    case F_OCT: fmt_s(o, v); break;
    default: fmt_s(o, v); break;
    }
}
static int32_t pd_get(const pd_t *d, uint32_t t)
{
    const int16_t *p = song.trk[t].p;
    if (d->idx < 0) return song.trk[t].patch;
    if (d->fmt == F_RATIO) {                    /* the nearest ratio of the list */
        uint32_t i, b = 0;
        for (i = 0; i < WB_NRATIO; i++)
            if (clampi(p[PY_RATIO] - (int32_t)WB_RATIO[i], -9999, 9999) * clampi(p[PY_RATIO] - (int32_t)WB_RATIO[i], -9999, 9999) <
                clampi(p[PY_RATIO] - (int32_t)WB_RATIO[b], -9999, 9999) * clampi(p[PY_RATIO] - (int32_t)WB_RATIO[b], -9999, 9999)) b = i;
        return (int32_t)b;
    }
    return p[d->idx];
}
static void pd_set(const pd_t *d, uint32_t t, int32_t v)
{
    v = clampi(v, d->lo, d->hi);
    if (d->idx < 0) { sound_patch(t, (uint32_t)v); return; }
    song.trk[t].p[d->idx] = (int16_t)(d->fmt == F_RATIO ? (int32_t)WB_RATIO[v] : v);
}
static int32_t step_of(const pd_t *d, int32_t v, int32_t dd)   /* times move faster when long */
{
    if (d->fmt == F_MS) { int32_t s = v / 16; return v + dd * (s < 1 ? 1 : s); }
    if (d->hi - d->lo >= 100) return v + dd * (dd > 2 || dd < -2 ? 2 : 1);
    return v + dd;
}

/* ---- the page's knob i: turned by dd (0: only its label and value) */
static const uint8_t RATES[4] = {4, 8, 16, 32};
static uint32_t chord_trk(void) { return genre()->chords ? T_KEY : T_SYN; }
static void cell_def(cell_t *c, uint32_t t)
{
    if (!c->n) { c->n = (uint8_t)(TRK_TYPE[t] == TY_BREAK ? last_note[t] + 1u : last_note[t]); c->v = 12; c->fx = 0; c->fv = 0; }
}
static int knob(uint32_t i, int32_t dd, char *lab, char *val)
{
    uint32_t t = trk();
    track_t *tr = &song.trk[t];
    char *e;
    lab[0] = val[0] = 0;
    switch (page) {
    case PG_TRK: {
        cell_t *c = &P()->c[t][cur_row % plen()];
        int brk = TRK_TYPE[t] == TY_BREAK;
        if (i == 0u) {
            int32_t cur = c->n ? (c->n == NOTE_OFF ? -1 : (int32_t)c->n - (brk ? 1 : 0)) : (int32_t)last_note[t];
            str_cat(lab, "NOTE");
            if (dd) {
                cur = clampi(cur + dd, -1, brk ? 15 : 108);
                cell_def(c, t);
                if (cur < 0) c->n = NOTE_OFF;
                else { c->n = (uint8_t)(brk ? cur + 1 : cur); last_note[t] = (uint8_t)cur; if (!sq_play) audition(t, (uint32_t)cur); }
                touch();
            }
            if (cur < 0) str_cat(val, "OFF");
            else if (brk) { e = str_cat(val, "SLC "); fmt_u(e, (uint32_t)cur + 1u, 2); }
            else note_name((uint32_t)cur, val);
        } else if (i == 1u) {
            str_cat(lab, "VEL");
            if (dd) { cell_def(c, t); c->v = (uint8_t)clampi((int32_t)c->v + dd, 0, 15); touch(); }
            fmt_u(val, c->n ? c->v : 12u, 0);
        } else if (i == 2u) {
            str_cat(lab, "FX");
            if (dd) {
                cell_def(c, t);
                c->fx = (uint8_t)clampi((int32_t)c->fx + dd, 0, FX_N - 1);
                if (c->fx == FX_R && c->fv < 2u) c->fv = 2;
                if (c->fx == FX_G && !c->fv) c->fv = 7;
                if (c->fx == FX_P && !c->fv) c->fv = 8;
                touch();
            }
            str_cat(val, FX_NAME[c->n ? c->fx % FX_N : 0]);
        } else {
            uint32_t fv = c->n ? c->fv : 0u, fx = c->n ? c->fx : 0u;
            str_cat(lab, "AMT");
            if (dd) { cell_def(c, t); c->fv = (uint8_t)clampi((int32_t)c->fv + dd, 0, 15); fv = c->fv; fx = c->fx; touch(); }
            if (fx == FX_R) { e = str_cat(val, "X"); fmt_u(e, fv, 0); }
            else if (fx == FX_G) { e = fmt_u(val, (fv + 1u) * 100u / 16u, 0); str_cat(e, "%"); }
            else if (fx == FX_D) { e = fmt_u(val, fv, 0); str_cat(e, "/16"); }
            else if (fx == FX_P) { e = fmt_s(val, (int32_t)fv - 8); str_cat(e, " ST"); }
            else fmt_u(val, fv, 0);
        }
        return 1;
    }
    case PG_SND: {
        const pd_t *d = snd_pd(t, sub, i);
        int32_t v;
        if (!d) return 0;
        str_cat(lab, d->lab);
        v = pd_get(d, t);
        if (dd) {
            v = d->fmt == F_SOUND || d->fmt == F_RATIO || d->fmt == F_BRK || d->fmt == F_SYN || d->fmt == F_CHORD || d->fmt == F_CTR ? v + dd : step_of(d, v, dd);
            pd_set(d, t, v);
            v = pd_get(d, t);
            touch();
            if (d->fmt == F_SOUND && !sq_play) audition(t, last_note[t]);
        }
        pd_fmt(val, d, d->fmt == F_RATIO ? (int32_t)WB_RATIO[clampi(v, 0, WB_NRATIO - 1)] : v, t);
        return 1;
    }
    case PG_SNG:
        if (i == 0u) {
            str_cat(lab, "SLOT");
            if (dd) { slot_sel = (uint8_t)clampi((int32_t)slot_sel + dd, 1, NSLOT - 1); arm_f = 0xFF; }
            e = fmt_u(val, slot_sel, 2);
            if (!slot_ok[slot_sel]) str_cat(e, " -");
        } else if (i == 1u) {
            str_cat(lab, "GENRE");
            if (dd) { genre_pick = (uint8_t)(((int32_t)genre_pick + dd % (int32_t)WB_NGENRE + (int32_t)WB_NGENRE) % (int32_t)WB_NGENRE); arm_f = 0xFF; }
            fmt_u(val, genre_pick + 1u, 0);
        } else return 0;
        return 1;
    case PG_CHD:
        if (i == 0u) {
            str_cat(lab, "PROG");
            if (dd) { prog_sel = (uint8_t)clampi((int32_t)prog_sel + dd, 0, WB_NPROG - 1); prog_due = 1; prog_ms = fm1_ms; }
            str_cat(val, WB_PROGS[(prog_due ? prog_sel : song.prog) % WB_NPROG].name);
        } else if (i == 1u) {
            uint32_t k = 0;
            str_cat(lab, "LENGTH");
            while (k < 3u && RATES[k] < song.rows) k++;
            if (dd) {
                k = (uint32_t)clampi((int32_t)k + (dd > 0 ? 1 : -1), 0, 3);
                if (RATES[k] != song.rows) { prog_set(song.deg, song.ndeg, RATES[k], song.scale, song.prog); touch(); }
            }
            e = fmt_u(val, song.rows, 0);
            str_cat(e, " ROWS");
        } else if (i == 2u) {
            int16_t *ch = &song.trk[chord_trk()].p[PY_CHORD];
            str_cat(lab, "VOICE");
            if (dd) { *ch = (int16_t)clampi(*ch + dd, 0, WB_NCHORD - 1); touch(); }
            str_cat(val, WB_CHORD_NAME[clampi(*ch, 0, WB_NCHORD - 1)]);
        } else {
            str_cat(lab, "KEY");
            if (dd) { transpose_to((uint32_t)((song.key + 12 + (dd > 0 ? 1 : -1)) % 12)); touch(); }
            str_cat(val, NOTE_NM[song.key % 12u]);
        }
        return 1;
    case PG_GEN: {
        static const char *const L[4] = {"CHANCE", "MUTATE", "DRIFT", "DENS"};
        uint8_t *f[4] = {&tr->chance, &tr->mutate, &tr->drift, &tr->density};
        str_cat(lab, L[i]);
        if (dd) { *f[i] = (uint8_t)clampi((int32_t)*f[i] + dd, 0, 100); touch(); }
        e = fmt_u(val, *f[i], 0);
        str_cat(e, "%");
        return 1;
    }
    case PG_MIX:
        if (i == 1u) {
            str_cat(lab, "PAN");
            if (dd) { tr->pan = (int8_t)clampi(tr->pan + dd, -50, 50); touch(); }
            if (!tr->pan) str_cat(val, "C");
            else { e = str_cat(val, tr->pan < 0 ? "L" : "R"); fmt_u(e, (uint32_t)(tr->pan < 0 ? -tr->pan : tr->pan), 0); }
        } else {
            static const char *const L[4] = {"VOL", "", "VERB", "DELAY"};
            uint8_t *f = i == 0u ? &tr->vol : i == 2u ? &tr->rev : &tr->dly;
            str_cat(lab, L[i]);
            if (dd) { *f = (uint8_t)clampi((int32_t)*f + dd, 0, 100); touch(); }
            e = fmt_u(val, *f, 0);
            str_cat(e, "%");
        }
        return 1;
    case PG_MST:
        if (sub % 3u == 2u) {
            if (i) return 0;
            str_cat(lab, "PUMP");
            if (dd) { song.pump = (uint8_t)clampi(song.pump + dd, 0, 80); touch(); }
            e = fmt_u(val, song.pump, 0);
            str_cat(e, "%");
        } else if (sub % 3u == 0u) {
            if (i == 0u) {
                str_cat(lab, "BPM");
                if (dd) { song.bpm = (uint8_t)clampi(song.bpm + dd, 50, 200); touch(); }
                fmt_u(val, song.bpm, 0);
            } else if (i == 1u) {
                str_cat(lab, "SWING");
                if (dd) { song.swing = (uint8_t)clampi(song.swing + dd, 50, 75); touch(); }
                e = fmt_u(val, song.swing, 0);
                str_cat(e, "%");
            } else if (i == 2u) {
                str_cat(lab, "AGE");
                if (dd) { song.age = (uint8_t)clampi(song.age + dd, 0, 100); touch(); }
                e = fmt_u(val, song.age, 0);
                str_cat(e, "%");
            } else {
                str_cat(lab, "CRUSH");
                if (dd) { song.crush = (uint8_t)clampi(song.crush + dd, 0, 100); touch(); }
                if (song.crush < 6u) str_cat(val, "CLEAN");
                else { e = fmt_u(val, 16u - song.crush / 10u, 0); str_cat(e, " BIT"); }
            }
        } else {
            if (i == 0u) {
                str_cat(lab, "KEY");
                if (dd) { transpose_to((uint32_t)((song.key + 12 + (dd > 0 ? 1 : -1)) % 12)); touch(); }
                str_cat(val, NOTE_NM[song.key % 12u]);
            } else if (i == 1u) {
                str_cat(lab, "SCALE");
                if (dd) { song.scale = (uint8_t)clampi(song.scale + dd, 0, WB_NSCALE - 1); touch(); }
                str_cat(val, WB_SCALE_NAME[song.scale % WB_NSCALE]);
            } else if (i == 2u) {
                str_cat(lab, "SPACE");
                if (dd) { song.space = (uint8_t)clampi(song.space + dd, 0, 100); touch(); }
                e = fmt_u(val, song.space, 0);
                str_cat(e, "%");
            } else {
                str_cat(lab, "VOLUME");
                if (dd) { song.mvol = (uint8_t)clampi(song.mvol + dd, 0, 100); touch(); }
                e = fmt_u(val, song.mvol, 0);
                str_cat(e, "%");
            }
        }
        return 1;
    }
    return 0;
}

/* ================================================================ the F buttons */
static const char *fn_label(uint32_t i, char *buf)
{
    uint32_t t = trk();
    char *e;
    buf[0] = 0;
    switch (page) {
    case PG_TRK:
        if (i == 2u) { e = str_cat(buf, "LEN "); fmt_u(e, plen(), 0); return buf; }
        return (const char *const[]){"CLR", "OFF", "", "DUP"}[i];
    case PG_SND:
        if (i == 0u) { e = str_cat(buf, "PG "); e = fmt_u(e, sub % snd_npages(t) + 1u, 0); *e++ = '/'; fmt_u(e, snd_npages(t), 0); return buf; }
        return (const char *const[]){"", "PLAY", "RAND", "INIT"}[i];
    case PG_SNG:
        if (arm_f == i) return "SURE?";
        return (const char *const[]){"SAVE", "NEWSONG", "LOAD", "DELETE"}[i];
    case PG_CHD: return (const char *const[]){"WRITE", "NEWPROG", "UNDO", "HEAR"}[i];
    case PG_GEN: return (const char *const[]){"FILL", "DICE", "RESTORE", "FILLALL"}[i];
    case PG_MIX:
        if (i == 0u) return song.trk[t].mute ? "UNMUTE" : "MUTE";
        if (i == 1u) return song.trk[t].solo ? "UNSOLO" : "SOLO";
        return (const char *const[]){"", "", "ALL ON", "CLR TRK"}[i];
    case PG_MST:
        if (i == 0u) { e = str_cat(buf, "PG "); e = fmt_u(e, sub % 3u + 1u, 0); str_cat(e, "/3"); return buf; }
        if (i == 2u) return sit_on ? "SIT OFF" : "SIT";
        return (const char *const[]){"", "TAP", "", "CLR PAT"}[i];
    }
    return "";
}

static void sit_set(int on)
{
    sit_on = (uint8_t)on;
    sit_mute = 0;
    if (on) {
        sit_sect = 2;
        sit_left = 4;
        if (!sq_play) sq_req = 1;
        toast("SIT - LET IT PLAY");
    } else toast("SIT OFF");
}
static uint32_t fill_mode(uint32_t t, int alt)
{
    uint32_t m = song_mode(t, alt);
    if ((t == T_BRK && m == WM_OFF) || (TRK_TYPE[t] == TY_SYNTH && m == WK_OFF)) m = default_mode(t);
    return m;
}
static void fill_track(uint32_t pi, uint32_t t)
{
    const char *k, *s, *h;
    song_lanes(rnd(10) < 4u, &k, &s, &h);
    gen_track(pi, t, fill_mode(t, rnd(10) < 4u), song.trk[t].density, k, s, h);
}
static void hear_chord(void)
{
    uint32_t t = chord_trk();
    int32_t m;
    if (!song.ndeg) return;
    m = deg_midi(song.deg[(chd_slot < song.ndeg ? chd_slot : 0u) % 8u]);
    while (m < 55) m += 12;
    while (m > 67) m -= 12;
    ev_push(EV_OFF, t, 0, 0);
    audition(t, (uint32_t)m);
    aud_off_ms = fm1_ms + 900u;
}
static void prog_text(char *o)                  /* "Am F C G" */
{
    uint32_t k;
    char nm[8];
    o[0] = 0;
    for (k = 0; k < song.ndeg && k < 8u; k++) {
        chord_name(song.deg[k], nm, 0);
        if (k) str_cat(o, " ");
        str_cat(o, nm);
    }
}
static void fn_press(uint32_t i)
{
    uint32_t t = trk(), pi = song.cur % NPAT, r;
    pat_t *p = P();
    char buf[40];
    switch (page) {
    case PG_TRK:
        if (i == 0u) { memset(&p->c[t][cur_row % plen()], 0, sizeof(cell_t)); touch(); }
        else if (i == 1u) { cell_set(&p->c[t][cur_row % plen()], NOTE_OFF, 0, 0, 0); cur_row = (uint8_t)((cur_row + 1u) % plen()); touch(); }
        else if (i == 2u) {
            uint32_t L = plen(), n = L == 16u ? 32u : L == 32u ? 64u : 16u, k;
            for (k = 0; k < NT; k++) for (r = L; r < n; r++) p->c[k][r] = p->c[k][r % L];
            p->len = (uint8_t)n;
            if (cur_row >= n) cur_row = 0;
            buf[0] = 0;
            fmt_u(str_cat(buf, "PATTERN LENGTH "), n, 0);
            toast(buf);
            touch();
        } else {
            uint32_t to, k, any;
            for (to = 0; to < NPAT; to++) {
                if (to == pi) continue;
                for (k = 0, any = 0; k < NT && !any; k++) for (r = 0; r < NROW; r++) if (song.pat[to].c[k][r].n) { any = 1; break; }
                if (!any) break;
            }
            if (to >= NPAT) to = (pi + 1u) % NPAT;
            song.pat[to] = *p;
            if (sq_play) sq_cue = (uint8_t)to;
            else song.cur = (uint8_t)to;
            buf[0] = 0;
            {
                char *e = str_cat(buf, "COPIED TO PATTERN ");
                e[0] = (char)('A' + to);
                e[1] = 0;
            }
            toast(buf);
            touch();
        }
        break;
    case PG_SND:
        if (i == 0u) sub = (uint8_t)((sub + 1u) % snd_npages(t));
        else if (i == 1u) audition(t, last_note[t]);
        else if (i == 2u) {
            uint32_t s, k, n = snd_npages(t);
            for (s = 0; s < n; s++)
                for (k = 0; k < 4u; k++) {
                    const pd_t *d = snd_pd(t, s, k);
                    if (d && d->rnd) pd_set(d, t, d->lo + (int32_t)rnd((uint32_t)(d->hi - d->lo) + 1u));
                }
            buf[0] = 0;
            str_cat(str_cat(buf, "RANDOM "), TRK_ID[t]);
            toast(buf);
            touch();
        } else {
            track_t keep = song.trk[t];
            track_defaults(t);
            song.trk[t].vol = keep.vol; song.trk[t].pan = keep.pan; song.trk[t].rev = keep.rev; song.trk[t].dly = keep.dly;
            song.trk[t].mute = keep.mute; song.trk[t].solo = keep.solo; song.trk[t].chance = keep.chance;
            song.trk[t].mutate = keep.mutate; song.trk[t].drift = keep.drift; song.trk[t].density = keep.density;
            toast("SOUND RESET");
            touch();
        }
        break;
    case PG_SNG:
        if (arm_f != i || fm1_ms - arm_ms > 3000u) {
            if (i == 0u && !slot_ok[slot_sel]) goto do_it;   /* (an empty slot: no question) */
            arm_f = (uint8_t)i;
            arm_ms = fm1_ms;
            toast(i == 1u ? "AGAIN: NEW SONG ON A-H" : i == 0u ? "AGAIN: OVERWRITE SLOT" : i == 2u ? "AGAIN: LOAD THE SLOT" : "AGAIN: DELETE THE SLOT");
            return;
        }
    do_it:
        arm_f = 0xFF;
        if (i == 0u) {
            if (sq_play) { toast("STOP FIRST TO SAVE"); return; }
            song.loop = 0;
            toast(song_save(slot_sel) ? "SAVE FAILED" : "SAVED");
        } else if (i == 1u) {
            int was = sq_play;
            song_new(genre_pick);
            cur_row = 0;
            chd_slot = 0;
            prog_sel = song.prog;
            sit_mute = 0;
            if (was) sq_req = 1;
            buf[0] = 0;
            str_cat(str_cat(buf, "NEW SONG "), song.name);
            toast(buf);
            touch();
        } else if (i == 2u) {
            if (song_load(slot_sel)) { toast("NOTHING TO LOAD"); return; }
            genre_pick = song.genre;
            prog_sel = song.prog;
            cur_row = 0;
            toast("LOADED");
            touch();
        } else {
            if (sq_play) { toast("STOP FIRST TO DELETE"); return; }
            store_erase(slot_sel);
            slot_ok[slot_sel] = 0;
            slot_name[slot_sel][0] = 0;
            toast("DELETED");
        }
        break;
    case PG_CHD:
        if (i == 0u) {
            seed_take(pi);
            for (t = T_BAS; t < NT; t++) fill_track(pi, t);
            prog_text(buf);
            toast(buf);
            touch();
        } else if (i == 1u) {
            int8_t d[8];
            uint32_t n = (const uint8_t[]){2, 4, 4, 4}[rnd(4)], k;
            d[0] = 0;
            for (k = 1; k < n; k++) { int8_t x; do x = (int8_t)rnd(7); while (x == d[k - 1u]); d[k] = x; }
            prog_set(d, n, song.rows, song.scale, WB_NPROG - 1u);
            prog_sel = WB_NPROG - 1u;
            prog_text(buf);
            toast(buf);
            touch();
        } else if (i == 2u) toast(prog_undo() ? "PROGRESSION BACK" : "NOTHING TO UNDO");
        else hear_chord();
        break;
    case PG_GEN:
        if (i == 0u) { seed_take(pi); fill_track(pi, t); buf[0] = 0; str_cat(str_cat(buf, "FILLED "), TRK_ID[t]); toast(buf); }
        else if (i == 1u) { seed_take(pi); mutate_track(pi, t, 60); buf[0] = 0; str_cat(str_cat(buf, "MUTATED "), TRK_ID[t]); toast(buf); }
        else if (i == 2u) {
            if (seed_of != pi) { toast("NOTHING TO RESTORE"); return; }
            *p = seed_pat;
            seed_of = 0xFF;
            toast("PATTERN RESTORED");
        } else { seed_take(pi); gen_pattern(pi, pi); toast("NEW PATTERN - RESTORE UNDOES"); }
        touch();
        break;
    case PG_MIX:
        if (i == 0u) song.trk[t].mute ^= 1u;
        else if (i == 1u) song.trk[t].solo ^= 1u;
        else if (i == 2u) { for (r = 0; r < NT; r++) song.trk[r].mute = song.trk[r].solo = 0; }
        else { seed_take(pi); memset(p->c[t], 0, sizeof p->c[t]); toast("TRACK CLEARED - RESTORE ON GEN"); }
        touch();
        break;
    case PG_MST:
        if (i == 0u) sub = (uint8_t)((sub + 1u) % 3u);
        else if (i == 1u) {
            if (ntap && fm1_ms - taps[(ntap - 1u) % 4u] > 2500u) ntap = 0;
            taps[ntap % 4u] = fm1_ms;
            ntap++;
            if (ntap >= 3u) {
                uint32_t n = ntap > 4u ? 4u : ntap, d = (fm1_ms - taps[(ntap - n) % 4u]) / (n - 1u);
                if (d) song.bpm = (uint8_t)clampi((int32_t)(60000u / d), 50, 200);
                touch();
            }
            buf[0] = 0;
            fmt_u(str_cat(buf, "TAP "), ntap, 0);
            toast(buf);
        } else if (i == 2u) sit_set(!sit_on);
        else { seed_take(pi); for (t = 0; t < NT; t++) memset(p->c[t], 0, sizeof p->c[t]); toast("PATTERN CLEARED - RESTORE ON GEN"); touch(); }
        break;
    }
}

/* ================================================================ the pads */
static void chord_pad(uint32_t i)
{
    int8_t d[8];
    uint32_t n = song.ndeg, k;
    char buf[40];
    for (k = 0; k < 8u; k++) d[k] = song.deg[k];
    if (i >= 8u) {
        k = i - 8u;
        if (k > n || k >= 8u) return;
        chd_slot = (uint8_t)k;
        if (k < n) hear_chord();
        return;
    }
    if (i == 7u) { if (n < 2u) return; n--; if (chd_slot >= n) chd_slot = (uint8_t)(n - 1u); }
    else {
        if (chd_slot >= n) { if (n >= 8u) return; d[n++] = (int8_t)i; }
        else d[chd_slot] = (int8_t)i;
    }
    prog_set(d, n, song.rows, song.scale, WB_NPROG - 1u);
    prog_sel = WB_NPROG - 1u;
    touch();
    if (i < 7u) { hear_chord(); if (chd_slot < 7u && chd_slot + 1u == song.ndeg && song.ndeg < 8u) chd_slot++; }
    buf[0] = 0;
    prog_text(str_cat(buf, "CUSTOM "));
    toast(buf);
}
static void rec_note(uint32_t t, uint32_t n)
{
    uint32_t stored = TRK_TYPE[t] == TY_BREAK ? n + 1u : n;
    if (sq_play) {
        pat_t *p = &song.pat[sq_pi % NPAT];
        uint32_t L = p->len ? p->len : 16u, r = sq_row;
        if (sq_frame > sq_rowlen / 2u) r = (r + 1u) % L;
        cell_set(&p->c[t][r], stored, 12, 0, 0);
    } else {
        cell_set(&P()->c[t][cur_row % plen()], stored, 12, 0, 0);
        cur_row = (uint8_t)((cur_row + 1u) % plen());
    }
    touch();
}
static void punch(uint32_t i, int on)
{
    switch (i) {
    case 0: case 1: case 2: case 3:
        if (on) {
            uint32_t n = (const uint8_t[]){4, 2, 1, 1}[i];
            loop_start = (uint8_t)(sq_pos - sq_pos % n);
            punch_loop = (uint8_t)n;
            punch_roll = i == 3u;
        } else { punch_loop = 0; punch_roll = 0; }
        break;
    case 4: if (on) rev_at = (uint8_t)sq_pos; punch_rev = (uint8_t)on; break;
    case 5: punch_half = (uint8_t)on; break;
    case 6: punch_scat = (uint8_t)on; break;
    case 7: punch_dub = (uint8_t)on; break;
    case 8: punch_wash = (uint8_t)on; break;
    case 9: punch_lpf = (uint8_t)on; break;
    }
}
static const char *const PUNCH_NAME[10] = {"ROLL 1/4", "ROLL 1/8", "ROLL 16", "ROLL 32", "REVERSE", "HALF", "SCATTER", "DUB", "WASH", "LPF"};

/* ================================================================ input */
static void ui_input(void)
{
    uint32_t rel, b = fm1_input_edges(&rel), held = fm1_in.buttons, keys = fm1_in.notes, edges = fm1_input_note_edges(), k, i;
    int32_t d;
    uint32_t t = trk();
    edges |= keys & ~prev_keys;
    /* the buttons */
    for (i = 0; i < PG_N; i++)
        if (b & (1u << PG_BTN[i])) {
            if (page == i && (i == PG_SND || i == PG_MST)) sub = (uint8_t)((sub + 1u) % (i == PG_SND ? snd_npages(t) : 3u));
            else if (page != i) { page = (uint8_t)i; sub = 0; arm_f = 0xFF; }
        }
    if (b & (1u << BTN_PLAY)) {
        if (held & (1u << BTN_HOME)) sit_set(!sit_on);
        else if (sq_play) { sq_req = 2; if (sit_on) sit_set(0); }
        else sq_req = 1;
    }
    if (b & (1u << BTN_REC)) { rec_on ^= 1u; toast(rec_on ? "REC - PADS WRITE NOTES" : "REC OFF"); }
    if (b & ((1u << BTN_OCT_DOWN) | (1u << BTN_OCT_UP))) {
        int up = (b >> BTN_OCT_UP) & 1u;
        if (page == PG_SNG) {
            if (up && arm_f != 0xFF) fn_press(arm_f);
            else arm_f = 0xFF;
        } else if (TRK_TYPE[t] == TY_SYNTH) {
            char buf[24];
            pad_oct[t] = (int8_t)clampi(pad_oct[t] + (up ? 1 : -1), -2, 2);
            buf[0] = 0;
            fmt_s(str_cat(buf, "PADS OCTAVE "), pad_oct[t]);
            toast(buf);
        }
    }
    if (b & (1u << BTN_FX)) toast("FX + WHITE: PUNCH-INS");
    if (b & (1u << BTN_SEQ)) toast("SEQ + WHITE 1-8: PATTERNS");
    if (b & (1u << BTN_ARP)) toast("ARP + PAD: NOTE REPEAT");
    if (rel & (1u << BTN_ARP)) rpt_held = 0;
    /* the encoders */
    if ((d = fm1_enc_take(ENC_SELECT)) != 0) {
        if (page == PG_SNG) slot_sel = (uint8_t)clampi((int32_t)slot_sel + d, 1, NSLOT - 1);
        else if (page == PG_CHD) chd_slot = (uint8_t)clampi((int32_t)chd_slot + d, 0, song.ndeg < 8u ? song.ndeg : 7);
        else cur_row = (uint8_t)(((int32_t)cur_row + d % (int32_t)plen() + (int32_t)plen()) % (int32_t)plen());
    }
    if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0) {
        sel_trk = (uint8_t)(((int32_t)sel_trk + d % (int32_t)NT + (int32_t)NT) % (int32_t)NT);
        if (page == PG_SND) sub = 0;
    }
    if ((d = fm1_enc_take(ENC_PRESETS)) != 0) {
        char lab[16], val[24];
        uint32_t s0 = sub;
        if (page == PG_SND) { sub = 0; knob(0, d, lab, val); sub = (uint8_t)s0; }
        else if (page == PG_SNG) knob(1, d, lab, val);
        else if (page == PG_CHD) knob(0, d, lab, val);
        else {
            uint32_t q = (uint32_t)clampi((int32_t)(sq_play && sq_cue < NPAT ? sq_cue : song.cur) + d, 0, NPAT - 1);
            if (sq_play) sq_cue = (uint8_t)q;
            else song.cur = (uint8_t)q;
            if (cur_row >= plen()) cur_row = 0;
        }
    }
    for (i = 0; i < 4u; i++)
        if ((d = fm1_enc_take(ENC_KNOB1 + i)) != 0) {
            char lab[16], val[24];
            knob(i, d, lab, val);
        }
    /* the keys */
    for (k = 0; k < 27u; k++) {
        uint32_t m = 1u << k;
        int down = (edges & m) != 0, up = (prev_keys & m) && !(keys & m);
        int w = WHITE_OF_KEY[k], bl = BLACK_OF_KEY[k];
        if (!down && !up) continue;
        if (bl >= 0) {
            if (!down) continue;
            if (bl < (int)NT) { sel_trk = (uint8_t)bl; if (page == PG_SND) sub = 0; }
            else fn_press((uint32_t)bl - NT);
            continue;
        }
        if (w < 0) continue;
        if (up) {
            if (punch_keys & m) { punch_keys &= ~m; punch((uint32_t)w, 0); }
            if (pad_keys & m) {
                pad_keys &= ~m;
                if (rpt_held == (0x80u | (uint32_t)w)) rpt_held = 0;
                if (!pad_keys && TRK_TYPE[t] == TY_SYNTH) ev_push(EV_OFF, t, 0, 0);
            }
            continue;
        }
        if (held & (1u << BTN_FX)) {
            if (w < 10) { punch_keys |= m; punch((uint32_t)w, 1); toast(PUNCH_NAME[w]); }
            continue;
        }
        if (held & (1u << BTN_SEQ)) {
            if (w < (int)NPAT) {
                char buf[24];
                if (sq_play) sq_cue = (uint8_t)w;
                else song.cur = (uint8_t)w;
                if (cur_row >= plen()) cur_row = 0;
                char *e;
                buf[0] = 0;
                e = str_cat(buf, sq_play ? "NEXT: PATTERN " : "PATTERN ");
                e[0] = (char)('A' + w);
                e[1] = 0;
                toast(buf);
            } else if (w == 8) { song.loop ^= 1u; toast(song.loop ? "LOOP THE PATTERN" : "PLAY THE CHAIN"); touch(); }
            continue;
        }
        if (page == PG_CHD) { chord_pad((uint32_t)w); continue; }
        {
            uint32_t n = pad_note(t, (uint32_t)w);
            pad_keys |= m;
            last_note[t] = (uint8_t)n;
            if ((held & (1u << BTN_ARP)) && sq_play) { rpt_trk = (uint8_t)t; rpt_note = (uint8_t)n; rpt_held = (uint8_t)(0x80u | (uint32_t)w); }
            else ev_push(EV_ON, t, n, 12);
            if (rec_on && !((held & (1u << BTN_ARP)) && sq_play)) rec_note(t, n);
        }
    }
    prev_keys = keys;
}

/* ================================================================ the LEDs */
#if defined(DINGHY_HOST) || defined(KAWA_WASM)
static void leds_flush(void) { host_leds(led_want); }
#else
static void leds_flush(void)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++) {
        uint8_t on = 0, dim = 0;
        for (r = 1; r < 5u; r++) {
            int8_t id = FM1_KEYMAP[r][p];
            if (id < 0 || id > 40) continue;
            if (led_want[id] == 2u) on |= (uint8_t)(1u << r);
            else if (led_want[id] == 1u) dim |= (uint8_t)(1u << r);
        }
        fm1_led[p] = on;
        fm1_led_dim[p] = dim;
    }
}
#endif
static void led(uint32_t id, uint32_t lvl) { if (id < 41u) led_want[id] = (uint8_t)lvl; }
static void ui_leds(void)
{
    uint32_t i, held = fm1_in.buttons, t = trk();
    for (i = 0; i < 41u; i++) led_want[i] = 0;
    for (i = 0; i < PG_N; i++) led(PG_BTN[i], page == i ? 2u : 1u);
    led(BTN_PLAY, sq_play ? ((sq_row & 3u) == 0u ? 2u : 1u) : 0u);
    led(BTN_REC, rec_on ? 2u : 0u);
    led(BTN_FX, held & (1u << BTN_FX) ? 2u : 1u);
    led(BTN_SEQ, held & (1u << BTN_SEQ) ? 2u : 1u);
    led(BTN_ARP, held & (1u << BTN_ARP) ? 2u : 1u);
    led(BTN_OCT_DOWN, pad_oct[t] < 0 ? 2u : 1u);
    led(BTN_OCT_UP, pad_oct[t] > 0 ? 2u : 1u);
    for (i = 0; i < 11u; i++)                   /* black: the tracks, F1..F4 */
        led(KEY_LED(KEY_OF_BLACK[i]), i < NT ? (i == t ? 2u : song.trk[i].mute ? 0u : 1u) : 1u);
    for (i = 0; i < 16u; i++) {
        uint32_t lv = (fm1_in.notes >> KEY_OF_WHITE[i]) & 1u ? 2u : 0u;
        if (held & (1u << BTN_FX)) lv = i < 10u ? ((punch_keys >> KEY_OF_WHITE[i]) & 1u ? 2u : 1u) : 0u;
        else if (held & (1u << BTN_SEQ)) lv = i < NPAT ? (i == (sq_play ? sq_pi : song.cur) ? 2u : 1u) : i == 8u ? (song.loop ? 2u : 1u) : 0u;
        else if (page == PG_CHD) {
            int32_t pd = sq_play ? prog_deg(sq_row) : song.deg[chd_slot < song.ndeg ? chd_slot : 0u];
            int32_t n7 = ((pd % 7) + 7) % 7;
            if (i < 7u) lv = (int32_t)i == n7 ? 2u : 1u;
            else if (i == 7u) lv = 1u;
            else lv = i - 8u == chd_slot ? 2u : i - 8u < song.ndeg ? 1u : 0u;
        } else if (!lv) lv = sq_play && (sq_row % 16u) == i ? 1u : 0u;
        led(KEY_LED(KEY_OF_WHITE[i]), lv);
    }
    leds_flush();
}

/* ================================================================ drawing */
#define TOP_H 16
#define HEAD_Y 16
#define GRID_Y 28
#define ROW_H 12
#define NVIS 13
#define GUT_W 18
#define FOC_W 72
#define NAR_W 25
#define INFO_Y 185
#define KNOB_Y 200
static int32_t lane_x(uint32_t t)
{
    int32_t x = GUT_W;
    uint32_t k;
    for (k = 0; k < t; k++) x += k == trk() ? FOC_W : NAR_W;
    return x;
}
static int32_t lane_w(uint32_t t) { return t == trk() ? FOC_W : NAR_W; }
static void hex2(char *o, uint32_t v)
{
    static const char H[] = "0123456789ABCDEF";
    o[0] = H[(v >> 4) & 15u];
    o[1] = H[v & 15u];
    o[2] = 0;
}
static void cell_text(char *o, const cell_t *c, uint32_t t)   /* "C-4 C R3" (a note, its velocity, its effect) */
{
    static const char H[] = "0123456789ABCDEF";
    if (c->n == NOTE_OFF) { o[0] = o[1] = o[2] = '='; o[3] = 0; return; }
    if (TRK_TYPE[t] == TY_BREAK) { o[0] = 'S'; o[1] = (char)('0' + c->n / 10u); o[2] = (char)('0' + c->n % 10u); }
    else note_name(c->n, o);
    o[3] = ' ';
    o[4] = H[c->v & 15u];
    o[5] = ' ';
    o[6] = c->fx ? FX_CH[c->fx % FX_N] : '.';
    o[7] = c->fx ? H[c->fv & 15u] : '.';
    o[8] = 0;
}
static void draw_top(void)
{
    char s[40];
    uint32_t pc = sq_play ? sq_pi : song.cur;
    g_rect(0, 0, 240, TOP_H, C_PANEL);
    g_rect(0, 0, 32, TOP_H, page == PG_CHD ? C_TEAL : C_BLUE);
    g_text_c(16, 2, PG_NAME[page], C_BG, T_CROP | T_BOLD);
    s[0] = 0;
    str_cat(s, song.name);
    s[13] = 0;
    g_text(36, 2, s, C_TXT, T_CROP);
    s[0] = (char)('A' + pc % NPAT);
    s[1] = 0;
    if (sq_play && sq_cue < NPAT) { s[1] = '>'; s[2] = (char)('A' + sq_cue); s[3] = 0; }
    g_text(148, 2, s, C_AMBER, T_CROP | T_BOLD);
    fmt_u(s, song.bpm, 3);
    g_text(182, 2, s, C_TXT, T_CROP);
    g_icon(214, 4, sq_play ? ICO_PLAY : ICO_STOP, sq_play ? C_AMBER : C_MUTED, 1);
    if (rec_on) g_icon(226, 4, ICO_REC, C_REDC, 1);
    else if (sit_on) g_text(222, 2, "S", C_TEAL, T_CROP | T_BOLD);
    else if (song.loop) g_icon(226, 4, ICO_LOOP, C_MUTED, 1);
}
static void draw_grid(void)
{
    pat_t *p = &song.pat[(sq_play ? sq_pi : song.cur) % NPAT];
    uint32_t L = p->len ? p->len : 16u, t, j, sel = trk();
    int32_t center = sq_play ? sq_row : cur_row;
    char s[16];
    /* the heads */
    g_rect(0, HEAD_Y, 240, 12, C_BG);
    for (t = 0; t < NT; t++) {
        int32_t x = lane_x(t), w = lane_w(t);
        int fl = fm1_ms - flash_ms[t] < 90u;
        uint16_t c = song.trk[t].mute ? C_FAINT : TRK_COL[t];
        if (t == sel) {
            g_rect(x, HEAD_Y, w - 1, 12, fl ? C_AMBER : c);
            s[0] = 0;
            str_cat(s, TRK_ID[t]);
            if (song.trk[t].solo) str_cat(s, " S");
            if (song.trk[t].mute) str_cat(s, " M");
            g_text(x + 3, HEAD_Y, s, C_BG, T_CROP | T_BOLD);
        } else {
            g_rect(x, HEAD_Y, w - 1, 12, g_mix(C_BG, c, fl ? 150 : 50));
            g_text(x, HEAD_Y, TRK_ID[t], fl ? C_TXT : c, T_CROP);
        }
    }
    /* the rows */
    for (j = 0; j < NVIS; j++) {
        int32_t r = center - NVIS / 2 + (int32_t)j, y = GRID_Y + (int32_t)j * ROW_H;
        int play = sq_play && r == sq_row, cur = !sq_play && r == cur_row;
        if (!g_vis(y, ROW_H)) continue;
        if (r < 0 || r >= (int32_t)L) continue;
        if (play) g_rect(0, y, 240, ROW_H, g_mix(C_BG, C_AMBER, 60));
        else if (cur) g_rect(0, y, 240, ROW_H, g_mix(C_BG, C_BLUE, 50));
        else if ((r & 3) == 0) g_rect(0, y, 240, ROW_H, C_BEAT);
        hex2(s, (uint32_t)r);
        g_text(1, y, s, play ? C_AMBER : (r & 3) == 0 ? C_TXT : C_MUTED, T_CROP);
        for (t = 0; t < NT; t++) {
            const cell_t *c = &p->c[t][r];
            int32_t x = lane_x(t);
            if (c->n) cell_text(s, c, t);
            if (!c->n) g_text(t == sel ? x : x + 3, y, t == sel ? "... . .." : "..", C_FAINT, T_CROP);
            else if (t == sel) {
                char a[4] = {s[0], s[1], s[2], 0};
                g_text(x, y, a, c->n == NOTE_OFF ? C_MUTED : C_TXT, T_CROP | (play ? T_BOLD : 0));
                if (c->n != NOTE_OFF) {
                    char v[2] = {s[4], 0};
                    g_text(x + 32, y, v, C_MUTED, T_CROP);
                    g_text(x + 48, y, s + 6, c->fx ? C_TEAL : C_FAINT, T_CROP);
                }
            } else {                            /* narrow: two characters (C4, F#, a slice, X and the velocity) */
                static const char H[] = "0123456789ABCDEF";
                if (c->n == NOTE_OFF) { s[0] = s[1] = '='; }
                else if (TRK_TYPE[t] == TY_BREAK) { s[0] = (char)('0' + c->n / 10u); s[1] = (char)('0' + c->n % 10u); }
                else if (TRK_TYPE[t] != TY_SYNTH) { s[0] = 'X'; s[1] = H[c->v & 15u]; }
                else { note_name(c->n, s); if (s[1] == '-') s[1] = s[2]; }
                s[2] = 0;
                g_text(x + 3, y, s, c->n == NOTE_OFF ? C_MUTED : song.trk[t].mute ? C_FAINT : g_mix(TRK_COL[t], C_TXT, 140), T_CROP);
            }
        }
        if (r == cur_row && sq_play) g_frame(lane_x(sel) - 1, y, FOC_W, ROW_H, C_BLUE);
        else if (cur) g_frame(lane_x(sel) - 1, y, FOC_W, ROW_H, C_BLUE);
    }
}
static void draw_sng(void)
{
    const wb_genre_t *g = &WB_GENRES[genre_pick % WB_NGENRE];
    char s[48], *e;
    uint32_t k, n = 0;
    int32_t y;
    const char *b = g->blurb;
    g_rect(0, TOP_H, 240, 54, C_PANEL);
    s[0] = 0;
    e = str_cat(s, "GENRE ");
    e = fmt_u(e, genre_pick + 1u, 0);
    *e++ = '/';
    fmt_u(e, WB_NGENRE, 0);
    g_text(6, TOP_H + 3, s, C_MUTED, T_CROP);
    s[0] = 0;
    e = fmt_u(s, g->bpm[0], 0);
    *e++ = '-';
    e = fmt_u(e, g->bpm[1], 0);
    str_cat(e, " BPM");
    g_text(234 - g_text_w(s), TOP_H + 3, s, C_MUTED, T_CROP);
    g_text(6, TOP_H + 16, g->name, C_AMBER, T_BOLD);
    for (k = 0; b[k] && n < 29u; k++) s[n++] = b[k];   /* the blurb: one line */
    s[n] = 0;
    g_text(6, TOP_H + 36, s, C_TXT, T_CROP);
    /* the slots */
    y = TOP_H + 58;
    for (k = 0; k < 9u; k++) {
        int32_t sl = clampi((int32_t)slot_sel - 4, 1, (int32_t)NSLOT - 9) + (int32_t)k;
        int32_t yy = y + (int32_t)k * 12;
        if (sl < 1 || sl >= (int32_t)NSLOT) continue;
        if (sl == slot_sel) g_rect(0, yy, 240, 12, g_mix(C_BG, C_BLUE, 70));
        s[0] = 0;
        fmt_u(s, (uint32_t)sl, 2);
        g_text(6, yy, s, sl == slot_sel ? C_TXT : C_MUTED, T_CROP);
        g_text(30, yy, slot_ok[sl] ? slot_name[sl] : "- EMPTY -", slot_ok[sl] ? C_TXT : C_FAINT, T_CROP | (sl == slot_sel ? T_BOLD : 0));
        if (slot_ok[sl] && !memcmp(slot_name[sl], song.name, 15)) g_text(222, yy, "*", C_AMBER, T_CROP);
    }
}
static void draw_chd(void)
{
    uint32_t k, now = song.ndeg ? (uint32_t)((sq_play ? sq_row : cur_row) / (song.rows ? song.rows : 16u)) % song.ndeg : 0u;
    char nm[8], rom[8], s[40], *e;
    g_rect(0, TOP_H, 240, 16, C_PANEL);
    s[0] = 0;
    e = str_cat(s, WB_PROGS[(prog_due ? prog_sel : song.prog) % WB_NPROG].name);
    e = str_cat(e, "  ");
    e = str_cat(e, NOTE_NM[song.key % 12u]);
    e = str_cat(e, " ");
    str_cat(e, WB_SCALE_NAME[song.scale % WB_NSCALE]);
    g_text(6, TOP_H + 2, s, C_TEAL, T_CROP | T_BOLD);
    for (k = 0; k < 8u; k++) {
        int32_t x = 6 + (int32_t)(k % 4u) * 58, y = TOP_H + 22 + (int32_t)(k / 4u) * 50;
        int on = k < song.ndeg, play = on && k == now && sq_play, sel = k == chd_slot;
        g_rrect(x, y, 54, 44, play ? g_mix(C_BG, C_AMBER, 90) : on ? C_PANEL : C_BG);
        if (sel) g_frame(x, y, 54, 44, C_BLUE);
        else if (!on) g_frame(x, y, 54, 44, C_FAINT);
        if (on) {
            chord_name(song.deg[k], nm, rom);
            g_text_c(x + 27, y + 6, nm, play ? C_AMBER : C_TXT, T_BOLD);
            g_text_c(x + 27, y + 26, rom, C_TEAL, T_CROP);
        } else if (k == song.ndeg) g_text_c(x + 27, y + 16, "+", C_MUTED, T_CROP);
    }
    g_text(6, TOP_H + 126, "WHITE 1-7 CHORD  8 DEL", C_MUTED, T_CROP);
    g_text(6, TOP_H + 140, "9-16 PICK A SLOT", C_MUTED, T_CROP);
    s[0] = 0;
    e = fmt_u(s, song.rows, 0);
    str_cat(e, " ROWS A CHORD");
    g_text(6, TOP_H + 154, s, C_MUTED, T_CROP);
}
static void draw_snd(void)
{
    uint32_t t = trk(), k;
    char s[40], *e;
    const char *eng;
    g_rect(0, TOP_H, 240, 40, C_PANEL);
    g_rect(0, TOP_H, 4, 40, TRK_COL[t]);
    s[0] = 0;
    e = str_cat(s, TRK_ID[t]);
    e = str_cat(e, " ");
    str_cat(e, TRK_FULL[t]);
    g_text(10, TOP_H + 3, s, TRK_COL[t], T_CROP | T_BOLD);
    eng = TRK_TYPE[t] == TY_BREAK ? "BREAK" : TRK_TYPE[t] == TY_SYNTH ? (const char *const[]){"FM", "ANALOG", "VOICE"}[clampi(song.trk[t].p[PY_ENG], 0, 2)] : "DRUM";
    g_text(234 - g_text_w(eng), TOP_H + 3, eng, C_MUTED, T_CROP);
    g_text(10, TOP_H + 18, TRK_TYPE[t] == TY_BREAK ? BRK_NAME[clampi(song.trk[t].p[PB_SRC], 0, BRK_N - 1)] : patch_name(t, song.trk[t].patch), C_AMBER, T_BOLD);
    /* the scope */
    g_rect(0, TOP_H + 44, 240, 90, C_BG);
    g_rect(0, TOP_H + 89, 240, 1, C_FAINT);
    for (k = 0; k < 119u; k++) {
        int32_t a = scope[(scope_w + k) % 120u], b2 = scope[(scope_w + k + 1u) % 120u];
        g_line((int32_t)k * 2, TOP_H + 89 - clampi(a / 700, -44, 44), (int32_t)k * 2 + 2, TOP_H + 89 - clampi(b2 / 700, -44, 44), TRK_COL[t]);
    }
    g_text(6, TOP_H + 140, "PRESETS: SOUND  PG: MORE", C_MUTED, T_CROP);
    if (TRK_TYPE[t] == TY_SYNTH) {
        s[0] = 0;
        fmt_s(str_cat(s, "PADS OCTAVE "), pad_oct[t]);
        g_text(6, TOP_H + 154, s, C_MUTED, T_CROP);
    }
}
static void draw_info(void)
{
    char s[48], *e;
    int32_t pd;
    char nm[8], rom[8];
    g_rect(0, INFO_Y - 1, 240, 15, C_BG);
    g_rect(0, INFO_Y - 1, 240, 1, C_FAINT);
    if (toast_ms && fm1_ms - toast_ms < 1800u) { g_text(4, INFO_Y + 1, toast_s, C_AMBER, T_CROP); return; }
    if ((fm1_in.buttons >> BTN_FX) & 1u) { g_text(4, INFO_Y + 1, "PUNCH: ROL ROL ROL ROL REV HLF", C_AMBER, T_CROP); return; }
    s[0] = 0;
    if (page == PG_TRK || page == PG_GEN || page == PG_MIX || page == PG_MST) {
        e = str_cat(s, TRK_ID[trk()]);
        e = str_cat(e, " ");
        str_cat(e, PG_HINT[page]);
    } else str_cat(s, PG_HINT[page]);
    s[22] = 0;
    g_text(4, INFO_Y + 1, s, C_MUTED, T_CROP);
    pd = prog_deg(sq_play ? sq_row : cur_row);
    chord_name(pd, nm, rom);
    s[0] = 0;
    e = str_cat(s, nm);
    e = str_cat(e, " ");
    str_cat(e, rom);
    g_text(236 - g_text_w(s), INFO_Y + 1, s, C_TEAL, T_CROP | T_BOLD);
}
static void draw_knobs(void)
{
    uint32_t i;
    char lab[16], val[24], fb[16];
    g_rect(0, KNOB_Y, 240, 40, C_PANEL);
    for (i = 0; i < 4u; i++) {
        int32_t x = (int32_t)i * 60;
        const char *f;
        if (knob(i, 0, lab, val)) {
            val[7] = 0;
            g_rect(x + 3, KNOB_Y + 4, 3, 8, KNOB_C[i]);
            g_text(x + 9, KNOB_Y + 2, lab, KNOB_C[i], T_CROP);
            g_text(x + 3, KNOB_Y + 14, val, C_TXT, T_CROP | T_BOLD);
        }
        memcpy(knob_lab[i], lab, 16);
        f = fn_label(i, fb);
        if (f[0]) {
            char ff[8];
            uint32_t n = 0;
            while (f[n] && n < 7u) { ff[n] = f[n]; n++; }
            ff[n] = 0;
            g_rrect(x + 2, KNOB_Y + 27, 56, 12, arm_f == i && page == PG_SNG ? C_AMBER : C_FAINT);
            g_text_c(x + 30, KNOB_Y + 27, ff, arm_f == i && page == PG_SNG ? C_BG : C_TXT, T_CROP);
        }
    }
}
static void draw_all(void)
{
    g_rect(0, 0, 240, 240, C_BG);
    draw_top();
    switch (page) {
    case PG_SNG: draw_sng(); break;
    case PG_CHD: draw_chd(); break;
    case PG_SND: draw_snd(); break;
    default: draw_grid(); break;
    }
    draw_info();
    draw_knobs();
}

/* ================================================================ the main loop */
static void set_lang(uint32_t l) { (void)l; }
static int app_sysex(const uint8_t *p, uint32_t n) { (void)p; (void)n; return 0; }
static void app_poll(void) {}
static void pattern_ended(uint32_t pi)          /* MUTATE and DRIFT, then SIT */
{
    uint32_t t;
    for (t = 0; t < NT; t++) {
        track_t *tr = &song.trk[t];
        if (tr->mutate && rnd(100) < tr->mutate) mutate_track(pi, t, 8u + tr->mutate / 3u);
        if (tr->drift && TRK_TYPE[t] == TY_SYNTH && tr->patch < WB_NSYN) {
            int32_t base = WB_SYN[tr->patch][PY_INDEX], span = base * tr->drift / 100 + tr->drift / 10;
            tr->p[PY_INDEX] = (int16_t)clampi(base + (int32_t)rnd((uint32_t)span * 2u + 1u) - span, 0, 80);
        }
    }
    if (sit_on) sit_step(pi);
}
static void app_init(void)
{
    keys_init();
    g_font_set(FN_DOUCE);
    slots_scan();
    if (song_load(0)) {
        song_new(4);
        genre_pick = song.genre;
    } else genre_pick = song.genre;
    sq_req = 0;
    prog_sel = song.prog;
    g_force = 1;
}
static void app_step(void)
{
    uint32_t t;
    ui_input();
    if (sq_end_pi != 0xFF) {
        uint32_t pi = sq_end_pi;
        sq_end_pi = 0xFF;
        pattern_ended(pi);
    }
    if (sq_play && sq_pi != song.cur) { song.cur = sq_pi; if (cur_row >= plen()) cur_row = 0; }
    for (t = 0; t < NT; t++) if (trig_ms[t]) { trig_ms[t] = 0; flash_ms[t] = fm1_ms; }
    if (prog_due && fm1_ms - prog_ms > 900u) { prog_due = 0; if (prog_sel != song.prog || prog_sel == 0u) { prog_pick(prog_sel); touch(); } }
    if (aud_off_t != 0xFF && (int32_t)(fm1_ms - aud_off_ms) >= 0) { ev_push(EV_OFF, aud_off_t, 0, 0); aud_off_t = 0xFF; }
    if (arm_f != 0xFF && fm1_ms - arm_ms > 3000u) arm_f = 0xFF;
    if (cur_row >= plen()) cur_row = 0;
    if (fm1_ms - frame_ms >= 33u) {
        frame_ms = fm1_ms;
        ui_leds();
        g_frame_send(draw_all);
    }
    if (dirty && !sq_play && quiet() && fm1_ms - dirty_ms > 6000u) {   /* the autosave (erasing stalls the sound) */
        if (song_save(0) == 0) dirty = 0;
        else dirty_ms = fm1_ms;
    }
}
static void app_main(void)
{
    app_init();
    for (;;) {
        platform_poll();
        app_step();
    }
}

#if defined(KAWA_WASM)
/* the emulator page: the knobs' labels, the page, the track; the song slots live in the page's memory (it keeps them) */
static uint8_t w_smp_stage[16];
static char w_smp_name[16];
WASM_EXPORT("kawa_page") uint32_t kawa_page(void) { return page; }
WASM_EXPORT("kawa_knob_label") const char *kawa_knob_label(uint32_t i) { return i < 4u ? knob_lab[i] : ""; }
WASM_EXPORT("kawa_track") uint32_t kawa_track(void) { return trk(); }
WASM_EXPORT("kawa_theme") uint32_t kawa_theme(void) { return 0; }
WASM_EXPORT("kawa_sample_buf") uint8_t *kawa_sample_buf(void) { return w_smp_stage; }
WASM_EXPORT("kawa_sample_name") char *kawa_sample_name(void) { return w_smp_name; }
WASM_EXPORT("kawa_sample_size") uint32_t kawa_sample_size(void) { return sizeof w_smp_stage; }
WASM_EXPORT("kawa_sample_commit") void kawa_sample_commit(uint32_t slot, uint32_t len, uint32_t rate) { (void)slot; (void)len; (void)rate; toast("SAMPLES: NOT YET"); }
WASM_EXPORT("kawa_sample_slot_name") const char *kawa_sample_slot_name(uint32_t slot) { (void)slot; return ""; }
WASM_EXPORT("kawa_store") uint8_t *kawa_store(void) { return &host_flash[0][0]; }
WASM_EXPORT("kawa_store_size") uint32_t kawa_store_size(void) { return NSLOT * SLOT_SIZE; }
WASM_EXPORT("kawa_store_gen") uint32_t kawa_store_gen(void) { return store_gen; }
WASM_EXPORT("kawa_store_loaded") void kawa_store_loaded(void)   /* the page put the slots back: the autosave, if any */
{
    slots_scan();
    if (!song_load(0)) { genre_pick = song.genre; prog_sel = song.prog; cur_row = 0; }
    g_force = 1;
}
#endif
