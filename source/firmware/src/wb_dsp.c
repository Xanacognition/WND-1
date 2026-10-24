/* SPDX-License-Identifier: GPL-3.0-only */
/* Wonderblue-OS: the sound (audio interrupt). Voices: the FM kick (pitch drop, sub, click, drive), the snare (two
 * bodies, filtered noise, a clap), the hats (noise and metal), the break's slices, and three synths: 2-operator FM
 * (with a tine and two detuned carriers), analog (1-3 detuned saws / squares through a resonant filter with its own
 * envelope, a swoop into the note, vibrato) and voice (formant filters over a saw: vowels, chops, a wah). The mix:
 * a glued drum bus, the music ducking under the kick (PUMP), sends to a dotted delay and a reverb, tape wow and
 * flutter, hiss and crackle (AGE), a bit crusher, the punch-in filter and a limiter. The sequencer runs here too.
 * Integers only (the chip has no FPU): Q15 samples, envelopes in Q22, phases over 2^32. */

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (refined parabola) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15;
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}
static int32_t sat(int32_t x)                   /* tanh-shaped: small signals pass, big ones round off (Q15) */
{
    int32_t i, f;
    x = clampi(x, -32767, 32767);
    i = (x + 32768) >> 8;
    f = (x + 32768) & 255;
    if (i >= 256) return WB_TANH[256];
    return WB_TANH[i] + (((WB_TANH[i + 1] - WB_TANH[i]) * f) >> 8);
}
static uint32_t semi_q16(int32_t s) { return WB_SEMI[clampi(s + 48, 0, 96)]; }
static uint32_t cents_q16(int32_t c) { return (uint32_t)(65536 + c * 38); }
static uint32_t mulq16(uint32_t inc, uint32_t q) { return (uint32_t)(((uint64_t)inc * q) >> 16); }
static uint32_t hz_inc(uint32_t hz) { return hz * 97391u; }   /* 2^32 / 44100 (hz below 44100) */
static int32_t svf_f(int32_t hz) { return clampi((hz * 4780) >> 10, 40, 24000); }   /* 2 sin(pi fc / fs) ~ 2 pi fc / fs */
#define ENV1 (1 << 22)
static int32_t k_ms(int32_t ms) { return ms < 1 ? 0 : clampi(32768 - 11889 / ms, 0, 32767); }   /* decay per chunk, Q15 */
static int32_t env_to(int32_t e, int32_t target, int32_t k) { return target + (((e - target) >> 7) * k >> 8); }
static int32_t noise(void) { return (int32_t)(xr(&rnd_isr) >> 16) - 32768; }

/* ---- voices */
#define NV 18u
enum { K_FREE, K_KICK, K_SNARE, K_HAT, K_SLICE, K_FM, K_VA, K_VOX };
typedef struct {
    uint8_t kind, trk, stage, open;             /* stage: 0 attack, 1 decay / hold, 2 release */
    int32_t gate;                               /* frames before the release (-1: none) */
    uint32_t age;                               /* frames since the start */
    int32_t env, env_s, vel;                    /* env Q22 at the chunk's start, its step a sample; vel Q15 */
    int32_t e2, e3, e4;                         /* other envelopes (Q15): index / filter / pitch, sub, click */
    uint32_t ph[4], inc[4];
    int32_t s1, s2, s3, s4, s5, s6;             /* filter states */
    int32_t f1, f2, f3, q1, q2, q3;             /* filter coefficients */
    uint32_t pos, step, end;                    /* a slice: the read position and step (Q16), where it ends */
    int8_t dir;
    int16_t q[16], note;
} voice_t;
static voice_t vo[NV];

static voice_t *voice_alloc(uint32_t t)
{
    uint32_t i, best = 0, oldest = 0;
    for (i = 0; i < NV; i++)
        if (vo[i].kind == K_FREE) return &vo[i];
    for (i = 0; i < NV; i++) {                  /* none free: the oldest releasing one, else the oldest */
        uint32_t score = vo[i].age + (vo[i].stage == 2u ? 0x40000000u : 0u) + (vo[i].trk == t ? 0x10000000u : 0u);
        if (score > oldest) { oldest = score; best = i; }
    }
    return &vo[best];
}
static void voices_release(uint32_t t, int32_t fast)
{
    uint32_t i;
    for (i = 0; i < NV; i++)
        if (vo[i].kind != K_FREE && vo[i].trk == t) {
            vo[i].stage = 2;
            vo[i].gate = -1;
            if (fast) vo[i].q[PY_REL] = (int16_t)fast;
        }
}

