/*
 * main.c - RXDK + SDL3 front end for the Manic Miner port (ManicMinerX).
 *
 * Ported from the nxdk front end (ChrisGPlugs/ManicMiner-nxdk, src/main.c,
 * MM_COMBINED build). Both engines are linked in: at start-up it looks for
 * ManicMiner.bin and ManicMiner128.tap and starts the one that is there,
 * or shows a chooser built from Manic Miner's own title screen when both are.
 *
 * The game (mm_game.c) runs as straight-line code; every emulated Spectrum
 * frame (50 Hz) zx.c calls frame_hook() below, which turns that frame's
 * beeper/AY activity into PCM, reads the controller, draws the Spectrum
 * screen and paces everything to real time.
 *
 * What changed from nxdk: audio goes through an SDL3 audio stream (RXDK's
 * DirectSound on the MCPX APU) instead of the AC97 being fed directly;
 * pacing uses the stream's queue the same way. Video is a 320x240 frame
 * drawn by Direct3D (platform_rxdk.cpp) instead of a 2x copy into the
 * framebuffer. Input is SDL3's gamepad API.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "platform.h"
#include "zx.h"
#include "mm_mem.h"          /* Bug-Byte addresses (shared ones are identical) */
#include "mm_game.h"
#include "mm_font.h"
#include "hiscore.h"
#include "ay.h"
#include "mm128_load.h"

/* Which game is running. */
static int v128;

/* Manic Miner 128 addresses the front end needs (see mm_mem.h, -DMM128). */
#define MM128_PatchLivesDec   35147
#define MM128_MenuLivesYesNo  38004
#define MM128_MenuText        37986    /* "1 Infinite lives: NO ..." */

#define AUDIO_RATE   48000

static SDL_Gamepad *pad;
static uint32_t frame_pixels[ZX_SCREEN_W * ZX_SCREEN_H];
static uint64_t frame_count;
static uint64_t perf_next;          /* wall-clock pacing when there is no audio */

/* Audio queue targets. The amount of audio queued and not yet played is
 * the game's clock: the Spectrum is emulated only as fast as its sound is
 * played. (SDL's device keeps its own few chunks on top of this.) */
#define AUDIO_QUEUE_TARGET_MS  80   /* pace the game to keep ~80 ms queued */
#define AUDIO_QUEUE_LOW_MS     40   /* below this, skip drawing to catch up */

/* Diagnostics overlay (left-stick click): queued audio, underruns, draw
 * time, frames drawn and skipped. */
static int      diag_on;
static unsigned diag_underruns, diag_skipped;
static unsigned diag_draw_us;

/* Chooser input, latched by poll_input so a quick tap is never missed. */
static int ui_up, ui_down, ui_select;
static int ui_stick_y;

/* Back pressed twice on a game's intro (title screen or demo) returns to
 * the chooser: the frame hook longjmps out of the running engine. */
static int      chooser_available, game_running, back_to_chooser;
static uint64_t last_back_frame;
static jmp_buf  chooser_jmp;
#define BACK_DOUBLE_FRAMES 75                /* 1.5 s */

/* Logged once the first frame is on screen ("MM READY") and on each
 * change of screen, for tools/rxdk-run.ps1 -Until. */
static const char *pending_screen;
static int ready_logged;
static void screen_shown(const char *name) { pending_screen = name; }

/* Picture shape, toggled with the right stick click and kept in the save
 * folder (U:\ASPECT.TXT, "4:3" or "16:9"). After a change the new shape
 * is named at the top of the screen for two seconds. */
static int wide_on;
static int aspect_msg_frames;
#define ASPECT_MSG_FRAMES 100

static int in_chooser;               /* the chooser is on screen */

/* Back during play asks END GAME? first (1 = asking, 2 = yes, 3 = no); the
 * game is frozen meanwhile. On yes, SHIFT+SPACE is held for quit_frames. */
static int confirm_quit, quit_frames;

/* Manic Miner 128's options menu: Y (key 1) in play opens it, Start (key 4)
 * closes it. While it is open, LT / RT are its keys 2 (air) and 3 (music);
 * otherwise they jump left / right. The menu has no flag of its own, so it
 * also has to be on screen: its box is 7 rows x 24 cells of attribute 71
 * from row 2, column 4. If Y didn't open it, LT / RT stay jumps. */
static int mm128_menu;
#define MODE_LABEL_FRAMES 250        /* 5 s */

