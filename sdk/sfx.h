/* LiteKern Go: richer sound for apps than a beep (sdk/sfx.c). A small mixer that makes its sounds in code (layered
 * tones that slide in pitch, noise through a filter, bells, chords) and a few looping tunes, all mixed together
 * and played through sdk/audio.h. No samples, no files, no malloc, whole numbers only.
 *
 *   k86_sfx_play(SFX_THUD, 0);      any time: starts a sound (it mixes with the others)
 *   k86_sfx_music(1);               a looping tune (-1 stops it)
 *   k86_sfx_pump();                 from the app's timer, every tick: makes and writes the next bit of sound
 *   k86_sfx_close();                stop it all and let go of the speaker (pause, leaving the app)
 *
 * The timer must keep running while k86_sfx_busy() says 1, or a sound is cut short. The volume is the
 * phone's (see audio.h). Linux build only. */
#ifndef LKX_SFX_H
#define LKX_SFX_H

enum k86_sfx {
    SFX_TAP,        /* a soft tick for a button */
    SFX_BACK,       /* a lower tick: back, cancel */
    SFX_WHOOSH,     /* something thrown: a rising rush of air */
    SFX_THUD,       /* something sticking into wood or fruit */
    SFX_CLINK,      /* metal on metal */
    SFX_SPLAT,      /* something wet bursting */
    SFX_LEVELUP,    /* a bright rising arpeggio */
    SFX_FAIL,       /* a sad falling note with a low thump */
    SFX_WIN,        /* a fanfare */
    SFX_PAUSE,      /* two falling notes */
    SFX_RESUME,     /* two rising notes */
    SFX_COIN,       /* a two-note sparkle: points */
    SFX_CRASH,      /* a car crash: a burst of noise, a boom and a clang */
    SFX_LOCK,       /* the phone locking: two soft falling bells */
    SFX_UNLOCK,     /* the phone unlocking: three soft rising bells */
    SFX_NOTIFY,     /* a two-note chime */
    SFX_SHUTTER,    /* a camera shutter: two clicks */
    SFX_BOOT,       /* the start-up chime */
    SFX_KEY,        /* a keyboard key: a soft low "thock" with a tiny click on top (vary semis 0..3 so it never repeats exactly) */
    SFX_KEY_SPACE,  /* the space bar and Enter: deeper */
    SFX_KEY_DEL,    /* Delete: a lighter tick */
    SFX_COUNT
};

/* semis: shift the pitch up this many semitones (0..24), e.g. to rise with a combo. */
void k86_sfx_play(int id, int semis);
/* The tunes: 0 bouncy and bright, 1 slow and icy, 2 driving and dark, 3 dreamy and spacey. -1: none. */
void k86_sfx_music(int tune);
void k86_sfx_pump(void);
/* The mixer's overall level, percent (100 at first): for sounds meant to sit under everything else. */
void k86_sfx_gain(int pct);
/* 1 while anything is still sounding (keep the timer going). */
int k86_sfx_busy(void);
void k86_sfx_close(void);

/* For apps whose timer isn't always running: plays one sound at once, with no pump, from a small helper process
 * (like k86_audio_beep). A small sound asked for while another plays is dropped; FAIL, WIN, LEVELUP and CRASH
 * cut off whatever is playing. Don't mix this with k86_sfx_play in one app. */
void k86_sfx_shot(int id, int semis);

#endif
