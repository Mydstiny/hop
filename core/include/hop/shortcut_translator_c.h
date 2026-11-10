#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HOP_SHORTCUT_MAX_EVENTS 240
typedef struct hop_shortcut_translator hop_shortcut_translator;
typedef struct hop_key_event { uint16_t usage, modifiers; uint8_t down, repeat; } hop_key_event;
// Platforms: 1 macOS, 2 Windows, 3 Linux, 4 HarmonyOS. Mode 0=physical,
// 1=common shortcuts. The caller owns one serialized instance per focus.
hop_shortcut_translator* hop_shortcut_create(int source, int target, int mode);
// 0 success, -1 failure. Output capacity must be at least MAX_EVENTS even if
// a particular call emits fewer. Any error requires release and disconnect.
int hop_shortcut_push(hop_shortcut_translator*, const hop_key_event*, hop_key_event*, size_t capacity, size_t* count);
// Call before pointer/scroll events to preserve source modifier behavior.
int hop_shortcut_flush_pointer(hop_shortcut_translator*, hop_key_event*, size_t capacity, size_t* count);
// Call at focus end; unsent pending modifiers are discarded, sent keys released.
int hop_shortcut_release(hop_shortcut_translator*, hop_key_event*, size_t capacity, size_t* count);
void hop_shortcut_free(hop_shortcut_translator*);
#ifdef __cplusplus
}
#endif