static void aspect_load(void)
{
    char path[64], buf[8] = { 0 };
    snprintf(path, sizeof path, "%sASPECT.TXT", plat_save_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    wide_on = !strncmp(buf, "16:9", 4);
}

static void aspect_save(void)
{
    char path[64];
    snprintf(path, sizeof path, "%sASPECT.TXT", plat_save_dir());
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(wide_on ? "16:9" : "4:3", f);
    if (fclose(f) == 0) plat_commit_file(path);
}

static void aspect_toggle(void)
{
    wide_on = !wide_on;
    plat_set_wide(wide_on);
    aspect_save();
    aspect_msg_frames = ASPECT_MSG_FRAMES;
}

/* ------------------------------------------------------------------------ */
/* Audio output                                                             */
/* ------------------------------------------------------------------------ */
static SDL_AudioStream *audio_stream;
static int audio_ok;
/* Diagnostics measured over ~2 s windows of wall-clock time. */
static uint64_t stats_t0_us, stats_samples, stats_frames, stats_t0_z, stats_t0_contended;
static unsigned stats_rate_hz, stats_fps_x100, stats_contention_pct10;

static void audio_open(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!audio_stream) {
        plat_log("MM: no audio: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(audio_stream);
    audio_ok = 1;
}

static unsigned audio_queued_ms(void)
{
    if (!audio_ok) return 0;
    int q = SDL_GetAudioStreamQueued(audio_stream);
    return q > 0 ? (unsigned)q / (AUDIO_RATE / 1000 * 4) : 0;
}

static void audio_submit(const int16_t *buf, int frames)
{
    if (!audio_ok) return;
    if (frame_count > 50 && SDL_GetAudioStreamQueued(audio_stream) == 0) diag_underruns++;
    SDL_PutAudioStreamData(audio_stream, buf, frames * 2 * (int)sizeof(int16_t));
}

/* Per-frame statistics: playback rate (samples consumed per second; while
 * the queue level is steady, submitted == consumed), emulated frames per
 * second, and the share of emulated time spent in ULA contention. */
static void stats_frame(int samples)
{
    uint64_t now = plat_now_us();
    if (!stats_t0_us) {
        stats_t0_us = now; stats_samples = 0; stats_frames = 0;
        stats_t0_z = zx_t; stats_t0_contended = zx_contended_t;
        return;
    }
    stats_samples += (uint64_t)samples;
    stats_frames++;
    uint64_t dt = now - stats_t0_us;
    if (dt >= 2000000) {
        stats_rate_hz = (unsigned)(stats_samples * 1000000 / dt);
        stats_fps_x100 = (unsigned)(stats_frames * 100000000 / dt);
        uint64_t dz = zx_t - stats_t0_z, dc = zx_contended_t - stats_t0_contended;
        stats_contention_pct10 = dz ? (unsigned)(dc * 1000 / dz) : 0;
        /* Every ~10 s, the overlay's figures to the debug log too. */
        static unsigned windows;
        if (++windows % 5 == 0)
            plat_log("MM STATS fps %u.%02u audio %u Hz queued %u ms gaps %u skipped %u contention %u.%u%%\n",
                     stats_fps_x100 / 100, stats_fps_x100 % 100, stats_rate_hz, audio_queued_ms(),
                     diag_underruns, diag_skipped, stats_contention_pct10 / 10, stats_contention_pct10 % 10);
        stats_t0_us = now; stats_samples = 0; stats_frames = 0;
        stats_t0_z = zx_t; stats_t0_contended = zx_contended_t;
    }
}

/* ------------------------------------------------------------------------ */
/* Beeper -> PCM                                                            */
/* ------------------------------------------------------------------------ */
static int16_t audio_buf[2 * 2048];
static double  samples_carry;
static uint8_t frame_start_level;
static float   hp_prev_in, hp_prev_out;
static ay_t    ay;

static float level_value(uint8_t level)
{
    /* EAR (bit 4) drives the speaker; MIC (bit 3) leaks in slightly. */
    return ((level & 0x10) ? 1.0f : 0.0f) + ((level & 0x08) ? 0.06f : 0.0f);
}

static void render_audio(void)
{
    double exact = (double)AUDIO_RATE * ZX_T_PER_FRAME / ZX_CPU_HZ + samples_carry;
    int n = (int)exact;
    samples_carry = exact - n;
    if (n > 2048) n = 2048;

    double t_per_sample = (double)ZX_T_PER_FRAME / n;
    int ev = 0;
    uint8_t level = frame_start_level;
    int ay_ev = 0;
    for (int i = 0; i < n; i++) {
        /* Box-filter the speaker level over this sample's T-state span. */
        double t0 = i * t_per_sample, t1 = t0 + t_per_sample, t = t0;
        double acc = 0.0;
        while (ev < zx_beep_count && zx_beeps[ev].t < t1) {
            double te = zx_beeps[ev].t > t ? zx_beeps[ev].t : t;
            acc += level_value(level) * (te - t);
            t = te;
            level = zx_beeps[ev].level;
            ev++;
        }
        acc += level_value(level) * (t1 - t);
        float x = (float)(acc / t_per_sample);
        if (v128) {
            /* AY register writes take effect at the sample they fall in. */
            while (ay_ev < zx_ay_count && zx_ay_log[ay_ev].t < t1) {
                ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val);
                ay_ev++;
            }
            x = x * 0.6f + ay_sample(&ay) * 1.6f;
        }
        /* DC-blocking high-pass so a speaker left "on" is silent. */
        float y = x - hp_prev_in + 0.995f * hp_prev_out;
        hp_prev_in = x;
        hp_prev_out = y;
        int s = (int)(y * 9000.0f);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        audio_buf[2 * i] = audio_buf[2 * i + 1] = (int16_t)s;
    }
    frame_start_level = zx_beep_level;
    if (v128)
        while (ay_ev < zx_ay_count) { ay_write(&ay, zx_ay_log[ay_ev].reg, zx_ay_log[ay_ev].val); ay_ev++; }

    audio_submit(audio_buf, n);
    stats_frame(n);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */
/* Spectrum keys driven by the controller, and by the macros below. */
static uint8_t pad_keys[8], macro_keys[8];
static int triggers_held;            /* LT and RT both down: IGR combo in progress */

static void set_bit(uint8_t *rows, int row, int bit, int down)
{
    if (down) rows[row] |= (uint8_t)(1u << bit);
    else      rows[row] &= (uint8_t)~(1u << bit);
}

/* Cheat-code macro: types the cheat one key at a time. */
typedef struct { uint8_t row, bit; } zx_keypos_t;
static const zx_keypos_t cheat_keys_48[] = {
    /* 6031769 */
    { ZX_ROW_0_6, 4 }, { ZX_ROW_0_6, 0 }, { ZX_ROW_1_5, 2 }, { ZX_ROW_1_5, 0 },
    { ZX_ROW_0_6, 3 }, { ZX_ROW_0_6, 4 }, { ZX_ROW_0_6, 1 },
};
static const zx_keypos_t cheat_keys_128[] = {
    /* TYPEWRITER (Software Projects) */
    { ZX_ROW_Q_T, 4 }, { ZX_ROW_P_Y, 4 }, { ZX_ROW_P_Y, 0 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 1 },
    { ZX_ROW_Q_T, 3 }, { ZX_ROW_P_Y, 2 }, { ZX_ROW_Q_T, 4 }, { ZX_ROW_Q_T, 2 }, { ZX_ROW_Q_T, 3 },
};
#define CHEAT_KEYS   (v128 ? cheat_keys_128 : cheat_keys_48)
#define CHEAT_LEN    (v128 ? 10 : 7)
#define CHEAT_DONE   (v128 ? 10 : 7)       /* KeyCounter value once typed */
#define TELEPORT_BIT (v128 ? 1 : 4)        /* hold '9' (128) or '6' (48K) */
static int macro_step = -1, macro_timer;

/* Teleport macro. The game's teleport check reads the 0-6 row (activator
 * '6', or '9' on Manic Miner 128) and then, immediately after, the 1-5 row
 * for the cavern number. Rather than holding keys down, the macro answers
 * exactly that pair of port reads: the activator shows as pressed on 0-6
 * row reads, and the cavern number appears only on the 1-5 read that
 * directly follows one of those. Every other read of the 1-5 row sees the
 * keys up, so on Manic Miner 128 key '1' never opens Mihai's options menu
 * and key '5' never nudges Willy left. One teleport per press. */
static int teleport_pending, teleport_target, teleport_frames;
static uint16_t last_in_port;
static int last_in_was_forced_activator;

static uint8_t teleport_in_filter(uint16_t port, uint8_t v)
{
    int after_activator = (last_in_port == 0xEFFE) && last_in_was_forced_activator;
    last_in_port = port;
    last_in_was_forced_activator = 0;
    if (!teleport_pending) return v;
    if (port == 0xEFFE) {
        last_in_was_forced_activator = 1;
        return (uint8_t)(v & ~(1u << TELEPORT_BIT));
    }
    if (port == 0xF7FE && after_activator) {
        teleport_pending = 0;
        return (uint8_t)((v & 0xE0) | (0x1F & ~teleport_target));
    }
    return v;
}

static void start_teleport(int forward)
{
    if (mem[KeyCounter] != CHEAT_DONE) return;
    int cav = mem[CurrentCavernNumber] + (forward ? 1 : 19);
    teleport_target = cav % 20;
    teleport_pending = 1;
    teleport_frames = 0;
}

static void run_macros(void)
{
    memset(macro_keys, 0, sizeof macro_keys);
    if (macro_step >= 0) {
        /* Hold each digit for 8 frames then release for 8: the game checks
         * the keys once per main-loop pass (~3.5 frames). */
        if (macro_timer < 8) set_bit(macro_keys, CHEAT_KEYS[macro_step].row, CHEAT_KEYS[macro_step].bit, 1);
        if (++macro_timer >= 16) {
            macro_timer = 0;
            if (++macro_step >= CHEAT_LEN) macro_step = -1;
        }
    }
    /* Give up if the game never reached its teleport check (e.g. a
     * teleport requested on the title screen or during the game-over
     * sequence). */
    if (teleport_pending && ++teleport_frames > 25) teleport_pending = 0;
    if (quit_frames > 0) {
        quit_frames--;
        set_bit(macro_keys, ZX_ROW_SHIFT_V, 0, 1);         /* SHIFT+SPACE: quit */
        set_bit(macro_keys, ZX_ROW_SPACE_B, 0, 1);
    }
}

/* In a game, past the title screen and the demo. */
static int in_play(void)
{
    return game_running && !mm_in_title && mem[GameModeIndicator] == 0;
}

/* The options menu names the pad buttons instead of keys 1-4. Each label
 * keeps the original's length, because MenuToggle writes YES / NO at fixed
 * places in this text (MenuLivesYesNo, MenuAirYesNo, MenuMusicYesNo). */
static void mm128_menu_labels(void)
{
    static const struct { uint16_t at; const char *was, *now; } lab[] = {
        { MM128_MenuText,      "1 Infinite lives: ", "Y Infinite lives: " },
        { MM128_MenuText + 21, "2 Infinite air: ",   "LT Endless air: "   },
        { MM128_MenuText + 40, "3 Music: ",          "RT Tune: "          },
        { MM128_MenuText + 52, "4 Resume game",      "ST Resume    "      },
    };
    for (unsigned i = 0; i < sizeof lab / sizeof lab[0]; i++) {
        size_t n = strlen(lab[i].was);
        if (memcmp(mem + lab[i].at, lab[i].was, n)) {
            plat_log("MM: menu text %u not as expected, left alone\n", i);
            continue;
        }
        memcpy(mem + lab[i].at, lab[i].now, n);
    }
}

static int mm128_menu_on_screen(void)
{
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 24; c++)
            if (mem[22596 + r * 32 + c] != 71) return 0;
    return 1;
}

static void pad_button(int button, int down)
{
    /* While both triggers are held (the in-game reset combo), Start and
     * Back don't act on the game. Releases always go through. */
    if (down && triggers_held && (button == SDL_GAMEPAD_BUTTON_START || button == SDL_GAMEPAD_BUTTON_BACK))
        return;
    if (confirm_quit) {
        if (down) {
            if (button == SDL_GAMEPAD_BUTTON_SOUTH) confirm_quit = 2;
            else if (button == SDL_GAMEPAD_BUTTON_EAST || button == SDL_GAMEPAD_BUTTON_BACK) confirm_quit = 3;
        }
        return;
    }
    if (v128 && down && button == SDL_GAMEPAD_BUTTON_NORTH && in_play()) mm128_menu = 1;
    if (down && button == SDL_GAMEPAD_BUTTON_START) mm128_menu = 0;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:                             /* A */
    case SDL_GAMEPAD_BUTTON_EAST:                              /* B */
        set_bit(pad_keys, ZX_ROW_SPACE_B, 2, down);        /* M: jump / resume */
        break;
    case SDL_GAMEPAD_BUTTON_START:
        set_bit(pad_keys, ZX_ROW_ENTER_H, 0, down);        /* ENTER: start game */
        if (v128) set_bit(pad_keys, ZX_ROW_1_5, 3, down);  /* 4: leave the menu */
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:                             /* Y */
        if (!v128) set_bit(pad_keys, ZX_ROW_ENTER_H, 4, down);  /* H: music on/off */
        else       set_bit(pad_keys, ZX_ROW_1_5, 0, down);      /* 1: menu / lives */
        break;
    case SDL_GAMEPAD_BUTTON_WEST:                              /* X */
        set_bit(pad_keys, ZX_ROW_A_G, 0, down);            /* A: pause */
        break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        if (down) aspect_toggle();                         /* 4:3 <-> 16:9 */
        break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        if (down) diag_on = !diag_on;                      /* diagnostics */
        break;
    case SDL_GAMEPAD_BUTTON_BACK:
        if (mm128_menu && mm128_menu_on_screen())          /* MM128 menu: no quit there */
            break;
        if (in_play()) {                                   /* ask first */
            if (down) { confirm_quit = 1; memset(pad_keys, 0, sizeof pad_keys); }
            break;
        }
        set_bit(pad_keys, ZX_ROW_SHIFT_V, 0, down);        /* SHIFT+SPACE: quit */
        set_bit(pad_keys, ZX_ROW_SPACE_B, 0, down);
        if (down && chooser_available && game_running &&
            (mm_in_title || mem[GameModeIndicator] != 0)) {      /* intro / demo */
            if (last_back_frame && frame_count - last_back_frame <= BACK_DOUBLE_FRAMES)
                back_to_chooser = 1;
            last_back_frame = frame_count;
        }
        break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:                 /* White */
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:                /* Black */
        if (down) {
            if (mem[KeyCounter] != CHEAT_DONE) {
                if (macro_step < 0) { macro_step = 0; macro_timer = 0; }
            } else {
                start_teleport(button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            }
        }
        break;
    default:
        break;
    }
}

static void read_pad_axes(void)
{
    uint8_t joy = 0;
    int left = 0, right = 0, fire = 0;
    plat_check_igr(pad);                      /* doesn't return if the combo is held */
    int lt = pad && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
    int rt = pad && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    triggers_held = lt && rt;
    if (mm128_menu && (!v128 || !in_play())) mm128_menu = 0;
    int menu = mm128_menu && mm128_menu_on_screen();
    /* MM128 menu open: options 2 (air) and 3 (music) on the triggers. */
    set_bit(pad_keys, ZX_ROW_1_5, 1, menu && lt);
    set_bit(pad_keys, ZX_ROW_1_5, 2, menu && rt);
    if (pad) {
        int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        left  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -12000;
        right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 12000;
        fire  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
        /* LT / RT: jump left / right (direction + jump together). Both held
         * is the start of the IGR combo, so that does nothing. */
        if (in_play() && !menu && lt != rt) {            /* not on the title / demo */
            if (lt) left = 1; else right = 1;
            fire = 1;
        }
        int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        int dir = y < -16000 ? -1 : (y > 16000 ? 1 : 0);
        if (dir != ui_stick_y) { if (dir < 0) ui_up = 1; if (dir > 0) ui_down = 1; }
        ui_stick_y = dir;
    }
    if (right) joy |= 0x01;
    if (left)  joy |= 0x02;
    if (fire)  joy |= 0x10;
    zx_kempston = joy;
    /* Also press O / P so that left/right resume a paused game. */
    set_bit(pad_keys, ZX_ROW_P_Y, 1, left);
    set_bit(pad_keys, ZX_ROW_P_Y, 0, right);
}

static void poll_input(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
                SDL_CloseGamepad(pad);
                pad = NULL;
                memset(pad_keys, 0, sizeof pad_keys);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (!pad) pad = SDL_GetGamepadFromID(e.gbutton.which);
            pad_button(e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) ui_up = 1;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) ui_down = 1;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                    (e.gbutton.button == SDL_GAMEPAD_BUTTON_START && !triggers_held)) ui_select = 1;
            }
            break;
        default:
            break;
        }
    }
    read_pad_axes();
    run_macros();
    for (int row = 0; row < 8; row++)
        zx_keys[row] = (uint8_t)(0x1F & ~(pad_keys[row] | macro_keys[row]));
}

