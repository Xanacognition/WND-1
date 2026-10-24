/* SPDX-License-Identifier: GPL-3.0-only */
/* KAWA: the live effects (as a Pocket Operator's: hold, and it happens; let go, and it stops) and the
 * master: the echo, the reverb, the master's effect, the clip. Runs in the audio interrupt.
 *
 * The live effects play on a bus: the whole mix (TARGET: MASTER) or one track (TARGET: a track; the
 * others go on untouched). The buffer effects (loops, stutters, scratches, reverse, tape stop, dive,
 * freeze, the roll) read a capture of the last 0.74 s of that bus. They lock to the beat: a loop starts
 * on the last 1/16 (1/12, 1/32, 1/64) and the first pass is the live sound, then it repeats. Only one
 * plays at a time: the last one pressed. The others stack: crush, decimate, filters, gates, ring, echo,
 * distortion, wobble, vinyl; octave, unison, 6/8, mutes are applied by the voices and the sequencer. */

#define CAPN 32768u                             /* capture: frames (0.74 s), L | R << 16 */
#define CAPM (CAPN - 1u)
#define ECHON 16384u                            /* (at 22050 Hz: 0.74 s) */
static uint32_t cap[CAPN] POOL;
static int16_t echo_buf[ECHON] POOL;
static uint32_t afr;                            /* frames since boot */
static uint32_t echo_w;
static int32_t echo_lp;

#define BF_MASK ((1u << LF_LOOP16) | (1u << LF_LOOP12) | (1u << LF_LOOP32) | (1u << LF_LOOP64) | (1u << LF_STUT4) | \
                 (1u << LF_STUT3) | (1u << LF_SCR) | (1u << LF_SCRF) | (1u << LF_REV) | (1u << LF_ROLLUP) | \
                 (1u << LF_TAPE) | (1u << LF_DIVE) | (1u << LF_FREEZE))
static struct {
    uint8_t act, id;                            /* a buffer effect plays; which (LF_...) */
    uint8_t frozen, k;                          /* the capture is held; repeats so far */
    uint32_t S, L, E, t;                        /* segment start, length, the press (absolute frames); frames since */
    uint32_t p, v;                              /* varispeed: position in the segment (Q16 frames), speed (Q16) */
    int32_t mix, mix_to;                        /* 0 live .. 32767 effect */
} bf;
static uint32_t lfx_prev;
static uint8_t lpf_note = 127, hpf_note = 0;    /* the sweeps, MIDI note of the cutoff (the FX page draws them) */
static int32_t svf_lp[2], svf_bp[2];
static uint32_t lfo_ph, ring_ph;
static int32_t hold_l, hold_r, gate_g = 32767;
static uint32_t hold_n;
static uint8_t lv_target;                       /* the bus the live effects play on now: 0 the mix, 1..8 a track */

static uint32_t cap_rd(uint32_t a) { return cap[a & CAPM]; }
static int32_t cap_l(uint32_t w) { return (int16_t)(w & 0xFFFFu); }
static int32_t cap_r(uint32_t w) { return (int16_t)(w >> 16); }

/* frames back to the last grid line of g frames (the sequencer's beat when it plays) */
static uint32_t since_grid(uint32_t g) { return g ? g_beatpos % g : 0u; }

static void bf_engage(uint32_t id)
{
    uint32_t beat = g_rowlen * 4u, L;
    if (beat > CAPN - 4096u)
        beat = (CAPN - 4096u) & ~3u;
    bf.act = 1;
    bf.id = (uint8_t)id;
    bf.E = afr;
    bf.t = 0;
    bf.k = 0;
    bf.p = 0;
    bf.v = 65536u;
    bf.frozen = 0;
    switch (id) {
    case LF_LOOP16: L = beat / 4u; break;
    case LF_LOOP12: L = beat / 3u; break;
    case LF_LOOP32: L = beat / 8u; break;
    case LF_LOOP64: L = beat / 16u; break;
    case LF_STUT4: L = beat / 4u; break;
    case LF_STUT3: L = beat / 3u; break;
    case LF_ROLLUP: L = beat / 2u; break;
    case LF_DIVE: L = beat / 4u; break;
    case LF_SCR: case LF_SCRF: L = beat / 4u; break;
    case LF_REV: L = beat / 2u; break;
    case LF_FREEZE: L = 2048u; break;
    default: L = beat; break;
    }
    if (L < 64u)
        L = 64u;
    bf.L = L;
    if (id == LF_SCR || id == LF_SCRF || id == LF_REV || id == LF_FREEZE) {
        bf.S = afr - L;                         /* what just played */
        bf.frozen = 1;
    } else if (id == LF_TAPE)
        bf.S = afr - 1u;                        /* (one frame behind: the next one is not captured yet) */
    else
        bf.S = afr - since_grid(L);             /* the last grid line: the first pass is the live sound */
    bf.mix_to = 32767;
}

