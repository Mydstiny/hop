#pragma once
#include "hop/frame.h"
namespace hop {
// Structural checks for docs/V2_SESSION.md payloads. Does not authenticate,
// authorize capabilities, manage focus, or validate clipboard content.
bool valid_wire_payload(const Frame& frame);
}
