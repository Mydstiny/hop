#pragma once
#include "hop_windows/pairing_review.h"
#include "hop_windows/ui_state.h"
#include <windows.h>
#include <string>
namespace hop::windows {
// Text helper for a UI-only prompt; fingerprints must be rendered in full.
std::wstring pairing_prompt_text(UiLanguage language,const std::string& local,const std::string& remote,bool after_tls);
// Invoke only in response to an explicit user pairing action, on the UI
// thread. This records temporary approval only and never persists trust.
bool show_candidate_fingerprint_prompt(HWND owner,UiLanguage language,PairingReview& review);
// Called after the pinned TLS handshake. The return value must be marshalled to
// the serial session worker to invoke confirm_pairing/cancel_pairing.
bool show_tls_pair_confirmation(HWND owner,UiLanguage language,const PairingReview& review);
}