/* ------------------------------------------------------------------------ */
/* Video and pacing                                                         */
/* ------------------------------------------------------------------------ */
/* Has the picture changed since it was last drawn? Manic Miner redraws
 * the screen once per main-loop pass (~3.5 frames), so most frames are
 * identical and needn't be drawn again. */
static int screen_changed(int flash_phase)
{
    static uint8_t last[6912];
    static int last_border = -1, last_flash = -1;
    int has_flash = 0;
    for (int i = 0; i < 768; i++)
        if (mem[0x5800 + i] & 0x80) { has_flash = 1; break; }
    int flash_key = has_flash ? flash_phase : -1;
    if (last_border == zx_border && last_flash == flash_key && !memcmp(last, mem + 0x4000, sizeof last))
        return 0;
    memcpy(last, mem + 0x4000, sizeof last);
    last_border = zx_border;
    last_flash = flash_key;
    return 1;
}

/* Text for the diagnostics overlay, drawn with the game's own font. */
static void draw_text(uint32_t *dst, int pitch, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    for (; *s; s++, x += 8) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 32 || ch > 127) ch = '?';
        for (int py = 0; py < 8; py++) {
            uint8_t bits = mem[ROM_FONT + (ch - 32) * 8 + py];
            uint32_t *row = dst + (y + py) * pitch + x;
            for (int px = 0; px < 8; px++)
                row[px] = (bits & (0x80 >> px)) ? fg : bg;
        }
    }
}

