// HostContext backend for the Nintendo 3DS, included by runtime/src/host_context.cpp inside
// namespace HostContext (tools/patch_wiicompiled_arm32.py adds the include).
//
// Mirrors the AArch64 backend: each guest fiber gets its own heap-allocated stack, and
// mkw_co_switch (co_switch_arm32.S) swaps stack pointers. The 3DS has no mprotect, so there is no
// guard page: stack sizes come from the guest scheduler, which sizes them generously.

namespace {
struct Context {
    void* savedStackPointer = nullptr;
    void* stack = nullptr;
    std::size_t stackBytes = 0;
};

// Guest scheduling runs on one host thread (the game's), as on the other backends.
Context* g_current = nullptr;

// Fiber stacks are reused instead of freed. A guest thread's 256 KiB stack is one contiguous block, and late in a
// run the fragmented regular heap may have none: the title screen's movie reader thread then never ran and the game
// waited for it forever (console, 2026-11-24: "Failed to create host context", stall at the opening movie). Stacks
// of exited threads wait here for the next thread, and a few are set aside after the first frame
// (Brawl3dsReserveFiberStacks), while the heap still has room. Only the game thread creates and destroys fibers.
constexpr std::size_t kPooledStackBytes = 256 * 1024;
constexpr int kPoolSlots = 12;
void* g_stackPool[kPoolSlots];
int g_stackPoolCount = 0;

void* AllocateStack(std::size_t bytes)
{
    if (bytes == kPooledStackBytes && g_stackPoolCount > 0) return g_stackPool[--g_stackPoolCount];
    return std::aligned_alloc(16, bytes);
}

void ReleaseStack(void* stack, std::size_t bytes)
{
    if (!stack) return;
    if (bytes == kPooledStackBytes && g_stackPoolCount < kPoolSlots) {
        g_stackPool[g_stackPoolCount++] = stack;
        return;
    }
    std::free(stack);
}
} // namespace

// main_3ds.cpp, after the first frame: set `count` stacks aside for guest threads made later.
extern "C" int Brawl3dsReserveFiberStacks(int count)
{
    int reserved = 0;
    while (reserved < count && g_stackPoolCount < kPoolSlots) {
        void* stack = std::aligned_alloc(16, kPooledStackBytes);
        if (!stack) break;
        g_stackPool[g_stackPoolCount++] = stack;
        ++reserved;
    }
    return reserved;
}

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    g_current = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    context->stackBytes = (stackSize + 15) & ~static_cast<std::size_t>(15);
    context->stack = AllocateStack(context->stackBytes);
    if (!context->stack) {
        delete context;
        return nullptr;
    }
    auto* stackTop = static_cast<char*>(context->stack) + stackSize;
    context->savedStackPointer = mkw_co_init(stackTop, entry, argument);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }
    ReleaseStack(nativeContext->stack, nativeContext->stackBytes);
    delete nativeContext;
}

Handle Current()
{
    return g_current;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    mkw_co_switch(&destination->savedStackPointer, &source->savedStackPointer);
    g_current = source;
}
