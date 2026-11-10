#pragma once
#include "hop_windows/layout_editor.h"
#include <cstdint>
#include <utility>
#include <vector>
namespace hop::windows {
// Offline persistence deliberately does not preserve online/trusted status;
// these must be re-established by authenticated live sessions.
bool encode_layout(const LayoutEditor& editor,std::vector<std::uint8_t>& bytes);
bool decode_layout(const std::vector<std::uint8_t>& bytes,LayoutEditor& editor);
class LayoutStorage {
public:
    virtual ~LayoutStorage()=default;
    virtual bool load(std::vector<std::uint8_t>& bytes,bool& found)=0;
    virtual bool save(const std::vector<std::uint8_t>& bytes)=0;
};
class RegistryLayoutStorage final : public LayoutStorage {
public:
    explicit RegistryLayoutStorage(std::wstring value_name=L"Layout") : value_name_(std::move(value_name)) {}
    bool load(std::vector<std::uint8_t>& bytes,bool& found) override;
    bool save(const std::vector<std::uint8_t>& bytes) override;
private:
    std::wstring value_name_;
};
bool save_layout(LayoutStorage& storage,const LayoutEditor& editor);
// Missing record is successful and leaves editor unchanged. Corrupt records
// fail without replacing the prior in-memory layout.
bool load_layout(LayoutStorage& storage,LayoutEditor& editor,bool& found);
} // namespace hop::windows