/* The overlay: a box of labelled lines, inside the TV-safe area. */
static void draw_diag(uint32_t *dst, int pitch, int x0, int y0)
{
    char lines[6][48];
    unsigned exp_fps_x100 = (unsigned)((uint64_t)ZX_CPU_HZ * 100 / ZX_T_PER_FRAME);
    snprintf(lines[0], 48, " SPEED %3u.%02u FPS (REAL %u.%02u) ", stats_fps_x100 / 100, stats_fps_x100 % 100,
             exp_fps_x100 / 100, exp_fps_x100 % 100);
    snprintf(lines[1], 48, " AUDIO RATE %5u HZ (48000)    ", stats_rate_hz);
    snprintf(lines[2], 48, " AUDIO QUEUED %3u MS  GAPS %-5u", audio_queued_ms(), diag_underruns);
    snprintf(lines[3], 48, " DRAW %2u.%u MS  SKIPPED %-8u", diag_draw_us / 1000, (diag_draw_us / 100) % 10, diag_skipped);
    snprintf(lines[4], 48, " ULA CONTENTION %2u.%u%%          ", stats_contention_pct10 / 10, stats_contention_pct10 % 10);
    snprintf(lines[5], 48, "                               ");
    for (int i = 0; i < 6; i++)
        draw_text(dst, pitch, x0, y0 + i * 10, lines[i], 0xFFFFFFFF, 0xFF0000A0);
}