/* once per block: which effects sound, what the voices must know */
static void fx_block(uint32_t on)
{
    uint32_t newb = on & ~lfx_prev & BF_MASK, t;
    uint8_t mel = 0, beat = 0;
    if (lfx_target != lv_target) {              /* another bus: what it held is not this one's */
        lv_target = lfx_target <= NTRK ? lfx_target : 0u;
        bf.act = 0;
        bf.mix = bf.mix_to = 0;
        svf_lp[0] = svf_lp[1] = svf_bp[0] = svf_bp[1] = 0;
    }
    lfx_on = on;
    lv_mask = (uint8_t)(lv_target ? 1u << (lv_target - 1u) : 0xFFu);
    lv_transpose = (on >> LF_OCTUP & 1u ? 12 : 0) - (on >> LF_OCTDN & 1u ? 12 : 0);
    lv_uni = on >> LF_UNILO & 1u ? 2u : on >> LF_UNI & 1u ? 1u : 0u;
    for (t = 0; t < NTRK; t++) {
        if (IS_TONAL(t)) mel |= (uint8_t)(1u << t);
        else beat |= (uint8_t)(1u << t);
    }
    lv_mute = (uint8_t)(prj.mute | live_mute | (on >> LF_NOMEL & 1u ? mel : 0u) | (on >> LF_NOBEAT & 1u ? beat : 0u));
    if (newb) {                                 /* a new buffer effect: the lowest new one takes over */
        uint32_t id = 0;
        while (!(newb >> id & 1u))
            id++;
        if (bf.act)                             /* (a switch: no fade, the new one starts from the live sound) */
            bf.mix = 0;
        bf_engage(id);
    } else if (bf.act && bf.mix_to && !(on >> bf.id & 1u)) {
        uint32_t rest = on & BF_MASK, id = 0;
        if (rest) {
            while (!(rest >> id & 1u))
                id++;
            bf_engage(id);
        } else
            bf.mix_to = 0;                      /* let go: back to live */
    }
    lfx_prev = on;
}

