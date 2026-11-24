/* LiteKern Go: the shell's own sounds (kernel/machine.h, machine_ui_sound): short effects from the SDK's synthesizer,
 * played by a helper process so the shell never waits. Only where there is a sound card (not in the PC tests). */
#include <stdlib.h>
#include <unistd.h>

#include "kernel/machine.h"
#include "sdk/sfx.h"

static int muted;

void machine_ui_mute(int m)
{
    muted = m;
}

void machine_ui_sound(int id)
{
    static int have = -1;
    if (have < 0)
        have = access("/dev/snd/pcmC0D2p", F_OK) == 0;
    int vol = machine_volume();
    if (!have || muted || vol <= 0)
        return;
    switch (id) {
    case UI_TAP:     k86_sfx_shot(SFX_TAP, 0); break;
    case UI_BACK:    k86_sfx_shot(SFX_BACK, 0); break;
    case UI_LOCK:    k86_sfx_shot(SFX_LOCK, 0); break;
    case UI_UNLOCK:  k86_sfx_shot(SFX_UNLOCK, 0); break;
    case UI_SHUTTER: k86_sfx_shot(SFX_SHUTTER, 0); break;
    case UI_BOOT:    k86_sfx_shot(SFX_BOOT, 0); break;
    case UI_NOTIFY:  k86_sfx_shot(SFX_NOTIFY, 0); break;
    case UI_VOLUME:  k86_sfx_shot(SFX_TAP, vol / 10); break;      /* the tick rises with the level */
    case UI_ON:      k86_sfx_shot(SFX_RESUME, 0); break;
    case UI_OFF:     k86_sfx_shot(SFX_PAUSE, 0); break;
    default: break;
    }
}
