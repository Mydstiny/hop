#include "hop/clipboard_transfer_c.h"
#include "hop/clipboard_transfer.h"
#include <algorithm>
#include <memory>
struct hop_clipboard_assembler { hop::ClipboardAssembler value; };
extern "C" hop_clipboard_assembler* hop_clipboard_create() { try { return new hop_clipboard_assembler; } catch (...) { return nullptr; } }
extern "C" void hop_clipboard_focus(hop_clipboard_assembler* a,uint64_t epoch) { if(a) a->value.focus(epoch); }
extern "C" int hop_clipboard_begin(hop_clipboard_assembler* a,uint64_t epoch,uint64_t id,uint8_t format,size_t total) {
    if(!a || (format!=1 && format!=2)) return -1;
    try { return a->value.begin(epoch,id,static_cast<hop::ClipboardFormat>(format),total) ? 0 : -1; } catch (...) { a->value.cancel(); return -1; }
}
extern "C" int hop_clipboard_chunk(hop_clipboard_assembler* a,uint64_t epoch,uint64_t id,size_t offset,const uint8_t* data,size_t length) {
    if(!a) return -1;
    try { return a->value.chunk(epoch,id,offset,data,length) ? 0 : -1; } catch (...) { a->value.cancel(); return -1; }
}
extern "C" int hop_clipboard_info(const hop_clipboard_assembler* a,uint8_t* format,uint64_t* epoch,uint64_t* id,size_t* size) {
    if(!a || !format || !epoch || !id || !size) return -1;
    *format=0;*epoch=0;*id=0;*size=0;
    const auto* value=a->value.peek(); if(!value) return 0;
    *format=static_cast<uint8_t>(value->format);*epoch=value->epoch;*id=value->transfer_id;*size=value->data.size();return 1;
}
extern "C" int hop_clipboard_take(hop_clipboard_assembler* a,uint8_t* output,size_t capacity) {
    if(!a) return -1;
    const auto* view=a->value.peek();
    if(!view || capacity<view->data.size() || (!output && !view->data.empty())) return -1;
    auto value=a->value.take(); if(!value) return -1;
    if(!value->data.empty()) std::copy(value->data.begin(),value->data.end(),output);
    volatile std::uint8_t* p=value->data.data(); for(std::size_t i=0;i<value->data.size();++i) p[i]=0;
    return 0;
}
extern "C" void hop_clipboard_cancel(hop_clipboard_assembler* a) { if(a) a->value.cancel(); }
extern "C" void hop_clipboard_free(hop_clipboard_assembler* a) { delete a; }
