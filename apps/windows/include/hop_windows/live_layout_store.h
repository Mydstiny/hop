#pragma once
#include "hop_windows/layout_store.h"

namespace hop::windows {
// Geometry is sampled from the current authenticated connection, before any
// user placement. It must match in full before restoring saved placements.
enum class LiveLayoutRestore { restored, missing, geometry_mismatch, error };
bool encode_live_layout(const LayoutEditor& editor, const std::vector<hop::Screen>& geometry,
                        std::vector<std::uint8_t>& bytes);
LiveLayoutRestore decode_live_layout(const std::vector<std::uint8_t>& bytes,
                                     const std::vector<hop::Screen>& geometry, LayoutEditor& editor);
// Preserve an unreadable or invalid record. A valid older geometry may be
// replaced by an explicit edit after a change in display configuration.
bool save_live_layout(LayoutStorage& storage, const LayoutEditor& editor, const std::vector<hop::Screen>& geometry);
// Apply positions and expansion only to an initialized editor for this
// geometry. Names, device identity, trust and online status come from it.
LiveLayoutRestore load_live_layout(LayoutStorage& storage, const std::vector<hop::Screen>& geometry, LayoutEditor& editor);
} // namespace hop::windows