/* the buffer effect's sample for the frame now (L, R) */
static void bf_read(int32_t *l, int32_t *r)
{
    uint32_t t = bf.t, w = 0, L = bf.L;
    int32_t g = 32767;
    switch (bf.id) {
    case LF_LOOP16: case LF_LOOP12: case LF_LOOP32: case LF_LOOP64: case LF_STUT4: case LF_STUT3: case LF_ROLLUP: {
        uint32_t rel = afr - bf.S, p;
        if (bf.id == LF_ROLLUP) {               /* the loop halves every beat: 1/8, 1/16, 1/32, 1/64 */
            uint32_t beats = (afr - bf.E) / (g_rowlen * 4u + 1u), Lr = L >> (beats > 3u ? 3u : beats);
            if (Lr < 64u) Lr = 64u;
            L = Lr;
        }
        if (rel < bf.L && !bf.frozen)           /* the first pass: live */
            return;
        bf.frozen = 1;
        p = rel % L;
        w = cap_rd(bf.S + p);
        if (p >= L - 64u) {                     /* the seam: fade into what came before the start */
            uint32_t w2 = cap_rd(bf.S + p - L);
            int32_t a = (int32_t)(p - (L - 64u)) * 512;
            *l = (cap_l(w) * (32767 - a) + cap_l(w2) * a) >> 15;
            *r = (cap_r(w) * (32767 - a) + cap_r(w2) * a) >> 15;
        } else {
            *l = cap_l(w);
            *r = cap_r(w);
        }
        if (bf.id == LF_STUT4 || bf.id == LF_STUT3) {   /* each repeat softer, back to full every beat */
            uint32_t n = bf.id == LF_STUT4 ? 4u : 3u, k = (rel / L) % n;
            g = 32767 - (int32_t)(k * (24000u / n));
            *l = (*l * g) >> 15;
            *r = (*r * g) >> 15;
        }
        return;
    }
    case LF_REV: {
        uint32_t p = t % L;
        int32_t e = (int32_t)(p < 128u ? p : (L - p < 128u ? L - p : 128u)) * 256;
        w = cap_rd(bf.S + L - 1u - p);
        *l = (cap_l(w) * e) >> 15;
        *r = (cap_r(w) * e) >> 15;
        return;
    }
    case LF_FREEZE: {                           /* two heads, triangle windows: a smooth drone of 46 ms */
        uint32_t p1 = t % L, p2 = (t + L / 2u) % L, w2;
        int32_t a1 = (int32_t)(p1 < L / 2u ? p1 : L - p1) * 32767 / (int32_t)(L / 2u), a2 = 32767 - a1;
        w = cap_rd(bf.S + p1);
        w2 = cap_rd(bf.S + p2);
        *l = (cap_l(w) * a1 + cap_l(w2) * a2) >> 15;
        *r = (cap_r(w) * a1 + cap_r(w2) * a2) >> 15;
        return;
    }
    case LF_SCR: case LF_SCRF: {                /* back and forth over the last 1/16, one stroke per 1/8 (1/16) */
        uint32_t per = (bf.id == LF_SCR ? g_rowlen * 2u : g_rowlen) + 1u, ph = (t % per) * (0xFFFFFFFFu / per);
        int32_t c = 32767 - sine(ph + 0x40000000u), off = (int32_t)(((int64_t)L * (c + 32767)) >> 16);
        uint32_t a = bf.S + L - 1u - (uint32_t)(off < (int32_t)L ? off : (int32_t)L - 1);
        uint32_t w2 = cap_rd(a - 1u);
        w = cap_rd(a);
        *l = (cap_l(w) + cap_l(w2)) >> 1;
        *r = (cap_r(w) + cap_r(w2)) >> 1;
        return;
    }
    case LF_TAPE: case LF_DIVE: {               /* varispeed through the capture */
        uint32_t ip, fr;
        int32_t a0, b0, a1, b1;
        if (bf.id == LF_TAPE) {                 /* slows to a stop over a beat */
            uint32_t dv = 65536u / (g_rowlen * 4u + 1u) + 1u;
            bf.v = bf.v > dv ? bf.v - dv : 0u;
            bf.p += bf.v;
            if (!bf.v) { *l = *r = 0; return; }
        } else {                                /* each repeat of a 1/16, lower */
            if (bf.k == 0 && (afr - bf.S) < L && !bf.frozen) return;
            bf.frozen = 1;
            bf.p += bf.v;
            if ((bf.p >> 16) >= L) {
                bf.p -= L << 16;
                bf.k++;
                bf.v = (uint32_t)(((uint64_t)bf.v * 56000u) >> 16);
                if (bf.v < 16384u) bf.v = 16384u;
            }
        }
        ip = bf.S + (bf.p >> 16);
        fr = (bf.p >> 1) & 0x7FFFu;
        w = cap_rd(ip);
        a0 = cap_l(w); b0 = cap_r(w);
        w = cap_rd(ip + 1u);
        a1 = cap_l(w); b1 = cap_r(w);
        *l = a0 + (((a1 - a0) * (int32_t)fr) >> 15);
        *r = b0 + (((b1 - b0) * (int32_t)fr) >> 15);
        return;
    }
    }
}

