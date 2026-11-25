// Renderer milestone 5, step 2: Aurora's GX command processor on the core-2 worker (docs/RENDERER_3DS.md).
//
// Aurora's GX API encodes everything into a byte FIFO (one per thread on the 3DS, see gx/fifo.hpp) and its command
// processor decodes that into g_gxState, which the PICA back end reads. Here the game thread only produces: the
// FIFO bytes, display lists and the runtime's packed draws are copied into the renderer's command ring and the
// worker decodes them (pica_renderer_3ds.cpp). The few GX functions that write g_gxState directly are either queued
// as a call that runs on the worker in order (brawl3ds_cp_defer) or, when they must return something from it, run
// on the game thread after the worker has caught up (Brawl3dsCpInlineScope).
//
// tools/patch_wiicompiled_arm32.py inserts the calls into Aurora (tag brawl3ds-cp-worker).
#pragma once

#if defined(__3DS__)
#include <cstdint>
#include <new>
#include <type_traits>

extern "C" {
// True on the worker thread (it decodes GX commands; the game thread's FIFO is not its own).
bool brawl3ds_cp_on_worker(void);
// True on the game thread while GX commands go to the worker (not during an inline scope, not with debug flag 64).
bool brawl3ds_cp_deferred(void);
// Queue FIFO bytes / a display list (copied); false: not deferred, the caller decodes them itself.
bool brawl3ds_cp_defer_fifo(const uint8_t* data, uint32_t size);
bool brawl3ds_cp_defer_list(const uint8_t* data, uint32_t size, bool bigEndian);
// Queue one of the runtime's packed draws (vertices copied). The caller flushed its dirty state first.
void brawl3ds_cp_defer_raw_draw(int prim, int fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t bytes);
// A call that runs on the worker: begin returns room for `bytes` of payload (the FIFO written so far is queued
// first), end publishes it. The worker runs fn(payload), then decodes what the call wrote into its own FIFO.
void* brawl3ds_cp_begin_call(void (*fn)(void*), uint32_t bytes);
void brawl3ds_cp_end_call(void* payload);
// Game thread: wait until the worker has decoded everything queued, then decode on this thread until the end.
void brawl3ds_cp_inline_begin(const char* site);  // site: the GX function (headless.log)
void brawl3ds_cp_inline_end(void);
// GXDispList.cpp's display-list replay, for the worker.
void brawl3ds_cp_process_display_list(const uint8_t* data, uint32_t size, bool bigEndian);
}

// Queues `f` (a lambda capturing plain values) to run on the worker. Only when brawl3ds_cp_deferred() is true.
template <class F>
inline void brawl3ds_cp_defer_now(const F& f)
{
    static_assert(std::is_trivially_copyable_v<F>, "a deferred GX call may only capture plain values");
    static_assert(alignof(F) <= 8, "the ring aligns payloads to 8 bytes");
    void* payload = brawl3ds_cp_begin_call([](void* p) { (*static_cast<F*>(p))(); }, sizeof(F));
    new (payload) F(f);
    brawl3ds_cp_end_call(payload);
}

// On the game thread with deferral on: run PRODUCER (effects on the game thread's own GX SDK state) here, queue the
// call (the function itself, run again on the worker, where it goes the normal way) and return from the function.
#define BRAWL3DS_CP_DEFER_CALL(PRODUCER, ...)                                                                          \
    do {                                                                                                               \
        if (brawl3ds_cp_deferred()) {                                                                                  \
            PRODUCER;                                                                                                  \
            brawl3ds_cp_defer_now([=] { __VA_ARGS__; });                                                               \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

// Statements that only the game thread may run (the GX SDK state, __gx), skipped when the call runs on the worker.
#define BRAWL3DS_CP_PRODUCER_ONLY(...)                                                                                 \
    do {                                                                                                               \
        if (!brawl3ds_cp_on_worker()) {                                                                                \
            __VA_ARGS__;                                                                                               \
        }                                                                                                              \
    } while (0)

// For functions that read g_gxState back: the worker catches up first, then this thread decodes until the scope
// ends.
struct Brawl3dsCpInlineScope {
    explicit Brawl3dsCpInlineScope(bool active = true, const char* site = nullptr)
        : m_active(active && brawl3ds_cp_deferred())
    {
        if (m_active) brawl3ds_cp_inline_begin(site);
    }
    ~Brawl3dsCpInlineScope()
    {
        if (m_active) brawl3ds_cp_inline_end();
    }
    Brawl3dsCpInlineScope(const Brawl3dsCpInlineScope&) = delete;
    Brawl3dsCpInlineScope& operator=(const Brawl3dsCpInlineScope&) = delete;

private:
    bool m_active;
};
#endif