static void present(void)
{
    int flash = (int)((frame_count >> 4) & 1);
    /* The aspect label is drawn while it shows, plus once more to clear it. */
    int msg = aspect_msg_frames > 0;
    if (msg) aspect_msg_frames--;
    int clear_msg = msg && aspect_msg_frames == 0;
    /* The video mode is named in the top border on the chooser, and for
     * the first five seconds when a game starts without one. */
    static int last_mode_label;
    int mode_label = in_chooser || frame_count < MODE_LABEL_FRAMES;
    int label_changed = mode_label != last_mode_label;
    last_mode_label = mode_label;
    static int last_confirm;
    int confirm_changed = (confirm_quit == 1) != last_confirm;
    last_confirm = confirm_quit == 1;
    if (!screen_changed(flash) && !diag_on && !msg && !clear_msg && !label_changed && !confirm_changed) return;
    uint64_t t0 = plat_now_us();
    zx_render(frame_pixels, ZX_SCREEN_W, flash);
    if (diag_on) draw_diag(frame_pixels, ZX_SCREEN_W, 16, 16);
    if (confirm_quit == 1) {
        static const char *const box[3] = {
            "                        ",
            " END GAME?  A=YES  B=NO ",
            "                        ",
        };
        for (int i = 0; i < 3; i++)
            draw_text(frame_pixels, ZX_SCREEN_W, (ZX_SCREEN_W - 8 * 24) / 2, ZX_SCREEN_H / 2 - 12 + i * 8,
                      box[i], 0xFFFFFFFF, 0xFFA00000);
    }
    if (mode_label) {
        char label[48];
        snprintf(label, sizeof label, " %s ", plat_video_mode_name());
        int x = (ZX_SCREEN_W - 8 * (int)strlen(label)) / 2;
        draw_text(frame_pixels, ZX_SCREEN_W, x < 0 ? 0 : x, 8, label, 0xFFFFFFFF, 0xFF0000A0);
    } else if (msg && !clear_msg) {
        const char *label = wide_on ? " 16:9 " : " 4:3 ";
        int x = (ZX_SCREEN_W - 8 * (int)strlen(label)) / 2;
        draw_text(frame_pixels, ZX_SCREEN_W, x, 8, label, 0xFFFFFFFF, 0xFF0000A0);
    }
    plat_present(frame_pixels);
    diag_draw_us = (unsigned)((diag_draw_us * 9 + (unsigned)(plat_now_us() - t0)) / 10);
    if (pending_screen) {
        plat_log("MM SCREEN %s\n", pending_screen);
        pending_screen = NULL;
        if (!ready_logged) { ready_logged = 1; plat_log("MM READY\n"); }
    }
}

static void pace(void)
{
    if (audio_ok) {
        /* The audio queue is the clock: keep ~80 ms queued. */
        while (audio_queued_ms() > AUDIO_QUEUE_TARGET_MS)
            SDL_Delay(1);
        return;
    }
    uint64_t frame = (uint64_t)1000000 * ZX_T_PER_FRAME / ZX_CPU_HZ;
    uint64_t now = plat_now_us();
    if (!perf_next || now > perf_next + 5 * frame) perf_next = now;
    perf_next += frame;
    while (plat_now_us() < perf_next) {
        uint64_t left_ms = (perf_next - plat_now_us()) / 1000;
        if (left_ms > 1) SDL_Delay((Uint32)(left_ms - 1));
    }
}