/* the drums: from the track's sound, the cell's note shifting the tune */
static void start_kick(voice_t *v, const int16_t *p, int32_t tune)
{
    v->kind = K_KICK;
    v->inc[0] = mulq16(4869971u, semi_q16(p[PK_TUNE] + tune));   /* 50 Hz */
    v->q[0] = (int16_t)clampi(p[PK_DECAY], 80, 2000);
    v->q[1] = p[PK_PUNCH];
    v->q[2] = p[PK_DRIVE];
    v->e2 = (3 * 4096 + p[PK_PUNCH] * 6 * 4096 / 100) - 4096;   /* the pitch over the note, Q12 */
    v->e3 = 32767;                              /* the sub's level */
    v->e4 = (int32_t)(22937 * p[PK_PUNCH] / 100);   /* the click */
}
static void start_snare(voice_t *v, const int16_t *p, int32_t tune, int clap)   /* clap: the clap alone */
{
    uint32_t k = semi_q16(p[PS_TUNE] + tune);
    v->kind = K_SNARE;
    v->open = (uint8_t)clap;
    v->inc[0] = mulq16(hz_inc(185), k);
    v->inc[1] = mulq16(hz_inc(330), k);
    v->q[0] = (int16_t)clampi(p[PS_DECAY], 40, 800);
    v->q[1] = p[PS_SNAP];
    v->q[2] = (int16_t)(clap ? 100 : p[PS_CLAP]);
    v->f1 = svf_f(1200 + p[PS_TONE] * 50);
    v->e2 = 19661;                              /* the bodies' pitch over the note (x1.6), Q15 -> decays to 0 */
    v->e3 = 32767;
}
static void start_hat(voice_t *v, const int16_t *p, int32_t tune, int open)
{
    uint32_t k = semi_q16(p[PH_TUNE] + tune);
    v->kind = K_HAT;
    v->open = (uint8_t)open;
    v->inc[0] = mulq16(hz_inc(3170), k);        /* the metal: two rung against each other, and a third */
    v->inc[1] = mulq16(hz_inc(4950), k);
    v->inc[2] = mulq16(hz_inc(7230), k);
    v->q[0] = (int16_t)(open ? clampi(p[PH_DECAY] * 5, 250, 900) : clampi(p[PH_DECAY], 15, 400));
    v->q[1] = p[PH_METAL];
    v->f1 = svf_f(clampi((5000 + p[PH_TONE] * 40) * (int32_t)(k >> 6) / 1024, 3000, 5600));
}
static void start_slice(voice_t *v, const int16_t *p, uint32_t slice, int rev, int32_t pitch, int32_t gate_frames, int retrig)
{
    uint32_t src = (uint32_t)clampi(p[PB_SRC], 0, BRK_N - 1), sl = BRK_LEN / 16u, len;
    uint32_t rate = semi_q16(p[PB_PITCH] + pitch);
    if (punch_half) rate >>= 1;
    v->kind = K_SLICE;
    v->q[0] = (int16_t)src;
    v->step = mulq16(32768u, rate);             /* 22050 Hz data at 44100: half a sample a frame */
    len = sl * (uint32_t)clampi(p[PB_GATE], 10, 200) / 100u;
    if (retrig && gate_frames > 0) {
        uint32_t lim = (uint32_t)(((uint64_t)gate_frames * v->step) >> 16);
        if (lim < len) len = lim;
    }
    slice %= 16u;
    if (len > BRK_LEN - 2u) len = BRK_LEN - 2u;
    if (rev) {
        v->dir = -1;
        v->pos = (uint32_t)(((slice + 1u) * sl - 1u) << 16);
        v->end = v->pos >= (len << 16) ? v->pos - (len << 16) : 0;
    } else {
        v->dir = 1;
        v->pos = (slice * sl) << 16;
        v->end = v->pos + (len << 16);
        if ((v->end >> 16) > BRK_LEN - 2u) v->end = (BRK_LEN - 2u) << 16;
    }
}
/* the synths: q is the sound (WB_SYN's 16 values) */
static void start_synth(voice_t *v, const int16_t *q, uint32_t note, int32_t vel, int32_t glide_bend)
{
    uint32_t base = WB_INC[note & 127u], i, n;
    memcpy(v->q, q, sizeof v->q);
    v->note = (int16_t)note;
    v->kind = q[PY_ENG] == 1 ? K_VA : q[PY_ENG] == 2 ? K_VOX : K_FM;
    if (v->kind == K_FM) {
        int32_t det = q[PY_DET] > 0 ? q[PY_DET] / 2 : 0;
        v->inc[0] = (uint32_t)(((uint64_t)base * ((uint32_t)clampi(q[PY_RATIO], 25, 1400) * 65536u / 100u)) >> 16);   /* the modulator */
        v->inc[1] = mulq16(base, cents_q16(-det));
        v->inc[2] = mulq16(base, cents_q16(det));
        v->inc[3] = base * 14u;                 /* the tine */
        v->open = (uint8_t)(q[PY_DET] > 0);
        v->e2 = clampi(q[PY_INDEX], 0, 120) * (45 + vel * 55 / 32768) / 100 * 2086;   /* the index, phase units / 32768 / 10 */
        v->e4 = q[PY_TINE] * 125;               /* the tine's index */
    } else {
        n = (uint32_t)clampi(q[PY_UNI], 1, 3);
        for (i = 0; i < n; i++) {
            int32_t c = n > 1u ? ((int32_t)i * 2 - (int32_t)(n - 1u)) * q[PY_DET] / (int32_t)(n - 1u) : 0;
            v->inc[i] = mulq16(base, cents_q16(c));
        }
        v->open = (uint8_t)n;
        v->e3 = q[PY_GLIDE] ? (int32_t)semi_q16(q[PY_GLIDE] + glide_bend) - 65536 : 0;   /* the swoop, Q16 over 1 */
        if (v->kind == K_VA) {
            int32_t hz = (int32_t)(((uint64_t)base * 44100u) >> 32);
            int32_t b = clampi(hz * (12 + q[PY_INDEX] * 22 / 10) / 10, 60, 5600);
            v->f2 = b;                          /* the cutoff it settles to (Hz) */
            v->e2 = clampi(b * (100 + q[PY_FENV] * 5) / 100, 60, 5600) - b;   /* its envelope (Hz over it) */
            v->q1 = clampi(327680 / clampi(q[PY_RESO], 5, 200), 1600, 45000);
        } else {
            uint32_t vw = q[PY_VOWEL] >= 5 && q[PY_VOWEL] != 6 ? rnd_isr % 5u : (uint32_t)clampi(q[PY_VOWEL], 0, 4);
            if (q[PY_VOWEL] == 6) vw = 3;       /* WAH: from OH towards AH */
            v->f1 = svf_f(WB_VOWEL[vw][0]); v->f2 = svf_f(WB_VOWEL[vw][1]); v->f3 = svf_f(WB_VOWEL[vw][2]);
            v->q1 = 4096; v->q2 = 2979; v->q3 = 2520;   /* Q 8, 11, 13 */
        }
    }
}
static void voice_go(voice_t *v, uint32_t t, int32_t vel, int32_t gate)
{
    v->trk = (uint8_t)t;
    v->stage = 0;
    v->age = 0;
    v->gate = gate;
    v->env = 0;
    v->env_s = 0;
    v->vel = vel;
    v->ph[0] = v->ph[1] = v->ph[2] = v->ph[3] = 0;
    v->s1 = v->s2 = v->s3 = v->s4 = v->s5 = v->s6 = 0;
}