/* the live bus for CHUNK frames, in place; E: the echo's input (ECHO adds the bus to it) */
static void fx_live(int32_t *L, int32_t *R, int32_t *E)
{
    uint32_t i, on = lfx_on;
    int32_t q = 13000, f = 0, mode = 0;
    /* the filter: the LPF sweep wins, then the HPF, then wobble, then vinyl's top cut */
    if (on >> LF_LPF & 1u) { if (lpf_note > 34u) lpf_note--; }
    else if (lpf_note < 127u) lpf_note = (uint8_t)(lpf_note + 4u > 127u ? 127u : lpf_note + 4u);
    if (on >> LF_HPF & 1u) { if (hpf_note < 100u) hpf_note++; }
    else if (hpf_note) hpf_note = (uint8_t)(hpf_note > 4u ? hpf_note - 4u : 0u);
    lfo_ph += (0xFFFFFFFFu / (g_rowlen + 1u)) * (CHUNK / 2u);          /* two cycles a beat (1/8) */
    if (lpf_note < 127u) { mode = 1; f = SVF_F[lpf_note]; q = 9000; }
    else if (hpf_note) { mode = 2; f = SVF_F[hpf_note + 20u]; q = 12000; }
    else if (on >> LF_WOB & 1u) { mode = 1; f = SVF_F[64 + (sine(lfo_ph) * 28 >> 15)]; q = 7000; }
    else if (on >> LF_VINYL & 1u) { mode = 1; f = SVF_F[98]; q = 20000; }
    for (i = 0; i < CHUNK; i++) {
        int32_t l = clampi(L[i], -65535, 65535), r = clampi(R[i], -65535, 65535);   /* (two full scales: the
                                                 * products below stay inside 32 bits) */
        if (!(bf.act && bf.frozen) || ((afr - (bf.S - 64u)) & CAPM) >= bf.L + 128u)   /* (frozen: all but the */
            cap[afr & CAPM] = (uint32_t)(uint16_t)clampi(l, -32767, 32767) | (uint32_t)clampi(r, -32767, 32767) << 16;
        if (bf.act) {
            int32_t fl = l, fr = r;
            bf_read(&fl, &fr);
            if (bf.mix < bf.mix_to) {           /* 64 frames from live to the effect, and back */
                bf.mix += 512;
                if (bf.mix > bf.mix_to) bf.mix = bf.mix_to;
            } else if (bf.mix > bf.mix_to) {
                bf.mix -= 512;
                if (bf.mix < bf.mix_to) bf.mix = bf.mix_to;
            }
            l += (int32_t)(((int64_t)(fl - l) * bf.mix) >> 15);
            r += (int32_t)(((int64_t)(fr - r) * bf.mix) >> 15);
            bf.t++;
            if (!bf.mix && !bf.mix_to)
                bf.act = 0;
        }
        if (on >> LF_CRUSH & 1u) { l &= ~0x7FF; r &= ~0x7FF; }
        if (on >> LF_DECIM & 1u) {
            if (++hold_n >= 6u) { hold_n = 0; hold_l = l; hold_r = r; }
            l = hold_l; r = hold_r;
        }
        if (mode) {                             /* Chamberlin state variable filter, stereo */
            int32_t hp;
            svf_lp[0] += (int32_t)(((int64_t)f * svf_bp[0]) >> 15);   /* (64-bit products, the state bounded) */
            hp = l - svf_lp[0] - (int32_t)(((int64_t)q * svf_bp[0]) >> 15);
            svf_bp[0] += (int32_t)(((int64_t)f * hp) >> 15);
            l = mode == 1 ? svf_lp[0] : hp;
            svf_lp[1] += (int32_t)(((int64_t)f * svf_bp[1]) >> 15);
            hp = r - svf_lp[1] - (int32_t)(((int64_t)q * svf_bp[1]) >> 15);
            svf_bp[1] += (int32_t)(((int64_t)f * hp) >> 15);
            r = mode == 1 ? svf_lp[1] : hp;
            svf_bp[0] = clampi(svf_bp[0], -200000, 200000);
            svf_bp[1] = clampi(svf_bp[1], -200000, 200000);
            svf_lp[0] = clampi(svf_lp[0], -200000, 200000);
            svf_lp[1] = clampi(svf_lp[1], -200000, 200000);
            l = clampi(l, -65535, 65535);       /* (the ring, the gate and the drive after it stay in 32 bits) */
            r = clampi(r, -65535, 65535);
        } else
            svf_lp[0] = svf_lp[1] = svf_bp[0] = svf_bp[1] = 0;
        if (on >> LF_RING & 1u) {
            int32_t s;
            ring_ph += 21424000u;               /* 220 Hz */
            s = sine(ring_ph);
            l = (l * s) >> 15;
            r = (r * s) >> 15;
        }
        if (on & ((1u << LF_GATE16) | (1u << LF_GATE32))) {
            uint32_t g = on >> LF_GATE16 & 1u ? g_rowlen : g_rowlen / 2u, ph = since_grid(g);
            int32_t tgt = ph < (g * 3u) / 5u ? 32767 : 0;
            gate_g += gate_g < tgt ? 2048 : -2048;
            gate_g = clampi(gate_g, 0, 32767);
            l = (l * gate_g) >> 15;
            r = (r * gate_g) >> 15;
        } else
            gate_g = 32767;
        if (on >> LF_DIST & 1u) {
            l = (sat(l * 5) * 22000) >> 15;
            r = (sat(r * 5) * 22000) >> 15;
        }
        if (on >> LF_VINYL & 1u) {              /* crackle */
            uint32_t x = xr(&rnd_isr);
            if (x < 0x00080000u) { l += 9000; r += 9000; }
            l += (int32_t)(x >> 24) - 128;
            r += (int32_t)(x >> 24) - 128;
        }
        if (on >> LF_ECHO & 1u)
            E[i] += (l + r) >> 1;
        L[i] = l;
        R[i] = r;
        if ((afr & 7u) == 0u) {
            scope[scope_w] = (int16_t)clampi((l + r) >> 1, -32767, 32767);
            if (++scope_w >= 240u) scope_w = 0;
        }
        afr++;
    }
}