/* ------------------------------------------------------------------------ */
/* Saved high scores                                                        */
/* ------------------------------------------------------------------------ */
/* One high score per game, saved in the Xbox save area (hiscore.c). It is
 * loaded into the game's HighScore before the game starts, and saved when
 * a game ends with a better score, unless a cheat was used in that game:
 * the cheat code (which also enables teleport) or, in MM128, the options
 * menu's infinite lives or infinite air (which can be toggled during play,
 * so any frame with one active counts). */
#define MM128_PatchAirCall  34843        /* CALL (0xCD) when air is finite */
#define ScoreDigits (Score2 + 3)         /* six ASCII digits */

static uint8_t hs_best[6];
static int     hs_have, hs_cheated, hs_was_title = 1;

static void hs_begin_game(void)
{
    hs_have = hs_load(v128, hs_best);
    if (hs_have) memcpy(&mem[HighScore], hs_best, 6);
    hs_cheated = 0;
    hs_was_title = 1;
}

static void hs_track_cheats(void)
{
    int title = mm_in_title || mem[GameModeIndicator] != 0;
    if (hs_was_title && !title) hs_cheated = 0;     /* a new game has begun */
    hs_was_title = title;
    if (title) return;
    if (mem[KeyCounter] == CHEAT_DONE) hs_cheated = 1;
    if (v128 && (mem[MM128_PatchLivesDec] != 0x35 || mem[MM128_PatchAirCall] != 0xCD))
        hs_cheated = 1;
}

static void hs_game_over(void)
{
    const uint8_t *sc = &mem[ScoreDigits];
    if (hs_cheated) {
        plat_log("MM: game over %.6s (cheat used, not saved)\n", (const char *)sc);
        return;
    }
    if (hs_have && memcmp(sc, hs_best, 6) <= 0) return;   /* ASCII digits compare numerically */
    if (!hs_have && memcmp(sc, "000000", 6) == 0) return;
    memcpy(hs_best, sc, 6);
    hs_have = 1;
    int ok = hs_save(v128, hs_best);
    plat_log("MM: game over, new high score %.6s %s\n", (const char *)hs_best, ok ? "saved" : "NOT saved");
}

/* END GAME? asked: hold the game still (sound silent, picture kept) until
 * A (yes) or B / Back (no), then until those buttons are let go, so the
 * answer never reaches the game as a jump. */
static void confirm_loop(void)
{
    static const int16_t silence[2 * 960];
    plat_log("MM SCREEN endgame?\n");
    while (confirm_quit == 1) { audio_submit(silence, 960); poll_input(); present(); pace(); }
    int yes = confirm_quit == 2;
    while (pad && (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))) {
        audio_submit(silence, 960); poll_input(); present(); pace();
    }
    confirm_quit = 0;
    if (yes) quit_frames = 10;
    plat_log("MM: end game? %s\n", yes ? "yes" : "no");
}

static void frame_hook(void)
{
    render_audio();
    poll_input();
    hs_track_cheats();
    if (back_to_chooser) {
        back_to_chooser = 0;
        longjmp(chooser_jmp, 1);             /* leave the engine for the chooser */
    }
    if (confirm_quit) confirm_loop();
    /* If the audio queue is running low, the game must catch up: emulate
     * this frame without drawing it. Sound must never wait for pictures. */
    if (audio_ok && frame_count > 50 && audio_queued_ms() < AUDIO_QUEUE_LOW_MS)
        diag_skipped++;
    else
        present();
    pace();
    frame_count++;
}

/* ------------------------------------------------------------------------ */
/* Start-up                                                                 */
/* ------------------------------------------------------------------------ */
/* Data files are looked for next to default.xbe (D:\), then in
 * D:\datafiles\, where the build puts the repo's datafiles/ folder. */
static long load_data(const char *name, uint8_t *dst, long max)
{
    static const char *const sub[] = { "", "datafiles\\" };
    for (int i = 0; i < 2; i++) {
        char path[128];
        snprintf(path, sizeof path, "%s%s%s", plat_data_dir(), sub[i], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        long n = (long)fread(dst, 1, (size_t)max, f);
        fclose(f);
        plat_log("MM: %s: %ld bytes\n", path, n);
        return n;
    }
    return -1;
}

/* Show an error on the Spectrum screen for a while, then quit. */
static void fatal_screen(const char *line1, const char *line2)
{
    plat_log("MM: %s %s\n", line1, line2);
    memset(mem + 0x4000, 0, 0x1800);
    memset(mem + 0x5800, 0x47, 0x300);
    zx_border = 2;
    const char *lines[2] = { line1, line2 };
    for (int l = 0; l < 2; l++) {
        const char *s = lines[l];
        int row = 10 + l * 2;
        for (int col = 0; s[col] && col < 32; col++) {
            uint8_t ch = (uint8_t)s[col];
            for (int py = 0; py < 8; py++) {
                uint16_t addr = (uint16_t)(0x4000 | ((row & 0x18) << 8) | (py << 8) | ((row & 7) << 5) | col);
                mem[addr] = mem[ROM_FONT + (ch - 32) * 8 + py];
            }
        }
    }
    screen_shown("error");
    for (int i = 0; i < 50 * 15; i++) {
        poll_input();
        present();
        SDL_Delay(20);
    }
    plat_exit();
}

/* ------------------------------------------------------------------------ */
/* Chooser (both games present)                                             */
/*                                                                          */
/* Built from Manic Miner's own title screen, which is in ManicMiner.bin:   */
/* the house-and-sun picture, the MANIC MINER band and the piano keyboard,  */
/* Willy walking at the top right, and the Blue Danube on the beeper with   */
/* the piano keys lighting up (the same note loop and key mapping as        */
/* PlayTheThemeTune). The bottom third holds the two choices, a scrolling   */
/* banner and a hint line.                                                  */
/* ------------------------------------------------------------------------ */

static uint16_t ui_scr(int row, int col)
{
    return (uint16_t)(0x4000 | ((row & 0x18) << 8) | ((row & 7) << 5) | col);
}

static void ui_print(int row, int col, const char *s)
{
    for (; *s && col < 32; s++, col++) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 32 || ch > 127) ch = ' ';
        for (int py = 0; py < 8; py++)
            mem[ui_scr(row, col) + (py << 8)] = mem[ROM_FONT + (ch - 32) * 8 + py];
    }
}

