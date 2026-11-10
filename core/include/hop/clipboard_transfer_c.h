#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct hop_clipboard_assembler hop_clipboard_assembler;
hop_clipboard_assembler* hop_clipboard_create(void);
void hop_clipboard_focus(hop_clipboard_assembler*, uint64_t epoch);
// Format 1 = UTF-8, 2 = PNG. 0 success, -1 failure. Application validates
// authentication, authorization and content independently.
int hop_clipboard_begin(hop_clipboard_assembler*, uint64_t epoch, uint64_t id, uint8_t format, size_t total);
int hop_clipboard_chunk(hop_clipboard_assembler*, uint64_t epoch, uint64_t id, size_t offset, const uint8_t* data, size_t length);
// Returns 1 if complete, 0 if not, -1 on invalid arguments. No content leaves.
int hop_clipboard_info(const hop_clipboard_assembler*, uint8_t* format, uint64_t* epoch, uint64_t* id, size_t* size);
// Copies into caller-owned memory and erases the internal copy; capacity must
// be sufficient. Caller must clear its own output after use. Empty text allows
// a null output with capacity zero.
int hop_clipboard_take(hop_clipboard_assembler*, uint8_t* output, size_t capacity);
void hop_clipboard_cancel(hop_clipboard_assembler*);
void hop_clipboard_free(hop_clipboard_assembler*);
#ifdef __cplusplus
}
#endif
