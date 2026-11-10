#include "hop/focus.h"
#include "hop/clipboard_transfer.h"
#include "hop/clipboard_transfer_c.h"
#include "hop/frame.h"
#include "hop/outbound_queue.h"
#include "hop/wire_validation.h"
#include "hop/shortcut_translator.h"
#include "hop/shortcut_translator_c.h"
#include "hop/input_policy.h"
#include "hop/layout.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int checks = 0;
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        std::cerr << "failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}
#define CHECK(expr) check(bool(expr), #expr, __LINE__)
void layout_tests() {
    using namespace hop;
    Layout layout;
    std::string error;
    // Adjacent right-hand screens split an edge; a lower screen touches a corner.
    std::vector<Screen> screens = {{"a", "mac", 0, 0, 100, 100},
                                   {"b", "harmony", 100, 0, 200, 50},
                                   {"c", "linux", 100, 50, 50, 50},
                                   {"d", "windows", 0, -100, 100, 100},
                                   {"corner", "other", 100, 100, 50, 50}};
    CHECK(layout.replace(screens, &error));
    CHECK(error.empty());
    auto b = layout.cross("a", Edge::right, .25);
    CHECK(b && b->screen_id == "b" && b->device_id == "harmony");
    CHECK(b->entry_edge == Edge::left && std::abs(b->position - .5) < 1e-12);
    auto boundary = layout.cross("a", Edge::right, .5);
    CHECK(boundary && boundary->screen_id == "c" && boundary->position == 0);
    auto last = layout.cross("a", Edge::right, 1);
    CHECK(last && last->screen_id == "c" && last->position < 1);
    auto d = layout.cross("a", Edge::top, .25);
    CHECK(d && d->screen_id == "d" && d->entry_edge == Edge::bottom);
    auto a = layout.cross("d", Edge::bottom, .25);
    CHECK(a && a->screen_id == "a" && a->entry_edge == Edge::top);
    auto back = layout.cross("b", Edge::left, .5);
    CHECK(back && back->screen_id == "a" && back->entry_edge == Edge::right);
    CHECK(std::abs(back->position - .25) < 1e-12);
    CHECK(!layout.cross("a", Edge::left, .2));
    CHECK(!layout.cross("missing", Edge::left, .2));
    CHECK(!layout.cross("a", Edge::right, -0.01));
    CHECK(!layout.cross("a", Edge::right, 1.01));
    CHECK(!layout.cross("a", Edge::right, std::numeric_limits<double>::infinity()));
    CHECK(!layout.cross("a", Edge::right, std::numeric_limits<double>::quiet_NaN()));
    auto invalid = screens;
    invalid.push_back({"overlap", "x", 99, 99, 3, 3});
    CHECK(!layout.replace(invalid, &error));
    CHECK(error == "overlapping screens");
    CHECK(layout.screens().size() == screens.size()); // atomic replacement
    Layout corner_only;
    CHECK(corner_only.replace({{"source", "one", 0, 0, 100, 100}, {"corner", "two", 100, 100, 20, 20}}));
    CHECK(!corner_only.cross("source", Edge::right, 1));
    invalid = screens;
    invalid[1].id = "a";
    CHECK(!layout.replace(invalid, &error));
    CHECK(error == "duplicate screen id");
    invalid = screens;
    invalid[0].width = 0;
    CHECK(!layout.replace(invalid));
    invalid = screens;
    invalid[0].x = std::numeric_limits<std::int32_t>::max();
    CHECK(!layout.replace(invalid));
    invalid = screens;
    invalid[0].device_id.clear();
    CHECK(!layout.replace(invalid));
    CHECK(layout.replace({}, &error));
    CHECK(error.empty() && layout.screens().empty());
}
void focus_tests() {
    using namespace hop;
    Focus focus;
    CHECK(focus.phase() == FocusPhase::local);
    CHECK(!focus.switch_to(""));
    auto enter = focus.switch_to("harmony");
    CHECK(enter && enter->action == FocusAction::enter_remote);
    CHECK(!focus.can_forward_to("harmony"));
    CHECK(!focus.entered(enter->generation + 1, "harmony"));
    CHECK(!focus.entered(enter->generation, "linux"));
    CHECK(!focus.switch_to("linux"));
    CHECK(focus.entered(enter->generation, "harmony"));
    CHECK(focus.can_forward_to("harmony") && !focus.can_forward_to("linux"));
    CHECK(!focus.switch_to("harmony"));
    auto release = focus.switch_to("linux");
    CHECK(release && release->action == FocusAction::release_remote && release->device_id == "harmony");
    CHECK(!focus.can_forward_to("harmony") && !focus.can_forward_to("linux"));
    CHECK(!focus.released(enter->generation, "harmony"));
    CHECK(!focus.released(release->generation, "linux"));
    auto enter2 = focus.released(release->generation, "harmony");
    CHECK(enter2 && enter2->action == FocusAction::enter_remote && enter2->device_id == "linux");
    CHECK(!focus.can_forward_to("linux"));
    CHECK(focus.entered(enter2->generation, "linux"));
    CHECK(focus.can_forward_to("linux"));
    auto aborted = focus.abort();
    CHECK(aborted.action == FocusAction::restore_local && aborted.device_id == "linux");
    CHECK(focus.phase() == FocusPhase::local && !focus.can_forward_to("linux"));
    CHECK(!focus.entered(enter2->generation, "linux"));
    auto enter3 = focus.switch_to("windows");
    CHECK(enter3);
    auto abort_pending = focus.abort();
    CHECK(abort_pending.device_id == "windows");
    CHECK(!focus.entered(enter3->generation, "windows"));
}
void frame_tests() {
    using namespace hop;
    const Frame original{FrameKind::input, 1, 42, {0x01, 0x02, 0x03}};
    const std::vector<std::uint8_t> golden = {
        'H','O','P','2',2,2,0,0, 0,0,0,3,
        0,0,0,0,0,0,0,1, 0,0,0,0,0,0,0,42, 1,2,3
    };
    CHECK(encode_frame(original) == golden);
    for (std::size_t split = 0; split <= golden.size(); ++split) {
        std::vector<Frame> received;
        FrameDecoder d([&](Frame&& f) { received.push_back(std::move(f)); return true; });
        CHECK(d.feed(golden.data(), split));
        CHECK(d.feed(golden.data() + split, golden.size() - split));
        CHECK(d.finish());
        CHECK(received.size() == 1 && received[0].sequence == 1 && received[0].epoch == 42 && received[0].payload == original.payload);
        CHECK(!d.feed(golden.data(), golden.size()));
    }
    std::vector<Frame> received;
    FrameDecoder d([&](Frame&& f) { received.push_back(std::move(f)); return true; });
    auto second = encode_frame({FrameKind::control, 2, 42, {}});
    auto joined = golden; joined.insert(joined.end(), second.begin(), second.end());
    CHECK(d.feed(joined.data(), joined.size()));
    CHECK(d.finish() && received.size() == 2 && received[1].payload.empty());
    FrameDecoder replay([](Frame&&) { return true; });
    CHECK(replay.feed(golden.data(), golden.size()));
    CHECK(!replay.feed(golden.data(), golden.size()) && replay.failed());
    CHECK(!replay.finish());
    FrameDecoder truncated([](Frame&&) { return true; });
    CHECK(truncated.feed(golden.data(), golden.size() - 1));
    CHECK(!truncated.finish());
    for (std::size_t offset : {0u, 4u, 5u, 6u, 7u}) {
        auto bad = golden; bad[offset] = 0xff;
        FrameDecoder invalid([](Frame&&) { return true; });
        CHECK(!invalid.feed(bad.data(), bad.size()));
    }
    auto huge = golden; huge[8] = 0xff;
    FrameDecoder invalid_length([](Frame&&) { return true; });
    CHECK(!invalid_length.feed(huge.data(), frame_header_size));
    CHECK(encode_frame({FrameKind::clipboard_chunk, 1, 1, {}}).empty());
    CHECK(encode_frame({FrameKind::input, 0, 1, {}}).empty());
    CHECK(encode_frame({FrameKind::input, 1, 0, {}}).empty());
    CHECK(encode_frame({FrameKind::input, 1, 1, std::vector<std::uint8_t>(max_control_payload + 1)}).empty());
    CHECK(encode_frame({FrameKind::clipboard_chunk, 1, 1, std::vector<std::uint8_t>(max_clipboard_chunk)}).size() == frame_header_size + max_clipboard_chunk);
    CHECK(encode_frame({FrameKind::clipboard_chunk, 1, 1, std::vector<std::uint8_t>(max_clipboard_chunk + 1)}).empty());
    FrameDecoder rejected([](Frame&&) { return false; });
    CHECK(!rejected.feed(golden.data(), golden.size()));
    FrameDecoder throws([](Frame&&) -> bool { throw 1; });
    CHECK(!throws.feed(golden.data(), golden.size()) && throws.failed());
    FrameDecoder null_input([](Frame&&) { return true; });
    CHECK(!null_input.feed(nullptr, 1));
}
void outbound_queue_tests() {
    using namespace hop;
    OutboundQueue queue;
    CHECK(!queue.pop());
    CHECK(!queue.enqueue(FrameKind::input, 0, {}));
    CHECK(queue.enqueue(FrameKind::clipboard_control, 4, {9}));
    CHECK(queue.enqueue(FrameKind::clipboard_chunk, 4, {1, 2}));
    CHECK(queue.enqueue(FrameKind::input, 4, {7}));
    CHECK(queue.enqueue(FrameKind::control, 4, {8}));
    auto a = queue.pop(), b = queue.pop(), c = queue.pop(), d = queue.pop();
    CHECK(a && a->kind == FrameKind::input && a->sequence == 1);
    CHECK(b && b->kind == FrameKind::control && b->sequence == 2);
    CHECK(c && c->kind == FrameKind::clipboard_control && c->sequence == 3);
    CHECK(d && d->kind == FrameKind::clipboard_chunk && d->sequence == 4);
    CHECK(queue.clipboard_bytes() == 0 && !queue.pop());
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(queue.enqueue(FrameKind::clipboard_chunk, 4, std::vector<std::uint8_t>(max_clipboard_chunk)));
    CHECK(queue.clipboard_bytes() == OutboundQueue::max_clipboard_bytes);
    CHECK(!queue.enqueue(FrameKind::clipboard_chunk, 4, {1}));
    queue.discard_clipboard();
    CHECK(queue.clipboard_bytes() == 0 && !queue.pop());
    for (std::size_t i = 0; i < OutboundQueue::max_interactive_frames; ++i)
        CHECK(queue.enqueue(FrameKind::input, 5, {1}));
    CHECK(!queue.enqueue(FrameKind::input, 5, {2}));
    CHECK(queue.pop()->sequence == 5);
    CHECK(queue.enqueue(FrameKind::input, 5, {2}));
    CHECK(queue.discard_input(5) == OutboundQueue::max_interactive_frames);
    CHECK(queue.enqueue(FrameKind::control, 5, {3}));
    CHECK(queue.enqueue(FrameKind::input, 6, {4}));
    auto control = queue.pop(); auto next = queue.pop();
    CHECK(control && control->kind == FrameKind::control && control->epoch == 5 && control->sequence == 6);
    CHECK(next && next->kind == FrameKind::input && next->epoch == 6 && next->sequence == 7);
    CHECK(queue.discard_input(5) == 0);
}
void clipboard_transfer_tests() {
    using namespace hop;
    ClipboardAssembler assembler;
    const std::uint8_t bytes[] = {1,2,3,4};
    CHECK(!assembler.begin(1, 1, ClipboardFormat::text, 4));
    assembler.focus(1);
    CHECK(!assembler.begin(1, 1, ClipboardFormat::png, 0));
    CHECK(!assembler.begin(1, 1, ClipboardFormat::text, ClipboardAssembler::max_text + 1));
    CHECK(!assembler.begin(1, 1, ClipboardFormat::png, ClipboardAssembler::max_png + 1));
    CHECK(!assembler.begin(1, 1, static_cast<ClipboardFormat>(99), 1));
    CHECK(assembler.begin(1, 1, ClipboardFormat::text, 4));
    CHECK(!assembler.begin(1, 2, ClipboardFormat::text, 4));
    CHECK(assembler.chunk(1, 1, 0, bytes, 2) && !assembler.take());
    CHECK(assembler.chunk(1, 1, 2, bytes + 2, 2));
    CHECK(!assembler.begin(1, 2, ClipboardFormat::text, 4));
    auto value = assembler.take();
    CHECK(value && value->format == ClipboardFormat::text && value->epoch == 1 && value->transfer_id == 1 && value->data == std::vector<std::uint8_t>({1,2,3,4}));
    CHECK(!assembler.take() && !assembler.begin(1, 1, ClipboardFormat::text, 1));
    CHECK(assembler.begin(1, 2, ClipboardFormat::text, 0));
    value = assembler.take(); CHECK(value && value->data.empty());
    CHECK(assembler.begin(1, 3, ClipboardFormat::png, 4));
    CHECK(!assembler.chunk(1, 3, 1, bytes, 2));
    CHECK(!assembler.chunk(1, 3, 0, bytes, 4));
    CHECK(assembler.begin(1, 4, ClipboardFormat::png, 4));
    CHECK(assembler.chunk(1, 4, 0, bytes, 2));
    assembler.focus(2);
    CHECK(!assembler.chunk(1, 4, 2, bytes + 2, 2) && !assembler.take());
    CHECK(!assembler.begin(2, 4, ClipboardFormat::text, 0));
    CHECK(assembler.begin(2, 5, ClipboardFormat::text, 1));
    CHECK(!assembler.chunk(1, 4, 0, bytes, 1));
    CHECK(assembler.chunk(2, 5, 0, bytes, 1));
    CHECK(assembler.take()->data == std::vector<std::uint8_t>({1}));
    CHECK(assembler.begin(2, 8, ClipboardFormat::text, 1));
    CHECK(!assembler.chunk(2, 8, 0, bytes, 2));
    CHECK(assembler.begin(2, 9, ClipboardFormat::text, 1));
    CHECK(!assembler.chunk(2, 9, 0, nullptr, 1));
    CHECK(assembler.begin(2, 10, ClipboardFormat::text, 1));
    assembler.focus(0);
    CHECK(!assembler.chunk(2, 10, 0, bytes, 1) && !assembler.take());
    auto* c = hop_clipboard_create(); CHECK(c);
    uint8_t format=9; uint64_t epoch=9,id=9; std::size_t size=9;
    CHECK(hop_clipboard_info(c,&format,&epoch,&id,&size)==0 && size==0);
    hop_clipboard_focus(c,7);
    CHECK(hop_clipboard_begin(c,7,1,1,4)==0 && hop_clipboard_chunk(c,7,1,0,bytes,4)==0);
    CHECK(hop_clipboard_info(c,&format,&epoch,&id,&size)==1 && format==1 && epoch==7 && id==1 && size==4);
    uint8_t output[4]{};
    CHECK(hop_clipboard_take(c,output,3)==-1 && hop_clipboard_info(c,&format,&epoch,&id,&size)==1);
    CHECK(hop_clipboard_take(c,output,4)==0 && std::equal(output,output+4,bytes));
    CHECK(hop_clipboard_info(c,&format,&epoch,&id,&size)==0);
    CHECK(hop_clipboard_begin(c,7,2,1,0)==0 && hop_clipboard_take(c,nullptr,0)==0);
    hop_clipboard_free(c); hop_clipboard_free(nullptr);
}
void wire_validation_tests() {
    using namespace hop;
    auto frame = [](FrameKind kind, std::uint8_t op, std::size_t size) {
        Frame f{kind, 1, 2, std::vector<std::uint8_t>(size)};
        f.payload[0] = 1; f.payload[1] = op; return f;
    };
    auto hello=frame(FrameKind::control,1,16); hello.payload[4]=1; hello.payload[5]=1; hello.payload[15]=31;
    CHECK(valid_wire_payload(hello));
    hello.payload[15]=64; CHECK(!valid_wire_payload(hello)); hello.payload[15]=31;
    hello.payload[4]=2; CHECK(!valid_wire_payload(hello)); hello.payload[4]=1;
    hello.payload[2]=1; CHECK(!valid_wire_payload(hello)); hello.payload[2]=0;
    hello.payload[0]=2; CHECK(!valid_wire_payload(hello)); hello.payload[0]=1;
    hello.sequence=0; CHECK(!valid_wire_payload(hello)); hello.sequence=1;
    hello.payload.pop_back(); CHECK(!valid_wire_payload(hello));
    auto screens=frame(FrameKind::control,3,32); screens.payload[5]=1; screens.payload[15]=1; screens.payload[27]=1; screens.payload[31]=1;
    CHECK(valid_wire_payload(screens));
    screens.payload[31]=0; CHECK(!valid_wire_payload(screens)); screens.payload[31]=1;
    screens.payload[15]=0; CHECK(!valid_wire_payload(screens)); screens.payload[15]=1;
    screens.payload.resize(56); screens.payload[5]=2;
    std::copy(screens.payload.begin()+8,screens.payload.begin()+32,screens.payload.begin()+32);
    CHECK(!valid_wire_payload(screens)); screens.payload[39]=2; CHECK(valid_wire_payload(screens));
    auto enter=frame(FrameKind::control,0x10,20); enter.payload[11]=1;
    CHECK(valid_wire_payload(enter)); enter.payload[16]=2; CHECK(!valid_wire_payload(enter));
    auto ack=frame(FrameKind::control,0x11,12); ack.payload[11]=1; CHECK(valid_wire_payload(ack));
    auto release=frame(FrameKind::control,0x12,8); release.payload[4]=2; CHECK(valid_wire_payload(release));
    release.payload[4]=3; CHECK(!valid_wire_payload(release));
    CHECK(valid_wire_payload(frame(FrameKind::control,0x13,4)));
    CHECK(valid_wire_payload(frame(FrameKind::control,0x04,4)));
    CHECK(valid_wire_payload(frame(FrameKind::control,0x05,4)));
    auto move=frame(FrameKind::input,1,16); move.payload[11]=1; CHECK(valid_wire_payload(move));
    auto button=frame(FrameKind::input,2,20); button.payload[11]=1; button.payload[16]=5; button.payload[17]=1; button.payload[18]=16;
    CHECK(valid_wire_payload(button)); button.payload[18]=0; CHECK(!valid_wire_payload(button));
    button.payload[16]=0; CHECK(!valid_wire_payload(button));
    auto wheel=frame(FrameKind::input,3,12); CHECK(valid_wire_payload(wheel));
    wheel.payload[4]=1; CHECK(!valid_wire_payload(wheel));
    auto key=frame(FrameKind::input,4,12); key.payload[5]=4; key.payload[8]=1; key.payload[9]=1;
    CHECK(valid_wire_payload(key)); key.payload[8]=0; CHECK(!valid_wire_payload(key));
    key.payload[9]=0; CHECK(valid_wire_payload(key)); key.payload[4]=1; CHECK(!valid_wire_payload(key));
    auto start=frame(FrameKind::clipboard_control,1,20); start.payload[11]=1; start.payload[12]=1;
    CHECK(valid_wire_payload(start)); start.payload[12]=2; CHECK(!valid_wire_payload(start));
    start.payload[19]=1; CHECK(valid_wire_payload(start)); start.payload[13]=1; CHECK(!valid_wire_payload(start));
    auto cancel=frame(FrameKind::clipboard_control,2,16); cancel.payload[11]=1; CHECK(valid_wire_payload(cancel));
    auto chunk=frame(FrameKind::clipboard_chunk,1,17); chunk.payload[11]=1; CHECK(valid_wire_payload(chunk));
    chunk.payload[12]=1; CHECK(!valid_wire_payload(chunk)); chunk.payload[12]=0;
    chunk.payload.resize(16); CHECK(!valid_wire_payload(chunk));
    chunk.payload.resize(max_clipboard_chunk+1); CHECK(!valid_wire_payload(chunk));
    auto unknown=frame(FrameKind::input,0xff,4); CHECK(!valid_wire_payload(unknown));
}
void shortcut_translator_tests() {
    using namespace hop;
    ShortcutTranslator mac(Platform::macos,Platform::harmonyos,KeyMode::common_shortcuts);
    auto a=mac.push({0xe3,left_meta,true,false}); CHECK(a && a->empty());
    a=mac.push({0x06,left_meta,true,false});
    CHECK(a && a->size()==2 && (*a)[0].usage==0xe0 && (*a)[0].down && (*a)[0].modifiers==left_control && (*a)[1].usage==0x06 && (*a)[1].modifiers==left_control);
    a=mac.push({0x06,left_meta,true,true}); CHECK(a && a->size()==1 && (*a)[0].repeat && (*a)[0].modifiers==left_control);
    a=mac.push({0x06,left_meta,false,false}); CHECK(a && a->size()==1 && !(*a)[0].down);
    a=mac.push({0xe3,0,false,false}); CHECK(a && a->size()==1 && (*a)[0].usage==0xe0 && !(*a)[0].down && (*a)[0].modifiers==0);
    ShortcutTranslator alone(Platform::macos,Platform::harmonyos,KeyMode::common_shortcuts);
    CHECK(alone.push({0xe3,left_meta,true,false})->empty());
    a=alone.push({0xe3,0,false,false}); CHECK(a && a->size()==2 && (*a)[0].usage==0xe3 && (*a)[0].down && !(*a)[1].down);
    ShortcutTranslator win(Platform::windows,Platform::macos,KeyMode::common_shortcuts);
    CHECK(win.push({0xe4,right_control|caps_lock,true,false})->empty());
    a=win.push({0x19,right_control|caps_lock,true,false});
    CHECK(a && a->size()==2 && (*a)[0].usage==0xe7 && (*a)[0].modifiers==(right_meta|caps_lock) && (*a)[1].usage==0x19);
    auto released=win.release_all(); CHECK(released.size()==2 && released[0].usage==0x19 && !released[0].down && released[1].usage==0xe7 && !released[1].down);
    ShortcutTranslator pointer(Platform::macos,Platform::windows,KeyMode::common_shortcuts);
    CHECK(pointer.push({0xe3,left_meta,true,false})->empty());
    auto pointerEvents=pointer.flush_for_pointer(); CHECK(pointerEvents.size()==1 && pointerEvents[0].usage==0xe3);
    a=pointer.push({0xe3,0,false,false}); CHECK(a && a->size()==1 && (*a)[0].usage==0xe3 && !(*a)[0].down);
    ShortcutTranslator cancel(Platform::macos,Platform::windows,KeyMode::common_shortcuts);
    CHECK(cancel.push({0xe3,left_meta,true,false})->empty() && cancel.release_all().empty());
    ShortcutTranslator shifted(Platform::macos,Platform::windows,KeyMode::common_shortcuts);
    CHECK(shifted.push({0xe3,left_meta,true,false})->empty());
    a=shifted.push({0xe1,left_meta|left_shift,true,false}); CHECK(a && a->size()==1 && (*a)[0].usage==0xe1);
    a=shifted.push({0x1d,left_meta|left_shift,true,false}); CHECK(a && a->size()==2 && (*a)[0].usage==0xe0 && (*a)[1].modifiers==(left_control|left_shift));
    released=shifted.release_all(); CHECK(released.size()==3 && released[0].usage==0x1d);
    ShortcutTranslator fallback(Platform::macos,Platform::windows,KeyMode::common_shortcuts);
    CHECK(fallback.push({0xe3,left_meta,true,false})->empty());
    a=fallback.push({0xe2,left_meta|left_alt,true,false}); CHECK(a && a->size()==2 && (*a)[0].usage==0xe3 && (*a)[1].usage==0xe2);
    a=fallback.push({0x06,left_meta|left_alt,true,false}); CHECK(a && a->size()==1 && (*a)[0].modifiers==(left_meta|left_alt));
    ShortcutTranslator prior(Platform::macos,Platform::windows,KeyMode::common_shortcuts);
    CHECK(prior.push({0x06,0,true,false})->size()==1);
    CHECK(prior.push({0xe3,left_meta,true,false})->empty());
    a=prior.push({0x06,left_meta,false,false}); CHECK(a && a->size()==2 && (*a)[0].usage==0xe3 && !(*a)[1].down);
    CHECK(!hop_shortcut_create(0,1,1) && !hop_shortcut_create(1,4,2));
    auto* c = hop_shortcut_create(1,4,1); CHECK(c);
    hop_key_event output[HOP_SHORTCUT_MAX_EVENTS]{}; std::size_t count=99;
    hop_key_event meta{0xe3,left_meta,1,0}, copy{0x06,left_meta,1,0};
    CHECK(hop_shortcut_push(c,&meta,output,HOP_SHORTCUT_MAX_EVENTS-1,&count)==-1 && count==0);
    CHECK(hop_shortcut_push(c,&meta,output,HOP_SHORTCUT_MAX_EVENTS,&count)==0 && count==0);
    CHECK(hop_shortcut_push(c,&copy,output,HOP_SHORTCUT_MAX_EVENTS,&count)==0 && count==2 && output[0].usage==0xe0 && output[1].usage==0x06);
    CHECK(hop_shortcut_release(c,output,HOP_SHORTCUT_MAX_EVENTS,&count)==0 && count==2 && !output[0].down && !output[1].down);
    hop_shortcut_free(c);
    ShortcutTranslator both(Platform::macos,Platform::windows,KeyMode::common_shortcuts);
    CHECK(both.push({0xe3,left_meta,true,false})->empty());
    CHECK(both.push({0xe7,left_meta|right_meta,true,false})->empty());
    a=both.push({0x06,left_meta|right_meta,true,false});
    CHECK(a && a->size()==3 && (*a)[0].usage==0xe0 && (*a)[1].usage==0xe4 && (*a)[2].modifiers==(left_control|right_control));
    a=both.push({0xe3,right_meta,false,false}); CHECK(a && a->size()==1 && (*a)[0].usage==0xe0 && (*a)[0].modifiers==right_control);
    a=both.push({0x06,right_meta,false,false}); CHECK(a && a->size()==1 && (*a)[0].modifiers==right_control);
    a=both.push({0xe7,0,false,false}); CHECK(a && a->size()==1 && (*a)[0].usage==0xe4 && (*a)[0].modifiers==0);
    ShortcutTranslator physical(Platform::macos,Platform::windows,KeyMode::physical);
    a=physical.push({0xe3,left_meta,true,false}); CHECK(a && a->size()==1 && (*a)[0].usage==0xe3);
    a=physical.push({0x06,left_meta,true,false}); CHECK(a && a->size()==1 && (*a)[0].modifiers==left_meta);
    ShortcutTranslator invalid(Platform::windows,Platform::macos,KeyMode::common_shortcuts);
    CHECK(!invalid.push({0xe0,0,true,false}) && invalid.failed());
    CHECK(!invalid.push({0x06,0,true,false}));
}
void input_policy_tests() {
    using namespace hop;
    CHECK(left_shift == 1 && right_shift == 2 && left_control == 4 && right_control == 8 && left_alt == 16 && right_alt == 32 && left_meta == 64 && right_meta == 128);
    auto c = map_chord(Platform::macos, Platform::harmonyos, KeyMode::common_shortcuts, {0x06, left_meta});
    CHECK(c.usage == 0x06 && c.modifiers == left_control);
    c = map_chord(Platform::macos, Platform::harmonyos, KeyMode::common_shortcuts, {0x06, left_meta | caps_lock});
    CHECK(c.usage == 0x06 && c.modifiers == (left_control | caps_lock));
    c = map_chord(Platform::windows, Platform::macos, KeyMode::common_shortcuts, {0x19, right_control});
    CHECK(c.usage == 0x19 && c.modifiers == right_meta);
    c = map_chord(Platform::linux_desktop, Platform::macos, KeyMode::common_shortcuts, {0x1d, left_control | left_shift});
    CHECK(c.modifiers == (left_meta | left_shift));
    c = map_chord(Platform::windows, Platform::linux_desktop, KeyMode::common_shortcuts, {0x06, left_control});
    CHECK(c.modifiers == left_control);
    c = map_chord(Platform::macos, Platform::windows, KeyMode::physical, {0x06, left_meta});
    CHECK(c.modifiers == left_meta);
    c = map_chord(Platform::macos, Platform::windows, KeyMode::common_shortcuts, {0x06, left_meta | left_alt});
    CHECK(c.modifiers == (left_meta | left_alt));
    c = map_chord(Platform::macos, Platform::windows, KeyMode::common_shortcuts, {0x06, left_meta | left_shift});
    CHECK(c.modifiers == (left_meta | left_shift));
    c = map_chord(Platform::windows, Platform::macos, KeyMode::common_shortcuts, {0x1c, left_control});
    CHECK(c.modifiers == left_control); // Ctrl+Y / Cmd+Y differ; do not guess.
    c = map_chord(Platform::windows, Platform::macos, KeyMode::common_shortcuts, {0x06, left_control | left_meta});
    CHECK(c.modifiers == (left_control | left_meta));
    PointerSettings settings;
    auto movement = map_motion(settings, {2, -3});
    CHECK(movement && movement->x == 2 && movement->y == -3);
    settings.movement_scale = 2;
    settings.horizontal_scroll_scale = .5;
    settings.vertical_scroll_scale = 3;
    settings.reverse_vertical = true;
    movement = map_motion(settings, {2, -3});
    CHECK(movement && movement->x == 4 && movement->y == -6);
    auto wheel = map_scroll(settings, {2, -3});
    CHECK(wheel && wheel->x == 1 && wheel->y == 9);
    CHECK(!map_scroll(settings, {std::numeric_limits<double>::quiet_NaN(), 0}));
    CHECK(!map_motion(settings, {1000001, 0}));
    settings.movement_scale = std::numeric_limits<double>::infinity();
    CHECK(!valid_pointer_settings(settings) && !map_motion(settings, {1, 1}));
    settings.movement_scale = .09;
    CHECK(!valid_pointer_settings(settings));
}
}
int main() {
    layout_tests();
    focus_tests();
    input_policy_tests();
    frame_tests();
    outbound_queue_tests();
    clipboard_transfer_tests();
    wire_validation_tests();
    shortcut_translator_tests();
    std::cout << "core: " << checks << " checks passed\n";
}