/* the control rate: envelopes, pitch moves, filters' cutoffs (every CH frames) */
static void voice_ctl(voice_t *v)
{
    int32_t e = v->env, ne = e;
    switch (v->kind) {
    case K_KICK: {
        int32_t dec = v->q[0];
        ne = v->age < 66u ? ENV1 : env_to(e, 0, k_ms(dec / 6));
        v->e2 = (v->e2 >> 7) * k_ms(12 + v->q[1] * 12 / 100) >> 8;
        v->e3 = (v->e3 * k_ms(dec * 115 / 690)) >> 15;
        v->e4 = (v->e4 * k_ms(3)) >> 15;
        if ((int32_t)v->age > dec * 44 + 2000) v->kind = K_FREE;
        break;
    }
    case K_SNARE: {
        int32_t dec = v->q[0];
        ne = v->age < 66u ? ENV1 : env_to(e, 0, k_ms(dec / 6));
        v->e2 = (v->e2 * k_ms(6)) >> 15;
        v->e3 = (v->e3 * k_ms((70 + dec * 3 / 10) / 6)) >> 15;
        if ((int32_t)v->age > dec * 44 + 4000) v->kind = K_FREE;
        break;
    }
    case K_HAT:
        ne = v->age < 33u ? ENV1 : env_to(e, 0, k_ms(v->q[0] / 6));
        if ((int32_t)v->age > v->q[0] * 44 + 1000) v->kind = K_FREE;
        break;
    case K_SLICE: {
        uint32_t left = v->dir > 0 ? (v->end > v->pos ? (v->end - v->pos) >> 16 : 0) : (v->pos > v->end ? (v->pos - v->end) >> 16 : 0);
        int32_t lev = left < 264u ? (int32_t)(left * (ENV1 / 264)) : ENV1;   /* 12 ms out */
        if (v->age < 44u) lev = lev < ENV1 / 2 ? lev : ENV1 / 2;
        if (v->stage == 2u) lev = env_to(e, 0, k_ms(2));
        ne = lev;
        if (!left || (v->stage == 2u && ne < 4096)) v->kind = K_FREE;
        break;
    }
    default: {                                  /* the synths: ADSR */
        int32_t sus = clampi(v->q[PY_SUS], 0, 100) * (ENV1 / 100);
        if (v->gate >= 0) { v->gate -= (int32_t)CH; if (v->gate < 0) v->stage = 2; }
        if (v->stage == 0u) {
            ne = e + 1521729 / clampi(v->q[PY_ATT], 2, 6000);
            if (ne >= ENV1) { ne = ENV1; v->stage = 1; }
        } else if (v->stage == 1u) ne = env_to(e, sus, k_ms(v->q[PY_DEC] / 3 + 1));
        else {
            ne = env_to(e, 0, k_ms(v->q[PY_REL] / 4 + 1));
            if (ne < 2048) { v->kind = K_FREE; ne = 0; }
        }
        if (v->kind == K_FM) {
            v->e2 = (v->e2 * k_ms(v->q[PY_DEC] / 3 + 1) + ((v->e2 * (20 + v->q[PY_SUS] / 2) / 100) * (32768 - k_ms(v->q[PY_DEC] / 3 + 1)))) >> 15;
            v->e4 = (v->e4 * k_ms(30)) >> 15;
        } else {
            v->e3 = (v->e3 * k_ms(50 + (v->q[PY_GLIDE] < 0 ? -v->q[PY_GLIDE] : v->q[PY_GLIDE]) * 6)) >> 15;
            if (v->kind == K_VA) {
                int32_t hz = v->f2 + v->e2;
                v->e2 = (v->e2 * k_ms(v->q[PY_DEC] / 3 + 1)) >> 15;
                v->f1 = svf_f(hz);
            }
        }
    }
    }
    v->env_s = (ne - e) / (int32_t)CH;
}
static uint32_t lfo_ph;                         /* vibrato: one 5.6 Hz LFO for every voice */
static int32_t blep(uint32_t t, uint32_t dt)    /* PolyBLEP: what smooths a step at phase 0 (Q15; 32-bit division only) */
{
    uint32_t d = (dt >> 16) | 1u, x;
    if (t < dt) {
        x = ((t >> 16) << 15) / d;
        if (x > 32768u) x = 32768u;
        return -(int32_t)(((32768u - x) * (32768u - x)) >> 15);
    }
    if (t > 0u - dt) {
        x = (((0u - t) >> 16) << 15) / d;
        if (x > 32768u) x = 32768u;
        return (int32_t)(((32768u - x) * (32768u - x)) >> 15);
    }
    return 0;
}
static int32_t voice_render(voice_t *v)         /* one sample, Q15 */
{
    int32_t s = 0, a;
    uint32_t i;
    v->env += v->env_s;
    a = v->env >> 7;                            /* Q15 */
    switch (v->kind) {
    case K_KICK: {
        uint32_t inc = (uint32_t)(((uint64_t)v->inc[0] * (uint32_t)(4096 + v->e2)) >> 12);
        int32_t body, sub, cl;
        v->ph[0] += inc;
        v->ph[1] += v->inc[0];
        body = sine(v->ph[0]);
        sub = (sine(v->ph[1]) * v->e3) >> 15;
        cl = (noise() * v->e4) >> 15;
        v->s1 += (cl - v->s1) >> 1;             /* the click, softened */
        s = ((body * a) >> 15) + ((sub * 18000) >> 15) + v->s1;
        s = sat((s * (100 + v->q[2] * 7)) / 400) ;
        break;
    }
    case K_SNARE: {
        uint32_t m = (uint32_t)(32768 + v->e2);
        int32_t t1, t2, x, hp, body, tail;
        v->ph[0] += (uint32_t)(((uint64_t)v->inc[0] * m) >> 15);
        v->ph[1] += (uint32_t)(((uint64_t)v->inc[1] * m) >> 15);
        t1 = (int32_t)(v->ph[0] >> 16); t1 = (t1 < 32768 ? t1 : 65535 - t1) * 2 - 32767;
        t2 = (int32_t)(v->ph[1] >> 16); t2 = (t2 < 32768 ? t2 : 65535 - t2) * 2 - 32767;
        body = (((t1 * 14745 + t2 * 8192) >> 15) * (36045 - v->q[1] * 164)) >> 15;
        body = v->open ? 0 : (body * v->e3) >> 15;
        x = noise() >> 1;                       /* the noise through a band-pass, then a high-pass */
        v->s1 += (v->f1 * v->s2) >> 15;
        hp = x - v->s1 - v->s2;
        v->s2 += (v->f1 * hp) >> 15;
        v->s3 += (v->s2 - v->s3) >> 3;
        tail = ((v->s2 - v->s3) * a) >> 15;
        tail = (tail * (40 + v->q[1] * 60 / 100)) / (v->open ? 160 : 64);
        s = body + tail;
        if (v->q[2]) {                          /* the clap: bursts at 0, 9, 19 ms, then its tail */
            uint32_t g = v->age;
            int32_t ce;
            if (g < 397u) ce = 32767 - (int32_t)((g % 397u) * 70);
            else if (g < 838u) ce = 32767 - (int32_t)(((g - 397u) % 441u) * 70);
            else if (g < 1235u) ce = 32767 - (int32_t)(((g - 838u) % 397u) * 70);
            else ce = a * 6 / 10;
            if (ce < 0) ce = 0;
            v->s4 += (v->f1 * 3 / 4 * v->s5) >> 15;
            v->s5 += ((v->f1 * 3 / 4) * (x - v->s4 - (v->s5 >> 1))) >> 15;
            s += (((v->s5 * ce) >> 15) * v->q[2]) / 90;
        }
        break;
    }
    case K_HAT: {
        int32_t m = 0, x;
        if (v->q[1] > 5) {                      /* sines rung together: metal without the squares' grit */
            for (i = 0; i < 3u; i++) v->ph[i] += v->inc[i];
            m = ((sine(v->ph[0]) * sine(v->ph[1])) >> 15) + (sine(v->ph[2]) >> 1);
            m = (m * v->q[1] * 12) / 100;
        }
        x = ((noise() * (100 - v->q[1] * 6 / 10)) / 100) + m;
        v->s1 += (v->f1 * (x - v->s1)) >> 15;   /* a one-pole low-pass: what is left is the high-pass */
        s = (((x - v->s1) >> 1) * a) >> 15;
        break;
    }
    case K_SLICE: {
        const uint8_t *d = BRK_DATA[v->q[0]];
        uint32_t k = v->pos >> 16, f = (v->pos >> 1) & 0x7FFFu;
        int32_t x0 = BRK_ULAW[d[k]], x1 = BRK_ULAW[d[k + 1u]];
        s = x0 + (((x1 - x0) * (int32_t)f) >> 15);
        s = (s * a) >> 15;
        if (v->dir > 0) v->pos += v->step;
        else v->pos = v->pos > v->step ? v->pos - v->step : 0;
        break;
    }
    case K_FM: {
        int32_t m, off, c, tn;
        v->ph[0] += v->inc[0];
        m = sine(v->ph[0]);
        off = (int32_t)((uint32_t)((m >> 2) * (v->e2 >> 2)) << 4);
        if (v->e4 > 64) { v->ph[3] += v->inc[3]; tn = sine(v->ph[3]); off += (int32_t)((uint32_t)((tn >> 2) * (v->e4 >> 2)) << 4); }
        v->ph[1] += v->inc[1];
        c = sine(v->ph[1] + (uint32_t)off);
        if (v->open) { v->ph[2] += v->inc[2]; c = (c + sine(v->ph[2] + (uint32_t)off)) >> 1; }
        s = (((c * a) >> 15) * 11141) >> 15;
        break;
    }
    case K_VA: case K_VOX: {
        int32_t x = 0, vib = 0;
        uint32_t n = v->open ? v->open : 1u, gm = (uint32_t)(65536 + v->e3);
        if (v->q[PY_VIB]) {
            int32_t ramp = v->age > 11025u ? 32767 : (int32_t)(v->age * 2);
            vib = ((sine(lfo_ph) >> 4) * ((v->q[PY_VIB] * ramp) >> 15)) >> 11;   /* cents */
        }
        for (i = 0; i < n; i++) {
            uint32_t inc = v->inc[i], ph;
            if (v->e3 || vib) inc = mulq16(mulq16(inc, gm), cents_q16(vib));
            ph = v->ph[i] += inc;
            switch (v->q[PY_WAVE]) {            /* the saw and the square band-limited (PolyBLEP): no fizz */
            case 1: x += (ph < 0x80000000u ? 26000 : -26000) + ((blep(ph, inc) - blep(ph + 0x80000000u, inc)) * 26000 >> 15); break;
            case 2: { int32_t t = (int32_t)(ph >> 16); x += (t < 32768 ? t : 65535 - t) * 2 - 32767; break; }
            case 3: x += sine(ph); break;
            default: x += (int32_t)(ph >> 16) - 32768 - blep(ph, inc); break;
            }
        }
        x = x / (int32_t)n;
        if (v->kind == K_VA) {
            int32_t hp;
            v->s1 += (v->f1 * v->s2) >> 15;
            hp = x - v->s1 - ((v->q1 * v->s2) >> 15);
            v->s2 += (v->f1 * hp) >> 15;
            v->s1 = clampi(v->s1, -60000, 60000);
            v->s2 = clampi(v->s2, -60000, 60000);
            s = (((v->s1 * a) >> 15) * 9830) >> 15;
        } else {
            int32_t hp, src = x >> 3;
            if (v->q[PY_VOWEL] == 6 && !(v->age & 15u) && v->age < (uint32_t)v->q[PY_DEC] * 26u) {
                v->f1 += (svf_f(WB_VOWEL[0][0]) - v->f1) / 24; v->f2 += (svf_f(WB_VOWEL[0][1]) - v->f2) / 24;
            }
            v->s1 += (v->f1 * v->s2) >> 15; hp = src - v->s1 - ((v->q1 * v->s2) >> 15); v->s2 += (v->f1 * hp) >> 15;
            v->s3 += (v->f2 * v->s4) >> 15; hp = src - v->s3 - ((v->q2 * v->s4) >> 15); v->s4 += (v->f2 * hp) >> 15;
            v->s5 += (v->f3 * v->s6) >> 15; hp = src - v->s5 - ((v->q3 * v->s6) >> 15); v->s6 += (v->f3 * hp) >> 15;
            x = (v->s2 * 3 + v->s4 * 2 + v->s6) >> 2;
            s = (((clampi(x, -40000, 40000) * a) >> 15) * 15000) >> 15;
        }
        break;
    }
    default: return 0;
    }
    v->age++;
    return (s * v->vel) >> 15;
}