/* ---- the reverb: the PlayStation SPU's (psx-spx, "SPU Reverb Formula"), the presets of its library ("SPU Reverb
 * Examples"). A work area of 16-bit samples that the reverb walks through at 22050 Hz, one sample a tick: two
 * reflections a side (the same side, the other side), four combs that read them back, two all-pass filters.
 * The addresses (in the presets: 8-byte units, here samples) are from where it is now; everything wraps in the
 * area. KNOB 4 on SENDS picks the preset, for the song (prj.grv.pad[0]). The tick is every other frame: its input
 * is the mean of two frames, its output is drawn in a line between two ticks (the SPU's 39-tap filter would cost
 * more than the whole reverb) */
#define RVB_NP 7u
enum { RV_DAPF1, RV_DAPF2, RV_VIIR, RV_VCOMB1, RV_VCOMB2, RV_VCOMB3, RV_VCOMB4, RV_VWALL, RV_VAPF1, RV_VAPF2,
       RV_MLSAME, RV_MRSAME, RV_MLCOMB1, RV_MRCOMB1, RV_MLCOMB2, RV_MRCOMB2, RV_DLSAME, RV_DRSAME, RV_MLDIFF, RV_MRDIFF,
       RV_MLCOMB3, RV_MRCOMB3, RV_MLCOMB4, RV_MRCOMB4, RV_DLDIFF, RV_DRDIFF, RV_MLAPF1, RV_MRAPF1, RV_MLAPF2, RV_MRAPF2,
       RV_VLIN, RV_VRIN };
typedef struct {
    const char *name;
    uint32_t bytes;                             /* the work area */
    uint16_t r[32];                             /* 1F801DC0h..1F801DFEh */
} rvb_preset_t;
static const rvb_preset_t RVB[RVB_NP] = {
    {"HALL", 0xADE0u, {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB,
                       0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
                       0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000}},
    {"STUDIO L", 0x6FE0u, {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58,
                           0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
                           0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000}},
    {"STUDIO M", 0x4840u, {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B,
                           0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
                           0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000}},
    {"STUDIO S", 0x1F40u, {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B,
                           0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
                           0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000}},
    {"ROOM", 0x26C0u, {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333,
                       0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                       0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000}},
    {"HALF ECHO", 0x3C00u, {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF,
                            0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
                            0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000}},
    {"SPACE ECHO", 0xF6C0u, {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31,
                             0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
                             0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000}},
};
#define RVB_N (0xF6C0u / 2u)                    /* samples: the largest work area (SPACE ECHO) */
static int16_t rvb[RVB_N] POOL;
static uint32_t rvb_n = 1, rvb_cur, rvb_clear; /* the area now (samples), where the reverb is in it, samples left to clear */
static uint8_t rvb_of = 0xFF;                   /* the preset it is set up for */
static int32_t rvb_v[10];                       /* the volumes, signed */
static uint32_t rvb_a[RV_VLIN];                 /* the addresses, in samples, each in 0 .. rvb_n - 1 */
static uint32_t rvb_m1[4];                      /* mLSAME, mRSAME, mLDIFF, mRDIFF one sample back ("-2" bytes) */
static uint32_t rvb_ap[4];                      /* mLAPF1 - dAPF1, mRAPF1 - dAPF1, mLAPF2 - dAPF2, mRAPF2 - dAPF2 */
static int32_t rvb_vin, rvb_l0, rvb_r0, rvb_l1, rvb_r1, rvb_x;
static uint8_t rvb_ph;