static void ui_attr_row(int row, uint8_t a) { memset(mem + 0x5800 + row * 32, a, 32); }

/* DrawASprite in overwrite mode (16x16, display-file addressing). */
static void ui_sprite(uint16_t hl, uint16_t de)
{
    for (int b = 0; b < 16; b++) {
        mem[hl] = mem[de++];
        mem[(hl & 0xFF00) | (uint8_t)(hl + 1)] = mem[de++];
        uint8_t h = (uint8_t)((hl >> 8) + 1), l = (uint8_t)hl;
        if (!(h & 7)) {
            h = (uint8_t)(h - 8);
            l = (uint8_t)(l + 0x20);
            if (!(l & 0xE0)) h = (uint8_t)(h + 8);
        }
        hl = (uint16_t)((h << 8) | l);
    }
}

static void ui_options(int sel)
{
    static const char *names[2] = {
        " MANIC MINER       BUG-BYTE 48K ",
        " MANIC MINER 128       AY MUSIC ",
    };
    for (int i = 0; i < 2; i++) {
        int row = 17 + 2 * i;
        char line[40];
        snprintf(line, sizeof line, "%s", names[i]);
        line[0] = (i == sel) ? '>' : ' ';
        ui_print(row, 0, line);
        ui_attr_row(row, (i == sel) ? 0x30 : 0x46);   /* cavern-name bar / bright yellow */
    }
}

/* Handle latched input; returns 1 if a game was chosen. */
static int ui_input(int *sel)
{
    int up = ui_up, down = ui_down, go = ui_select;
    ui_up = ui_down = ui_select = 0;
    if (up || down) { *sel ^= 1; ui_options(*sel); }
    return go;
}

/* CalcAFAForPianoKey */
static uint16_t ui_piano(uint8_t a)
{
    a = (uint8_t)(a - 8);
    a = (uint8_t)((a >> 3) | (a << 5));
    a = (uint8_t)(~a | 0xE0);
    return (uint16_t)(0x5900 | a);
}

/* One note of the theme tune (PlayTheThemeTune's loop, T-state timed). */
static void ui_note(uint16_t iy)
{
    uint8_t c = mem[iy], d = mem[iy + 1], e = mem[iy + 2], b = 0;
    mem[ui_piano(d)] = 80;
    uint16_t key = ui_piano(e);
    mem[key] = 40;
    uint8_t a = (uint8_t)key;
    zx_tick(150);
    for (;;) {
        zx_out(0x00FE, a);
        zx_tick(11 + 4);
        if (--d == 0) { d = mem[iy + 1]; a ^= 24; zx_tick(7 + 19 + 7); } else zx_tick(12);
        zx_tick(4);
        if (--e == 0) { e = mem[iy + 2]; a ^= 24; zx_tick(7 + 19 + 7); } else zx_tick(12);
        if (--b) { zx_tick(13); continue; }
        zx_tick(8 + 4);
        if (--c) { zx_tick(12); continue; }
        zx_tick(7);
        break;
    }
    mem[ui_piano(mem[iy + 1])] = 56;
    mem[ui_piano(mem[iy + 2])] = 56;
    zx_tick(200);
}