/* ---- triggers (the sequencer's and the pads') */
static uint32_t row_frames(void) { uint32_t f = 661500u / (song.bpm ? song.bpm : 100u); return punch_half ? f * 2u : f; }
static void counter_voice(uint32_t t, uint32_t n, int32_t vel, int32_t gate);
typedef struct {
    int32_t wait;
    uint8_t trk, note, rev, retrig;
    int8_t pitch;
    int16_t vel;
    int32_t gate;
    uint8_t ctr, pad[3];
} pend_t;
#define NPEND 48u
static pend_t pend[NPEND];
static void pend_add(int32_t wait, uint32_t t, uint32_t note, int32_t vel, int32_t gate, int rev, int32_t pitch, int retrig, int ctr)
{
    uint32_t i;
    for (i = 0; i < NPEND; i++)
        if (!pend[i].note && !pend[i].ctr) {
            pend_t *e = &pend[i];
            e->wait = wait; e->trk = (uint8_t)t; e->note = (uint8_t)(note ? note : 0); e->vel = (int16_t)vel; e->gate = gate;
            e->rev = (uint8_t)rev; e->pitch = (int8_t)pitch; e->retrig = (uint8_t)retrig; e->ctr = (uint8_t)ctr;
            if (!e->note) e->note = 0, e->ctr = (uint8_t)(ctr | 2);   /* (slice 0: kept by the flag) */
            return;
        }
}
static int32_t duck = 32768;                    /* PUMP: the music's gain, Q15 */
static void trigger(uint32_t t, uint32_t n, int32_t vel, int32_t gate, int rev, int32_t pitch, int retrig)
{
    const int16_t *p = song.trk[t].p;
    voice_t *v;
    trig_ms[t] = 1u;
    switch (TRK_TYPE[t]) {
    case TY_BREAK: {
        uint32_t i;
        for (i = 0; i < NV; i++) if (vo[i].kind == K_SLICE && vo[i].trk == t) vo[i].stage = 2;   /* one slice at a time */
        v = voice_alloc(t);
        voice_go(v, t, vel, -1);
        start_slice(v, p, n, rev, pitch, gate, retrig);
        break;
    }
    case TY_KICK:
        v = voice_alloc(t);
        voice_go(v, t, vel, -1);
        start_kick(v, p, (int32_t)n - 60);
        if (song.pump && !song.trk[t].mute) { int32_t d = 32768 - (int32_t)song.pump * (vel >> 7) * 3; if (d < duck) duck = d; }
        break;
    case TY_DRUMS: {                            /* the note says which drum: snare, clap, closed or open hat */
        uint32_t i, kd = drm_kind(n);
        if (kd == DK_HAT || kd == DK_OPEN) {
            if (kd == DK_HAT) for (i = 0; i < NV; i++) if (vo[i].kind == K_HAT && vo[i].trk == t && vo[i].open) vo[i].q[0] = 20;   /* closed chokes open */
            v = voice_alloc(t);
            voice_go(v, t, (vel * 12000) >> 15, -1);   /* (the hats sit under the snare) */
            start_hat(v, p, drm_tune(n), kd == DK_OPEN);
        } else {
            v = voice_alloc(t);
            voice_go(v, t, vel, -1);
            start_snare(v, p, drm_tune(n), kd == DK_CLAP);
        }
        break;
    }
    default: {
        int32_t notes[5];
        uint32_t k, cnt, m = (uint32_t)clampi((int32_t)n + 12 * p[PY_OCT], 12, 120);
        cnt = chord_notes((int32_t)m, (uint32_t)clampi(p[PY_CHORD], 0, WB_NCHORD - 1), notes);
        if (t == T_BAS) voices_release(t, 12);
        for (k = 0; k < cnt; k++) {
            v = voice_alloc(t);
            voice_go(v, t, (int32_t)(vel * (cnt == 1u ? 32767 : cnt == 2u ? 23170 : cnt == 3u ? 18918 : 16384) >> 15), gate);
            start_synth(v, p, (uint32_t)clampi(notes[k], 12, 120), vel, 0);
        }
        if (t == T_KEY && p[PY_CTR]) counter_voice(t, m, vel, gate);
    }
    }
}
static void counter_voice(uint32_t t, uint32_t n, int32_t vel, int32_t gate)
{
    const int16_t *p = song.trk[t].p;
    int32_t d = deg_at((int32_t)n), sh = (int32_t)n - deg_midi(d), m = (int32_t)n, wait = 0, g = gate, rf = (int32_t)row_frames();
    int32_t lvl = clampi(p[PY_CTRL], 0, 100) * 327;
    switch (p[PY_CTR]) {
    case 1: m = deg_midi(d + 2) + sh; break;
    case 2: m = deg_midi(d - 2) + sh; break;
    case 3: m = deg_midi(d - 5) + sh; break;
    case 4: m = (int32_t)n + 12; break;
    case 5: m = deg_midi(d + 2) + sh; wait = 3 * rf; g = gate < 2 * rf ? gate : 2 * rf; lvl = lvl * 8 / 10; break;
    case 6: m = (int32_t)n + 12; wait = 2 * rf; g = gate < 3 * rf ? gate : 3 * rf; lvl = lvl * 7 / 10; break;
    }
    if (wait) { pend_add(wait, t, (uint32_t)clampi(m, 12, 120), (vel * lvl) >> 15, g, 0, 0, 0, 1); return; }
    {
        voice_t *v = voice_alloc(t);
        voice_go(v, t, (vel * lvl) >> 15, g);
        start_synth(v, WB_SYN[clampi(p[PY_CTRP], 0, WB_NSYN - 1)], (uint32_t)clampi(m, 12, 120), vel, 0);
    }
}

