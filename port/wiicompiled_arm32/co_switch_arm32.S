// Cooperative context switch for 32-bit ARM (the 3DS's ARM11 MPCore, ARM mode, AAPCS + VFP hard-float).
// Same contract as WiiCompiled's runtime/src/platform/{macos,android}/co_switch.S, used by HostContext:
//
//   void  mkw_co_switch(void** targetSp, void** sourceSp);
//       Saves the callee-saved state on the current stack, stores that stack pointer in *sourceSp,
//       loads *targetSp and restores the target's state from it. Leaf-only: it never calls C++.
//   void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);
//       Builds a frame on a fresh stack that the first mkw_co_switch "returns" from into a trampoline
//       which calls entry(argument). Returns the saved stack pointer to store for that context.
//
// Frame (104 bytes, 8-byte aligned as AAPCS requires at a call boundary):
//   [sp +  0 .. +63]  d8-d15  (VFP callee-saved)
//   [sp + 64 .. +99]  r4-r12  (r12 is scratch; it is stored only to keep the frame 8-byte aligned)
//   [sp +100]         lr      (popped into pc)
// FPSCR is not switched, matching the AArch64 backends (the runtime sets the guest FP mode itself).

#if defined(__3DS__) || (defined(__arm__) && !defined(__aarch64__))
    .syntax unified
    .arm
    .fpu vfp
    .text

    .p2align 2
    .globl mkw_co_switch
    .type mkw_co_switch, %function
mkw_co_switch:
    push    {r4-r12, lr}
    vpush   {d8-d15}
    mov     r2, sp
    str     r2, [r1]
    ldr     r2, [r0]
    mov     sp, r2
    vpop    {d8-d15}
    pop     {r4-r12, pc}
    .size mkw_co_switch, . - mkw_co_switch

    .p2align 2
    .globl mkw_co_init
    .type mkw_co_init, %function
mkw_co_init:
    bic     r0, r0, #7
    sub     r0, r0, #104
    mov     r3, #0
    str     r1, [r0, #64]       // r4: entry
    str     r2, [r0, #68]       // r5: argument
    str     r3, [r0, #92]       // r11 (frame pointer): end of the frame chain
    adr     r3, mkw_co_entry_trampoline
    str     r3, [r0, #100]      // lr: the first switch "returns" into the trampoline
    bx      lr
    .size mkw_co_init, . - mkw_co_init

    .p2align 2
    .type mkw_co_entry_trampoline, %function
mkw_co_entry_trampoline:
    mov     r0, r5
    blx     r4
    // A guest fiber must leave through the scheduler's handoff, never by returning here.
    bkpt    #0
    .size mkw_co_entry_trampoline, . - mkw_co_entry_trampoline

    .section .note.GNU-stack, "", %progbits
#endif
