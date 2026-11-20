// Headless Aurora for the 3DS timing build: the few Aurora entry points whose results the runtime depends on.
// Every other Aurora/GX/VI/PAD function the runtime calls is generated as a typed no-op (tools/gen_link_stubs.py).
//
// The runtime presents once per game frame through aurora_begin_frame()/aurora_end_frame() (hle/vi.cpp), which makes
// them the frame boundary the headless build measures. Brawl3dsFrameBegin/End live in main_3ds.cpp.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx/GXEnum.h>

#include <cstdint>

extern "C" void Brawl3dsFrameBegin();
extern "C" void Brawl3dsFrameEnd();
extern "C" bool Brawl3dsExitRequested();

extern "C" AuroraInfo aurora_initialize(int, char*[], const AuroraConfig*)
{
    AuroraInfo info{};
    info.initializationStatus = AURORA_INITIALIZATION_SUCCESS;
    info.windowSize.width = 400;
    info.windowSize.height = 240;
    info.windowSize.fb_width = 400;
    info.windowSize.fb_height = 240;
    info.windowSize.native_fb_width = 400;
    info.windowSize.native_fb_height = 240;
    info.windowSize.scale = 1.0f;
    info.gpuName = "PICA200 (headless timing build)";
    return info;
}

extern "C" void aurora_shutdown() {}

extern "C" const AuroraEvent* aurora_update()
{
    // A list terminated by AURORA_NONE, as Aurora returns it; AURORA_EXIT asks the runtime to shut down cleanly.
    static AuroraEvent events[2]{};
    events[0].type = Brawl3dsExitRequested() ? AURORA_EXIT : AURORA_NONE;
    events[1].type = AURORA_NONE;
    return events;
}

extern "C" bool aurora_begin_frame()
{
    Brawl3dsFrameBegin();
    return true;
}

extern "C" void aurora_end_frame()
{
    Brawl3dsFrameEnd();
}

namespace aurora::gx::fifo {
// Packed raw draws are accepted and dropped: there is no renderer in the timing build.
bool submit_raw_draw(GXPrimitive, GXVtxFmt, const uint8_t*, uint16_t, uint32_t) { return true; }
} // namespace aurora::gx::fifo