/* ---- the sequencer */
static uint32_t sq_frame, sq_rowlen, sq_pos;    /* frames into the row, the row's length; the real row (punch-ins map it) */
static uint8_t loop_start, rev_at;
static uint32_t gate_rows(const pat_t *p, uint32_t t, uint32_t r)
{
    uint32_t k, L = p->len ? p->len : 16u;
    for (k = r + 1u; k < L; k++) if (p->c[t][k].n) return k - r;
    return L - r;
}
/* the meter: sq_pos counts the steps played; a bar of 16 rows plays METER_STEPS of them (fewer: its end is left
 * out; more: its last beat again) */
static uint32_t meter_steps(void) { return METER_STEPS[(punch_meter ? punch_meter : song.meter) % NMETER]; }
static uint32_t steps_of(uint32_t L)            /* the steps a pattern of L rows takes */
{
    uint32_t B = meter_steps(), rest = L % 16u;
    return B == 16u ? L : (L / 16u) * B + (rest < B ? rest : B);
}
static uint32_t row_of(uint32_t pos)            /* the row a step plays */
{
    uint32_t B = meter_steps(), k;
    if (B == 16u) return pos;
    k = pos % B;
    if (k >= 16u) k = 12u + (k - 16u) % 4u;
    return (pos / B) * 16u + k;
}
static uint32_t play_row(uint32_t L)
{
    uint32_t n = steps_of(L), vp = sq_pos % n;
    if (punch_rev) vp = (uint32_t)((((int32_t)rev_at * 2 - (int32_t)sq_pos) % (int32_t)n + (int32_t)n) % (int32_t)n);
    if (punch_loop) vp = (loop_start + ((sq_pos - loop_start) % punch_loop)) % n;
    return row_of(vp) % L;
}
static void row_fire(void)
{
    const pat_t *p = &song.pat[sq_pi % NPAT];
    uint32_t L = p->len ? p->len : 16u, r = play_row(L), t, rf = sq_rowlen, solo = 0;
    int32_t sw = (r & 1u) ? (int32_t)(rf * (uint32_t)clampi(song.swing - 50, 0, 25) * 2u / 100u) : 0;
    sq_row = (uint8_t)r;
    sq_rowtick++;
    for (t = 0; t < NT; t++) if (song.trk[t].solo) solo = 1;
    for (t = 0; t < NT; t++) {
        const cell_t *c = &p->c[t][r];
        const track_t *tr = &song.trk[t];
        uint32_t n = c->n, hits, h;
        int32_t vel, wait, prob;
        if ((rpt_held & 0x80u) && rpt_trk == t) {   /* note repeat: the held pad, every row */
            pend_add(sw, t, rpt_note + (TRK_TYPE[t] == TY_BREAK ? 1u : 0u), 26000, (int32_t)rf * 9 / 10, 0, 0, 1, 0);
            if (rec_on) { cell_t *w = &song.pat[sq_pi % NPAT].c[t][r]; w->n = (uint8_t)(rpt_note + (TRK_TYPE[t] == TY_BREAK ? 1u : 0u)); w->v = 12; w->fx = 0; w->fv = 0; }
        }
        if (!n) continue;
        if (n == NOTE_OFF) { voices_release(t, 0); continue; }
        if (tr->mute || (solo && !tr->solo)) continue;
        prob = tr->chance;
        if (c->fx == FX_G) prob = prob * (c->fv + 1) / 16;
        if (prob < 100 && (int32_t)((xr(&rnd_isr) >> 8) % 100u) >= prob) continue;
        if (TRK_TYPE[t] == TY_BREAK && punch_scat) { n = 1u + (xr(&rnd_isr) >> 8) % 16u; }
        wait = sw + (c->fx == FX_D ? (int32_t)(rf * c->fv / 16u) : 0);
        hits = (c->fx == FX_R && c->fv > 1u ? c->fv : 1u) * (punch_roll ? 2u : 1u);
        if (TRK_TYPE[t] == TY_BREAK && punch_scat && (xr(&rnd_isr) & 3u) == 0u) hits = 2u + (xr(&rnd_isr) >> 8) % 3u;
        vel = (int32_t)c->v * 2184;
        for (h = 0; h < hits; h++) {
            int32_t hv = hits > 1u ? vel - (int32_t)(vel * 4 / 10 * h / hits) : vel;
            int32_t g = hits > 1u ? (int32_t)(rf / hits) : (int32_t)(gate_rows(p, t, r) * rf);
            pend_add(wait + (int32_t)(rf * h / hits), t, n, hv, g, c->fx == FX_B || punch_rev, c->fx == FX_P ? (int32_t)c->fv - 8 : 0, hits > 1u, 0);
        }
    }
}
static void seq_chunk(void)
{
    uint32_t i;
    if (sq_req == 1u) {                         /* PLAY */
        sq_req = 0;
        sq_play = 1;
        sq_pos = 0;
        sq_frame = 0;
        sq_chainpos = 0;
        sq_pi = song.nchain > 1u && !song.loop ? song.chain[0] : song.cur;
        sq_rowlen = row_frames();
        row_fire();
    } else if (sq_req == 2u) {
        sq_req = 0;
        sq_play = 0;
        for (i = 0; i < NV; i++) if (vo[i].kind >= K_FM) vo[i].stage = 2, vo[i].gate = -1;
        for (i = 0; i < NPEND; i++) pend[i].note = 0, pend[i].ctr = 0;
    }
    for (i = 0; i < NPEND; i++) {               /* what is due in this chunk */
        pend_t *e = &pend[i];
        if (!e->note && !e->ctr) continue;
        e->wait -= (int32_t)CH;
        if (e->wait > 0) continue;
        if (e->ctr & 1u) {
            voice_t *v = voice_alloc(e->trk);
            voice_go(v, e->trk, e->vel, e->gate);
            start_synth(v, WB_SYN[clampi(song.trk[e->trk].p[PY_CTRP], 0, WB_NSYN - 1)], e->note, e->vel, 0);
        } else
            trigger(e->trk, (e->ctr & 2u) ? 0u : (TRK_TYPE[e->trk] == TY_BREAK ? e->note - 1u : e->note), e->vel, e->gate, e->rev, e->pitch, e->retrig);
        e->note = 0;
        e->ctr = 0;
    }
    if (!sq_play) return;
    sq_frame += CH;
    if (sq_frame >= sq_rowlen) {
        const pat_t *p = &song.pat[sq_pi % NPAT];
        uint32_t L = p->len ? p->len : 16u;
        sq_frame -= sq_rowlen;
        sq_rowlen = row_frames();
        if (++sq_pos >= steps_of(L)) {          /* the end of the pattern: the next one */
            sq_pos = 0;
            sq_end_pi = sq_pi;
            if (sq_cue < NPAT) { sq_pi = sq_cue; sq_cue = 0xFF; }
            else if (song.nchain > 1u && !song.loop) { sq_chainpos = (uint8_t)((sq_chainpos + 1u) % song.nchain); sq_pi = song.chain[sq_chainpos] % NPAT; }
            if (punch_rev) rev_at = 0;
        }
        row_fire();
    }
}

