#include "hop/shortcut_translator_c.h"
#include "hop/shortcut_translator.h"
#include <memory>
#include <vector>
struct hop_shortcut_translator { hop::ShortcutTranslator value; hop_shortcut_translator(hop::Platform s, hop::Platform t, hop::KeyMode m):value(s,t,m){} };
namespace {
hop::Platform platform(int p) {
    switch (p) { case 1: return hop::Platform::macos; case 2: return hop::Platform::windows; case 3: return hop::Platform::linux_desktop; default: return hop::Platform::harmonyos; }
}
bool output_ok(hop_key_event* out, size_t capacity, size_t* count) { if (count) *count=0; return out && capacity>=HOP_SHORTCUT_MAX_EVENTS && count; }
int copy(const std::vector<hop::KeyEvent>& events, hop_key_event* out, size_t* count) {
    if (events.size()>HOP_SHORTCUT_MAX_EVENTS) return -1;
    for (std::size_t i=0;i<events.size();++i) out[i]={events[i].usage,events[i].modifiers,static_cast<uint8_t>(events[i].down),static_cast<uint8_t>(events[i].repeat)};
    *count=events.size(); return 0;
}
}
extern "C" hop_shortcut_translator* hop_shortcut_create(int source,int target,int mode) {
    if (source<1 || source>4 || target<1 || target>4 || (mode!=0 && mode!=1)) return nullptr;
    try { return new hop_shortcut_translator(platform(source),platform(target),mode ? hop::KeyMode::common_shortcuts : hop::KeyMode::physical); } catch (...) { return nullptr; }
}
extern "C" int hop_shortcut_push(hop_shortcut_translator* t,const hop_key_event* event,hop_key_event* out,size_t capacity,size_t* count) {
    if (!output_ok(out,capacity,count) || !t || !event || event->down>1 || event->repeat>1) return -1;
    try { auto result=t->value.push({event->usage,event->modifiers,event->down!=0,event->repeat!=0}); return result ? copy(*result,out,count) : -1; } catch (...) { return -1; }
}
extern "C" int hop_shortcut_flush_pointer(hop_shortcut_translator* t,hop_key_event* out,size_t capacity,size_t* count) {
    if (!output_ok(out,capacity,count) || !t || t->value.failed()) return -1;
    try { return copy(t->value.flush_for_pointer(),out,count); } catch (...) { return -1; }
}
extern "C" int hop_shortcut_release(hop_shortcut_translator* t,hop_key_event* out,size_t capacity,size_t* count) {
    if (!output_ok(out,capacity,count) || !t) return -1;
    try { return copy(t->value.release_all(),out,count); } catch (...) { return -1; }
}
extern "C" void hop_shortcut_free(hop_shortcut_translator* t) { delete t; }