static int launcher(void)
{
    int sel = 0;
    static const char banner[] =
        "                                "
        ".  .  .  .  .  .  .  CHOOSE YOUR MINE  .  .  .  "
        "MANIC MINER . . BUG-BYTE ltd. 1983 . . By Matthew Smith  .  .  .  "
        "MANIC MINER 128 . . AY music by Mihai Novitchi . . fixes by Bob Fossil  .  .  .  "
        "MANICMINERX for the original Xbox  .  .  .  "
        "D-pad to choose . . A or START to play . . BACK twice on a title screen returns here . . "
        "LT+RT+BACK+START to reset  .  .  .  .  .  .  .  "
        "                                ";
    int banner_len = (int)sizeof banner - 1;

    /* The title screen, as Start draws it. */
    memset(mem + 0x4000, 0, 0x1800);
    memcpy(mem + 0x4000, mem + TitleScreenDataTop, 0x1000);
    memcpy(mem + 0x5800, mem + TheFinalBarrierData, 0x100);
    memcpy(mem + 0x5900, mem + BottomAttributes, 0x200);
    ui_sprite(0x483D, WillySpriteData1);
    /* Bottom third: the chooser. */
    for (int row = 16; row < 24; row++) ui_attr_row(row, 0x00);
    ui_options(sel);
    ui_print(23, 0, "  D-PAD: CHOOSE   A/START: PLAY ");
    ui_attr_row(23, 0x45);
    ui_attr_row(21, 0x46);
    ui_up = ui_down = ui_select = 0;
    screen_shown("chooser");
    in_chooser = 1;

    for (;;) {
        /* The Blue Danube, as on the title screen. */
        for (uint16_t iy = TitleScreenTuneData; mem[iy] != 0xFF; iy = (uint16_t)(iy + 3)) {
            ui_note(iy);
            if (ui_input(&sel)) goto chosen;
        }
        /* Then the banner scrolls past while Willy walks. */
        for (int i = 0; i + 32 <= banner_len; i++) {
            char window[33];
            memcpy(window, banner + i, 32);
            window[32] = 0;
            ui_print(21, 0, window);
            ui_sprite(0x483D, (uint16_t)(WillySpriteData | (((i & 6) >> 3 | (i & 6) << 5) & 0xFF)));
            zx_tick(100 * (256 * 13 + 16));      /* ~0.1 s, as Start4 */
            if (ui_input(&sel)) goto chosen;
        }
    }
chosen:
    /* Wait for the button to be released so the game's own title screen
     * doesn't see it as "start" straight away. */
    for (int i = 0; i < 100; i++) {
        zx_tick((int32_t)zx_t_per_frame);
        int held = !(zx_keys[ZX_ROW_ENTER_H] & 1) || !(zx_keys[ZX_ROW_0_6] & 1) ||
                   !(zx_keys[ZX_ROW_SPACE_B] & 4) || (zx_kempston & 0x10);
        if (!held) break;
    }
    zx_out(0x00FE, 0);
    return sel;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    plat_log("MM: Manic Miner (RXDK) starting\n");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        plat_log("MM: SDL_Init: %s\n", SDL_GetError());
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) plat_exit();
    }
    if (!plat_video_init(ZX_SCREEN_W, ZX_SCREEN_H)) plat_exit();

    aspect_load();
    if (wide_on) plat_set_wide(1);
    audio_open();

    {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }

    /* Optional 48K ROM for the authentic character set. */
    static uint8_t rom[0x4000];
    int have_rom = load_data("48.rom", rom, sizeof rom) == (long)sizeof rom;

    zx_reset();
    memset(mem, 0, sizeof mem);
    mm_install_font(have_rom ? rom : NULL);
    zx_frame_hook = frame_hook;

    /* Look for the games. A file that is there but isn't the expected one
     * counts as missing, so the other game still starts. */
    static uint8_t image48[65536];
    static uint8_t image128[65536];
    int ok48 = 0, ok128 = 0;
    const char *err1 = NULL, *err2 = NULL;
    {
        long n = load_data("ManicMiner.bin", image48, sizeof image48);
        if (n == 65536 && image48[AppEntry] == 0xF3 &&
            !memcmp(image48 + CentralCavernData + 0x200 + 9, "Central Cavern", 14))
            ok48 = 1;
        else if (n >= 0) { err1 = "ManicMiner.bin isn't the 64K"; err2 = "Manic Miner image (see README)."; }
    }
    {
        static uint8_t tap[65536];
        long n = load_data("ManicMiner128.tap", tap, sizeof tap);
        if (n >= 0) {
            int r = mm128_build_image(tap, n, image128);
            if (r == MM128_OK) ok128 = 1;
            else if (r == MM128_ERR_TAP) { err1 = "ManicMiner128.tap: not a"; err2 = "valid TAP file."; }
            else { err1 = "Unsupported Manic Miner 128"; err2 = "tape (SP version needed)."; }
        }
    }
    plat_log("MM: Manic Miner %s, Manic Miner 128 %s, 48.rom %s\n", ok48 ? "found" : "missing",
             ok128 ? "found" : "missing", have_rom ? "found" : "missing (built-in font)");
    if (!ok48 && !ok128) {
        if (!err1) { err1 = "Put ManicMiner.bin and/or"; err2 = "ManicMiner128.tap by the XBE."; }
        fatal_screen(err1, err2);
    }

    volatile int choose128 = ok128 && !ok48;
    chooser_available = ok48 && ok128;
    if (chooser_available) {
        /* Back twice on a game's intro lands here again. */
        if (setjmp(chooser_jmp)) {
            game_running = 0;
            last_back_frame = 0;
            macro_step = -1;
            teleport_pending = 0;
            v128 = 0;
            frame_start_level = 0;
            plat_log("MM: back to the chooser\n");
        }
        /* The chooser, built from Manic Miner's own title screen (so it
         * needs the 48K image in memory). */
        zx_reset();
        memset(mem, 0, sizeof mem);
        mm_install_font(have_rom ? rom : NULL);
        memcpy(mem + 0x4000, image48 + 0x4000, 0xC000);
        zx_set_model(ZX_MODEL_48K);
        choose128 = launcher();
        in_chooser = 0;
    }
    v128 = choose128;
    game_running = 1;

    /* Start the chosen game from a clean machine. */
    zx_reset();
    stats_t0_us = 0;
    memset(mem, 0, sizeof mem);
    mm_install_font(have_rom ? rom : NULL);
    zx_in_filter = teleport_in_filter;
    hs_init();
    mm_game_over_hook = hs_game_over;
    if (v128) {
        zx_set_model(ZX_MODEL_128K);
        ay_init(&ay, AY_CLOCK_128K, AUDIO_RATE);
        memcpy(mem + 0x4000, image128 + 0x4000, 0xC000);   /* keep our ROM area */
        mm128_menu_labels();
        hs_begin_game();
        screen_shown("game128");
        mm_run128();                                        /* never returns */
    }
    zx_set_model(ZX_MODEL_48K);
    memcpy(mem + 0x4000, image48 + 0x4000, 0xC000);
    hs_begin_game();
    screen_shown("game48");
    mm_run48();                                             /* never returns */
    return 0;
}
