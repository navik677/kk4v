#include "vita_klog.h"

#include <psp2/io/fcntl.h>
#include <string.h>

// See vita_klog.h. freopen(stdout)+fflush+sceIoSync("ux0:",0) was tried
// first and produced a persistent 0-byte log file on real hardware --
// sceIoSync's "device" argument wants a raw block-device name (e.g.
// "sdstor0:..."), not the logical mount "ux0:", so the sync silently did
// nothing and cached writes never reached storage.
//
// This sidesteps all of that: every call does a fresh sceIoOpen (append), a
// single sceIoWrite, and sceIoClose. Closing a Vita file descriptor is a
// synchronous operation guaranteed to flush that file's data, so this
// survives an abrupt kill/watchdog even though it's slower per line
// (irrelevant here -- only a handful of lifecycle checkpoints get logged).
#define KLOG_PATH "ux0:data/kirikiroid2/log.txt"

void KK4V_Log(const char *msg) {
    SceUID fd = sceIoOpen(KLOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, msg, strlen(msg));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}
