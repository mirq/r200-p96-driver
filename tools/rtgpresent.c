/*
 * RTGPresent - driver-failsafe helper for S:startup-sequence (2026-09-18).
 *
 * Returns 0 when the default public screen is an RTG (Picasso96) screen,
 * or 5 (WARN) when it is not (PAL fallback, or no Workbench screen yet).
 * The startup sequence keeps S:driver-boot-failed while this returns WARN,
 * so a driver that fails to initialise and leaves the machine on the
 * native display still gets restored on the following boot.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>

static BOOL IsRtg(struct Screen *screen)
{
    ULONG mode = GetVPModeID(&screen->ViewPort);

    /* RTG (chunky) modes carry the 0x80000000 flag in their mode ID. Keep a
     * size fallback for any mode that reports a native-looking ID. */
    if (mode & 0x80000000UL)
        return TRUE;
    if (screen->Width > 900 || screen->Height > 600)
        return TRUE;
    return FALSE;
}

int main(void)
{
    struct Screen *screen = NULL;
    ULONG tries;
    BOOL rtg;

    for (tries = 0; tries < 150; ++tries) {
        screen = LockPubScreen(NULL);
        if (screen)
            break;
        Delay(10); /* 200 ms per attempt, up to 30 s */
    }
    if (!screen)
        return 5;
    rtg = IsRtg(screen);
    UnlockPubScreen(NULL, screen);
    return rtg ? 0 : 5;
}