static uint8_t rvb_preset(void) { return (uint8_t)(prj.grv.pad[0] < RVB_NP ? prj.grv.pad[0] : 0u); }

static void rvb_setup(uint32_t k)               /* a new preset: its registers, the area cleared bit by bit */
{
    const rvb_preset_t *p = &RVB[k % RVB_NP];
    uint32_t i, n = p->bytes / 2u;
    rvb_of = (uint8_t)k;
    rvb_n = n > RVB_N ? RVB_N : n;
    for (i = 0; i < 10u; i++) rvb_v[i] = (int16_t)p->r[i];
    for (i = RV_MLSAME; i < RV_VLIN; i++) rvb_a[i] = ((uint32_t)p->r[i] * 4u) % rvb_n;
    rvb_a[RV_DAPF1] = ((uint32_t)p->r[RV_DAPF1] * 4u) % rvb_n;
    rvb_a[RV_DAPF2] = ((uint32_t)p->r[RV_DAPF2] * 4u) % rvb_n;
    rvb_m1[0] = (rvb_a[RV_MLSAME] + rvb_n - 1u) % rvb_n;
    rvb_m1[1] = (rvb_a[RV_MRSAME] + rvb_n - 1u) % rvb_n;
    rvb_m1[2] = (rvb_a[RV_MLDIFF] + rvb_n - 1u) % rvb_n;
    rvb_m1[3] = (rvb_a[RV_MRDIFF] + rvb_n - 1u) % rvb_n;
    rvb_ap[0] = (rvb_a[RV_MLAPF1] + rvb_n - rvb_a[RV_DAPF1]) % rvb_n;
    rvb_ap[1] = (rvb_a[RV_MRAPF1] + rvb_n - rvb_a[RV_DAPF1]) % rvb_n;
    rvb_ap[2] = (rvb_a[RV_MLAPF2] + rvb_n - rvb_a[RV_DAPF2]) % rvb_n;
    rvb_ap[3] = (rvb_a[RV_MRAPF2] + rvb_n - rvb_a[RV_DAPF2]) % rvb_n;
    rvb_vin = (int16_t)p->r[RV_VLIN];
    rvb_cur = 0;
    rvb_clear = RVB_N;
}

