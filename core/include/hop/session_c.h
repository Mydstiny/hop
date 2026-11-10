#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct hop_v2_session hop_v2_session;
typedef struct hop_v2_screen { uint64_t id; int32_t x,y; uint32_t width,height; } hop_v2_screen;
typedef struct hop_v2_callbacks {
    int (*enter)(void*,const uint8_t*,size_t,uint64_t);
    int (*input)(void*,const uint8_t*,size_t,uint64_t);
    int (*restore)(void*);
    int (*leave)(void*,uint64_t,uint16_t,uint16_t);
    void (*clipboard_revoke)(void*);
    int (*clipboard_sample)(void*,uint64_t*);
    int (*clipboard_apply)(void*,uint8_t,uint64_t,uint64_t,const uint8_t*,size_t,uint64_t);
} hop_v2_callbacks;
// Single-threaded worker only. authenticated is an assertion made by an owner
// that has verified the exact peer certificate on this same TLS connection.
// Never pass 1 for a candidate, unauthenticated or closed socket. Callbacks are
// non-reentrant, bounded, and return 1 on success. Caller's user context must
// outlive the session. role: 1 host, 2 receiver; platform: 1 Mac, 2 Windows,
// 3 Linux, 4 HarmonyOS. Screen IDs are nonzero and unique, at most 64.
hop_v2_session* hop_v2_create(int role,int platform,uint64_t session_epoch,const uint64_t* screens,size_t count,uint64_t capabilities,hop_v2_callbacks callbacks,void* user);
// Convenience entry for language FFIs that cannot pass a C callback struct by
// value. Clipboard is intentionally disabled; callbacks obey the same rules.
typedef int (*hop_v2_event_callback)(void*,const uint8_t*,size_t,uint64_t);
typedef int (*hop_v2_restore_callback)(void*);
typedef int (*hop_v2_leave_callback)(void*,uint64_t,uint16_t,uint16_t);
hop_v2_session* hop_v2_create_input_only(int role,int platform,uint64_t session_epoch,const uint64_t* screens,size_t count,uint64_t capabilities,
    hop_v2_event_callback enter,hop_v2_event_callback input,hop_v2_restore_callback restore,hop_v2_leave_callback leave,void* user);
typedef void (*hop_v2_clipboard_revoke_callback)(void*);
typedef int (*hop_v2_clipboard_sample_callback)(void*,uint64_t*);
typedef int (*hop_v2_clipboard_apply_callback)(void*,uint8_t,uint64_t,uint64_t,const uint8_t*,size_t,uint64_t);
hop_v2_session* hop_v2_create_with_clipboard(int role,int platform,uint64_t session_epoch,const uint64_t* screens,size_t count,uint64_t capabilities,
    hop_v2_event_callback enter,hop_v2_event_callback input,hop_v2_restore_callback restore,hop_v2_leave_callback leave,
    hop_v2_clipboard_revoke_callback revoke,hop_v2_clipboard_sample_callback sample,hop_v2_clipboard_apply_callback apply,void* user);
int hop_v2_bind_peer(hop_v2_session*,const char* uppercase_fingerprint);
int hop_v2_require_pairing(hop_v2_session*);
int hop_v2_confirm_pairing(hop_v2_session*);
int hop_v2_cancel_pairing(hop_v2_session*);
int hop_v2_pairing_confirmed(const hop_v2_session*);
int hop_v2_set_key_mode(hop_v2_session*,int mode); // 0 physical, 1 common shortcuts
int hop_v2_enable_clipboard(hop_v2_session*,int text,int png);
int hop_v2_start(hop_v2_session*,int authenticated,uint64_t monotonic_ms);
int hop_v2_receive(hop_v2_session*,int authenticated,const uint8_t* bytes,size_t count,uint64_t monotonic_ms);
int hop_v2_tick(hop_v2_session*,int authenticated,uint64_t monotonic_ms);
// 1 frame available, 0 no frame, -1 failure. If capacity is too small, sets
// required length without consuming. Caller clears the copied encoded bytes.
int hop_v2_pull(hop_v2_session*,int authenticated,uint8_t* output,size_t capacity,size_t* length);
int hop_v2_advertise(hop_v2_session*,const uint8_t* payload,size_t length);
int hop_v2_enter(hop_v2_session*,uint64_t epoch,uint64_t screen,uint16_t x,uint16_t y);
int hop_v2_release(hop_v2_session*,uint8_t reason);
// After clipboard revocation barrier completes, on the session worker thread.
int hop_v2_complete_release(hop_v2_session*,uint64_t monotonic_ms);
int hop_v2_request_leave(hop_v2_session*,uint16_t x,uint16_t y);
int hop_v2_send_input(hop_v2_session*,const uint8_t* payload,size_t length);
int hop_v2_clipboard_ready(hop_v2_session*);
int hop_v2_clipboard_pause(hop_v2_session*);
uint64_t hop_v2_clipboard_formats(const hop_v2_session*); // negotiated HELLO mask, bit 1 text and bit 2 PNG
int hop_v2_clipboard_available(const hop_v2_session*);
int hop_v2_clipboard_initial_done(hop_v2_session*);
int hop_v2_clipboard_initial_received(const hop_v2_session*);
int hop_v2_clipboard_initial_drained(hop_v2_session*);
int hop_v2_clipboard_start(hop_v2_session*,uint64_t id,uint8_t format,uint32_t total);
int hop_v2_clipboard_chunk(hop_v2_session*,uint64_t id,const uint8_t* data,size_t size);
int hop_v2_clipboard_cancel(hop_v2_session*,uint64_t id,uint8_t reason);
// Phase values correspond to the declared enum order in session.h.
// Returns the required count; copies only if capacity is sufficient.
// Call from the session's serialized owner thread, never concurrently.
size_t hop_v2_peer_screens(const hop_v2_session*,hop_v2_screen* output,size_t capacity);
int hop_v2_peer_screen_at(const hop_v2_session*,size_t index,uint64_t* id,int32_t* x,int32_t* y,uint32_t* width,uint32_t* height);
int hop_v2_phase(const hop_v2_session*);
uint64_t hop_v2_focus_epoch(const hop_v2_session*);
void hop_v2_disconnect(hop_v2_session*);
void hop_v2_free(hop_v2_session*);
#ifdef __cplusplus
}
#endif
