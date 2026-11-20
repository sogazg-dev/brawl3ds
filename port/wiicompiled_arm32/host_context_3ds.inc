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
};

// Guest scheduling runs on one host thread (the game's), as on the other backends.
Context* g_current = nullptr;
} // namespace

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
    context->stack = std::aligned_alloc(16, (stackSize + 15) & ~static_cast<std::size_t>(15));
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
    std::free(nativeContext->stack);
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