/* ---- the master */
#define DLY_N 16384u
#define TAPE_N 512u
static int16_t dly_buf[DLY_N] POOL;
static uint32_t dly_w;
static int32_t dly_lp, dly_out, dly_o0, sd_prev, sr_prev;
static int16_t cmb[4][700] POOL, apb[2][360];
static const uint16_t CMB_LEN[4] = {557, 593, 641, 677}, AP_LEN[2] = {225, 341};
static uint16_t cmb_i[4], ap_i[2];
static int32_t cmb_lp[4], rev_l, rev_r, rev_l0, rev_r0;
static int16_t tape[2][TAPE_N];
static uint32_t tape_w, wow_ph, flut_ph;
static int32_t tone_l, tone_r, lpf_l1, lpf_l2, lpf_r1, lpf_r2, lpf_f = 24000, lim_g = 32768, dbus_env, dbus_g = 32768;
static int32_t trk_lp[NT], trk_g[NT], trk_gt[NT];
static const uint8_t TRK_MULT[NT] = {50, 40, 70, 160, 180, 200};   /* the mix's balance (%): the kick at half, the synths up */
static uint32_t half;
static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, k, t;
    while (evq_r != evq_w) {                    /* the pads */
        ev_t *e = &evq[evq_r % EVQ];
        if (e->type == EV_ON) trigger(e->trk, e->note, e->vel * 2184, TRK_TYPE[e->trk] == TY_SYNTH ? 44100 * 8 : 22050, 0, 0, 0);
        else if (e->type == EV_OFF) voices_release(e->trk, 0);
        else if (e->type == EV_ALLOFF) { for (k = 0; k < NV; k++) vo[k].kind = K_FREE; }
        evq_r++;
    }
    for (i = 0; i < n; i += CH) {
        int32_t rev_fb = 21000 + song.space * 70, dly_fb = punch_dub ? 26214 : 13763;
        uint32_t dly_len = (uint32_t)clampi((int32_t)(row_frames() * 3u / 2u), 1000, DLY_N - 1);
        seq_chunk();
        for (k = 0; k < NV; k++) if (vo[k].kind != K_FREE) voice_ctl(&vo[k]);
        lfo_ph += 545400u * CH;                 /* 5.6 Hz */
        for (t = 0; t < NT; t++) {              /* the tracks' gains glide (mutes, SIT's sections) */
            int32_t v = song.trk[t].vol, on = !((sit_mute >> t) & 1u);
            trk_gt[t] = on ? v * v * TRK_MULT[t] / 1000 * 328 / 10 : 0;   /* vol² x the track's balance, Q15 */
            trk_g[t] += (trk_gt[t] - trk_g[t]) >> 4;
        }
        duck += (32768 - duck) >> 6;
        lpf_f += ((punch_lpf ? 1800 : 24000) - lpf_f) >> 5;
        if (lim_g < 32768) lim_g += (32768 - lim_g) >> 7;
        dbus_g = dbus_env > 14000 ? (int32_t)(((uint32_t)(14000 + (dbus_env - 14000) / 4) << 8) / ((uint32_t)dbus_env >> 7)) : 32768;
        for (k = 0; k < CH && i + k < n; k++) {
            int32_t acc[NT], dl = 0, dr = 0, ml = 0, mr = 0, sr = 0, sd = 0, L, R, x;
            for (t = 0; t < NT; t++) acc[t] = 0;
            for (t = 0; t < NV; t++) if (vo[t].kind != K_FREE) acc[vo[t].trk] += voice_render(&vo[t]);
            for (t = 0; t < NT; t++) {
                int32_t s = acc[t], pan = song.trk[t].pan, l, r;
                if (t == T_BRK) {               /* the break's tone */
                    int32_t a = clampi((int32_t)(180 * 4780 >> 10) * (1 + song.trk[t].p[PB_TONE] * song.trk[t].p[PB_TONE] / 90), 600, 32767);
                    trk_lp[t] += ((s - trk_lp[t]) * a) >> 15;
                    s = song.trk[t].p[PB_TONE] >= 98 ? s : trk_lp[t];
                }
                s = ((clampi(s, -98000, 98000) >> 2) * trk_g[t]) >> 13;
                l = (s * (50 - pan)) / 50;
                r = (s * (50 + pan)) / 50;
                if (t < 3u) { dl += l; dr += r; } else { ml += l; mr += r; }
                sr += (s * song.trk[t].rev) / 100;
                sd += (s * song.trk[t].dly) / 100;
            }
            x = (dl < 0 ? -dl : dl) + (dr < 0 ? -dr : dr);   /* the drum bus: glued, rounded */
            dbus_env += x > dbus_env ? (x - dbus_env) >> 4 : (x - dbus_env) >> 11;
            dl = sat((int32_t)(((int64_t)dl * dbus_g) >> 16)) * 2;   /* (twice the headroom before it rounds off) */
            dr = sat((int32_t)(((int64_t)dr * dbus_g) >> 16)) * 2;
            ml = (ml * duck) >> 15;
            mr = (mr * duck) >> 15;
            L = dl + ml;
            R = dr + mr;
            if (punch_dub) sd += (L + R) / 3;
            if (punch_wash) sr += (L + R) * 4 / 10;
            {
            int even = !(half++ & 1u), sd2 = (sd + sd_prev) >> 1, sr2 = (sr + sr_prev) >> 1;
            sd_prev = sd;
            sr_prev = sr;
            if (even) {                         /* the delay and the reverb at 22 kHz: what goes in averaged, what */
                int32_t in = sd2, dv, rin = sr2 >> 2, cl = 0, cr = 0, ap;   /* comes out in between (no buzz) */
                dly_o0 = dly_out;
                rev_l0 = rev_l;
                rev_r0 = rev_r;
                uint32_t j;
                dv = dly_buf[(dly_w + DLY_N - dly_len) % DLY_N];
                dly_lp += (dv - dly_lp) >> 1;
                dly_buf[dly_w] = (int16_t)clampi(in / 2 + ((dly_lp * dly_fb) >> 15), -32767, 32767);
                dly_w = (dly_w + 1u) % DLY_N;
                dly_out = dly_lp;
                for (j = 0; j < 4u; j++) {
                    int32_t y = cmb[j][cmb_i[j]];
                    cmb_lp[j] += (y - cmb_lp[j]) * 3 >> 3;
                    cmb[j][cmb_i[j]] = (int16_t)clampi(rin + ((cmb_lp[j] * rev_fb) >> 15), -32767, 32767);
                    if (++cmb_i[j] >= CMB_LEN[j]) cmb_i[j] = 0;
                    if (j & 1u) cr += y; else cl += y;
                }
                for (j = 0; j < 2u; j++) {
                    int32_t b = apb[j][ap_i[j]];
                    ap = cl - (b >> 1);
                    apb[j][ap_i[j]] = (int16_t)clampi(ap, -32767, 32767);
                    cl = b + (ap >> 1);
                    if (++ap_i[j] >= AP_LEN[j]) ap_i[j] = 0;
                }
                rev_l = cl;
                rev_r = (cr + cl) >> 1;
            }
            {
                int32_t dq = even ? (dly_o0 + dly_out) >> 1 : dly_out, rl = even ? (rev_l0 + rev_l) >> 1 : rev_l, rr = even ? (rev_r0 + rev_r) >> 1 : rev_r;
                L += dq + ((rl * song.space) / 60);
                R += dq + ((rr * song.space) / 60);
            }
            }
            if (song.age) {                     /* tape: wow and flutter, hiss and crackle, a darker top */
                uint32_t d, f;
                int32_t a0, a1;
                wow_ph += 40852u;
                flut_ph += 612915u;
                tape[0][tape_w] = (int16_t)clampi(L, -32767, 32767);
                tape[1][tape_w] = (int16_t)clampi(R, -32767, 32767);
                d = (uint32_t)(200 << 8) + (uint32_t)(((sine(wow_ph) * song.age) >> 7) * 115 / 256 + ((sine(flut_ph) * song.age) >> 7) * 12 / 256);
                f = d & 255u;
                d >>= 8;
                a0 = tape[0][(tape_w + TAPE_N - d) % TAPE_N]; a1 = tape[0][(tape_w + TAPE_N - d - 1u) % TAPE_N];
                L = a0 + (((a1 - a0) * (int32_t)f) >> 8);
                a0 = tape[1][(tape_w + TAPE_N - d) % TAPE_N]; a1 = tape[1][(tape_w + TAPE_N - d - 1u) % TAPE_N];
                R = a0 + (((a1 - a0) * (int32_t)f) >> 8);
                tape_w = (tape_w + 1u) % TAPE_N;
                {
                    int32_t a = clampi(32767 - song.age * 230, 6000, 32767), hs = (noise() * song.age) >> 13;
                    if ((xr(&rnd_isr) & 0xFFFFu) < (uint32_t)song.age / 20u) hs += (noise() * song.age) >> 10;
                    tone_l += ((L - tone_l) * a) >> 15;
                    tone_r += ((R - tone_r) * a) >> 15;
                    L = tone_l + hs;
                    R = tone_r + hs;
                }
            }
            if (lpf_f < 23000) {                /* the punch-in filter */
                int32_t f = svf_f(lpf_f > 5600 ? 5600 : lpf_f), hp;
                lpf_l1 += (f * lpf_l2) >> 15; hp = L - lpf_l1 - (lpf_l2 >> 2); lpf_l2 += (f * hp) >> 15;
                lpf_r1 += (f * lpf_r2) >> 15; hp = R - lpf_r1 - (lpf_r2 >> 2); lpf_r2 += (f * hp) >> 15;
                L = clampi(lpf_l1, -90000, 90000); R = clampi(lpf_r1, -90000, 90000);
                lpf_l1 = L; lpf_r1 = R;
            }
            if (song.crush > 5u) {
                int32_t m = ~((1 << (song.crush / 10u)) - 1);
                L &= m; R &= m;
            }
            {                                   /* the limiter */
                int32_t pk = L < 0 ? -L : L, pr = R < 0 ? -R : R;
                if (pr > pk) pk = pr;
                if (((int64_t)pk * lim_g >> 15) > 31000) lim_g = (int32_t)((31000u << 15) / (uint32_t)(pk ? pk : 1));
                L = (int32_t)(((int64_t)L * lim_g) >> 15) * song.mvol / 100;
                R = (int32_t)(((int64_t)R * lim_g) >> 15) * song.mvol / 100;
            }
            out[2u * (i + k)] = clampi(L, -32767, 32767);
            out[2u * (i + k) + 1u] = clampi(R, -32767, 32767);
            if (!((i + k) & 7u)) { scope[scope_w % 120u] = (int16_t)((L + R) >> 1); scope_w++; }
        }
    }
}
