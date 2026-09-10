#pragma once
// Minimal crash-survivable logger for the PS Vita build.
// Every call does an open/write/close (see vita_klog.cpp for why); this is
// only used at a handful of lifecycle checkpoints, never in a hot path.
#ifdef __cplusplus
extern "C" {
#endif
void KK4V_Log(const char *msg);
#ifdef __cplusplus
}
#endif
