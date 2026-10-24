/* SPDX-License-Identifier: GPL-3.0-only */
/* Wonderblue-OS (WND-1): a tracker for the M-VAVE FM-1 for ambient breakbeat, trip-hop, jungle, boom bap and the rest:
 * a chopped break with punch-ins, three synthesised drums, three synths (FM, analog, voice) over the chords you pick,
 * songs written from 49 genres, and SIT, which lets it play by itself. The browser version (z-noise.html) on the chip.
 *
 * Built on KAWA's platform (Dinghy, Felucca, SLOOP): platform.c gives updates, the USB rescue, USB-MIDI and audio, the
 * panel, the screen and flash; kawa_gfx.c draws in bands. One compilation unit:
 *   wb_tables.h, wb_data.h  tables (tools/gen_wb_tables.py) and the genres, sounds, progressions (tools/gen_wb_data.js)
 *   wb_core.c     the song, the scale and chord maths, what the interrupt and the main loop share
 *   wb_gen.c      writing music: genres, sections A..H, FILL, DICE, progressions, SIT
 *   wb_dsp.c      the voices, the sequencer, the master, app_audio      (audio interrupt)
 *   wb_ui.c       the pages, the panel, the store, app_main
 * Rules of the chip (docs/RULES.md): integers only, no 64-bit division, the audio interrupt short. */
#define APP_NAME "WONDERBLUE"
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_920"                   /* Wonderblue-OS's identity, also outside build.py (it passes FM-1_92N) */
#endif
#if !defined(DINGHY_HOST) && !defined(KAWA_WASM)
#ifndef STORE_LO
#define STORE_LO 0x00097000u                    /* the store: 16 song slots of 0x4000 (slot 0: the autosave) */
#define STORE_HI 0x000E0000u                    /* (below the update staging, 0xE0000) */
#endif
#endif
#include "platform.c"
#include "wb_breaks.h"                     /* the breaks, clean (tools/gen_breaks.py --clean) */
#include "kawa_art.h"
#include "kawa_fonts.h"
#include "wb_tables.h"
#include "wb_data.h"
#include "wb_core.c"
#include "wb_gen.c"
#include "wb_dsp.c"
#include "kawa_gfx.c"
#include "wb_ui.c"