static int32_t rv_rd(uint32_t a)
{
    a += rvb_cur;
    return rvb[a >= rvb_n ? a - rvb_n : a];
}
static void rv_wr(uint32_t a, int32_t v)
{
    a += rvb_cur;
    rvb[a >= rvb_n ? a - rvb_n : a] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
#define RVM(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 15))   /* (64-bit: the sums before a product pass 16 bits) */

static void rvb_tick(int32_t in, int32_t *lo, int32_t *ro)   /* one 22050 Hz tick of the SPU's reverb */
{
    int32_t lin = RVM(in, rvb_vin), rin = lin, l, r, m;
    const int32_t iir = rvb_v[RV_VIIR], wall = rvb_v[RV_VWALL];
    m = rv_rd(rvb_m1[0]);                       /* the same side: left to left, right to right */
    rv_wr(rvb_a[RV_MLSAME], RVM(lin + RVM(rv_rd(rvb_a[RV_DLSAME]), wall) - m, iir) + m);
    m = rv_rd(rvb_m1[1]);
    rv_wr(rvb_a[RV_MRSAME], RVM(rin + RVM(rv_rd(rvb_a[RV_DRSAME]), wall) - m, iir) + m);
    m = rv_rd(rvb_m1[2]);                       /* the other side */
    rv_wr(rvb_a[RV_MLDIFF], RVM(lin + RVM(rv_rd(rvb_a[RV_DRDIFF]), wall) - m, iir) + m);
    m = rv_rd(rvb_m1[3]);
    rv_wr(rvb_a[RV_MRDIFF], RVM(rin + RVM(rv_rd(rvb_a[RV_DLDIFF]), wall) - m, iir) + m);
    l = RVM(rvb_v[RV_VCOMB1], rv_rd(rvb_a[RV_MLCOMB1])) + RVM(rvb_v[RV_VCOMB2], rv_rd(rvb_a[RV_MLCOMB2])) +
        RVM(rvb_v[RV_VCOMB3], rv_rd(rvb_a[RV_MLCOMB3])) + RVM(rvb_v[RV_VCOMB4], rv_rd(rvb_a[RV_MLCOMB4]));
    r = RVM(rvb_v[RV_VCOMB1], rv_rd(rvb_a[RV_MRCOMB1])) + RVM(rvb_v[RV_VCOMB2], rv_rd(rvb_a[RV_MRCOMB2])) +
        RVM(rvb_v[RV_VCOMB3], rv_rd(rvb_a[RV_MRCOMB3])) + RVM(rvb_v[RV_VCOMB4], rv_rd(rvb_a[RV_MRCOMB4]));
    m = rv_rd(rvb_ap[0]);                       /* the all-pass filters */
    l -= RVM(rvb_v[RV_VAPF1], m);
    rv_wr(rvb_a[RV_MLAPF1], l);
    l = RVM(clampi(l, -32768, 32767), rvb_v[RV_VAPF1]) + m;
    m = rv_rd(rvb_ap[1]);
    r -= RVM(rvb_v[RV_VAPF1], m);
    rv_wr(rvb_a[RV_MRAPF1], r);
    r = RVM(clampi(r, -32768, 32767), rvb_v[RV_VAPF1]) + m;
    m = rv_rd(rvb_ap[2]);
    l -= RVM(rvb_v[RV_VAPF2], m);
    rv_wr(rvb_a[RV_MLAPF2], l);
    l = RVM(clampi(l, -32768, 32767), rvb_v[RV_VAPF2]) + m;
    m = rv_rd(rvb_ap[3]);
    r -= RVM(rvb_v[RV_VAPF2], m);
    rv_wr(rvb_a[RV_MRAPF2], r);
    r = RVM(clampi(r, -32768, 32767), rvb_v[RV_VAPF2]) + m;
    if (++rvb_cur >= rvb_n) rvb_cur = 0;
    *lo = clampi(l, -32768, 32767);
    *ro = clampi(r, -32768, 32767);
}

static void reverb_chunk(const int32_t *in, int32_t *L, int32_t *R)
{
    uint32_t i, k = rvb_preset();
    if (k != rvb_of) rvb_setup(k);
    if (rvb_clear) {                            /* (a new preset: its area is cleared first, 2048 samples a chunk) */
        uint32_t n = rvb_clear > 2048u ? 2048u : rvb_clear;
        rvb_clear -= n;
        while (n--) rvb[rvb_clear + n] = 0;
        rvb_l0 = rvb_r0 = rvb_l1 = rvb_r1 = 0;
        return;
    }
    for (i = 0; i < CHUNK; i++) {
        int32_t x = in[i] >> 2;                 /* (a quarter in, four times out: headroom in its 16-bit area, and
                                                 * about the level of KAWA's reverb) */
        if (!rvb_ph) {
            rvb_x = x;
        } else {
            rvb_l0 = rvb_l1;
            rvb_r0 = rvb_r1;
            rvb_tick(clampi((rvb_x + x) >> 1, -32768, 32767), &rvb_l1, &rvb_r1);
        }
        rvb_ph ^= 1u;                           /* (the output: a line from the last tick to this one) */
        L[i] += (rvb_ph ? rvb_l0 + ((rvb_l1 - rvb_l0) >> 1) : rvb_l1) * 4;
        R[i] += (rvb_ph ? rvb_r0 + ((rvb_r1 - rvb_r0) >> 1) : rvb_r1) * 4;
    }
}

/* ---- the limiter: the chunk's peak sets the gain (one division a chunk), down at once and ramped over the
 * chunk, back up in ~50 ms: six tracks at full volume stay under the clip instead of crackling */
#define LIM_T 26000
static int32_t lim_g = 32767;
static void limiter(int32_t *L, int32_t *R)
{
    uint32_t i;
    int32_t pk = 0, tgt = 32767, g = lim_g, dg;
    for (i = 0; i < CHUNK; i++) {
        int32_t a = L[i] < 0 ? -L[i] : L[i], b = R[i] < 0 ? -R[i] : R[i];
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    if (pk > LIM_T)
        tgt = (int32_t)(((uint32_t)LIM_T << 15) / (uint32_t)pk);
    if (tgt < g) g = tgt;                       /* attack: this chunk already */
    else g += (tgt - g) >> 7;                   /* release */
    dg = (g - lim_g) / (int32_t)CHUNK;
    for (i = 0; i < CHUNK; i++) {
        int32_t gi = lim_g + dg * (int32_t)(i + 1u);
        if (gi > g && tgt < lim_g) gi = g;      /* (never above what the peak allows) */
        L[i] = (int32_t)(((int64_t)L[i] * (tgt < lim_g ? g : gi)) >> 15);
        R[i] = (int32_t)(((int64_t)R[i] * (tgt < lim_g ? g : gi)) >> 15);
    }
    lim_g = g;
}

/* ---- the master for CHUNK frames: L, R in (the tracks and the live bus); E, V: the echo's and the
 * reverb's sends. The echo is 3/16 (a dotted 8th), darker each repeat; then the master's effect */
static fxs_t mfx_l, mfx_r;
static void fx_master(int32_t *L, int32_t *R, const int32_t *E, const int32_t *V)
{
    uint32_t i, edly = (g_rowlen * 3u) >> 1;   /* (the echo runs at 22050 Hz, as the SPU's reverb: its line is half) */
    int32_t fb = lfx_on >> LF_ECHO & 1u ? 22000 : 13000;
    if (edly >= ECHON - 16u)
        edly = g_rowlen;
    if (edly >= ECHON - 16u)
        edly = ECHON - 16u;
    for (i = 0; i + 1u < CHUNK && cpu_shed < 2u; i += 2u) {
        int32_t e = echo_buf[(echo_w - edly) & (ECHON - 1u)], in, last = echo_lp;
        echo_lp += ((e - echo_lp) * 18000) >> 15;
        in = ((E[i] + E[i + 1u]) >> 1) + ((echo_lp * fb) >> 15);
        echo_buf[echo_w & (ECHON - 1u)] = (int16_t)clampi(in, -32767, 32767);
        echo_w++;
        L[i] += (last + echo_lp) >> 1;
        R[i] += (last + echo_lp) >> 1;
        L[i + 1u] += echo_lp;
        R[i + 1u] += echo_lp;
    }
    if (cpu_shed < 1u)                          /* (the load governor: kawa_seq.c) */
        reverb_chunk(V, L, R);
    fx_insert(&mfx_l, prj.mfx[0], prj.mfx[1], prj.mfx[2], prj.mfx[3], L, CHUNK);
    fx_insert(&mfx_r, prj.mfx[0], prj.mfx[1], prj.mfx[2], prj.mfx[3], R, CHUNK);
    limiter(L, R);
    for (i = 0; i < CHUNK; i++) {
        L[i] = sat(L[i]);
        R[i] = sat(R[i]);
    }
}

/* a track's SAT send (FX, SENDS): a soft saturation before its pan and its sends. A cubic knee, x 1.5 to x 9 of gain into
 * it, the level brought back down as the gain goes up; 32-bit products but the first (the FM goes past 100000) */
static void sat_chunk(int32_t *m, uint32_t amt)
{
    int32_t g = 384 + (int32_t)amt * 128, comp = 256 - (int32_t)amt * 9, i;
    for (i = 0; i < (int32_t)CHUNK; i++) {
        int32_t x = (int32_t)(((int64_t)m[i] * g) >> 8), u2, u3;
        if (x > 32767) x = 32767;
        else if (x < -32767) x = -32767;
        u2 = (x * x) >> 15;                     /* x squared, Q15 */
        u3 = (u2 * x) >> 15;                    /* x cubed */
        m[i] = (((3 * x - u3) >> 1) * comp) >> 8;
    }
}
