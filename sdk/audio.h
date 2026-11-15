/* LiteKern Go: sound for apps. The phone's loudspeaker (the amplifier on the quaternary MI2S
 * port, see phone/kernel/0004-a5-sound-lpass.patch), played straight through the kernel's
 * sound driver: no sound server, no library.
 *
 *   k86_audio_open(48000);                      once
 *   n = k86_audio_write(frames, count);         from the app's timer: stereo, 16 bit,
 *                                               interleaved (left, right); returns how many
 *                                               frames were taken (0: full, try later)
 *   k86_audio_close();
 *
 * The volume is the one on the volume keys and the quick settings slider (0-100). It is applied
 * here, in software: the loudspeaker amplifier has no volume control of its own, and at full
 * scale it is LOUD. Write full-scale audio and let the volume bring it down. Apps that make a
 * sound should also keep it short and not start at a high level.
 *
 * A game needs only a few lines: k86_audio_tone(440, 80) is a beep.
 * Linux build only (the apps here run on Linux). */
#ifndef LKX_AUDIO_H
#define LKX_AUDIO_H

#include <stdint.h>

/* 0 or -errno (-ENODEV: no sound card). rate: 48000 is the one the hardware likes. */
int k86_audio_open(int rate);
/* Frames taken (they were played from a ring the driver owns), or -errno. Never waits. */
int k86_audio_write(const int16_t *frames, int count);
/* How many frames could be written right now without being refused. */
int k86_audio_space(void);
/* The ring's size in frames: k86_audio_space() equal to it means everything written has played. */
int k86_audio_buffer(void);
/* Throw away what's written but not played yet (a seek, a new track), to write new sound at once.
 * It does NOT silence the output: the hardware keeps playing the last of the ring over and over.
 * To stop the sound (a pause, the end), k86_audio_close(); open again to carry on. */
void k86_audio_flush(void);
void k86_audio_close(void);

/* The volume, 0-100, as the shell has it now. */
int k86_volume(void);

/* A short beep (a sine, with a soft start and end), played to the end before it returns:
 * for a click or a game's blip, not for music. */
void k86_audio_tone(int hz, int ms);

/* The same, but it returns at once (a child process plays it): for a game's blips. A second
 * note (hz2 > 0) follows the first. A beep asked for while the last is still playing is dropped. */
void k86_audio_beep(int hz1, int ms1, int hz2, int ms2);

/* The microphone: mono, 16 bit, no volume applied. A game or a call reads it from its timer.
 *   k86_mic_open(48000);  n = k86_mic_read(frames, count);  k86_mic_close();
 * Read returns how many frames were there (0: none yet, try later), never waits. Close it when
 * done: it also stops the recording. 0 or -errno from open. */
int k86_mic_open(int rate);
int k86_mic_read(int16_t *frames, int count);
void k86_mic_close(void);

#endif
