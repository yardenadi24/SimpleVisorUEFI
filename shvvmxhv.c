/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvvmxhv.c

Abstract:

    This module implements the Simple Hyper Visor itself.

Author:

    Alex Ionescu (@aionescu) 16-Mar-2016 - Initial version

Environment:

    Hypervisor mode only, IRQL MAX_IRQL

--*/

#include "shv.h"

//
// Minimal serial output for VM-Exit handler debugging (COM2 = 0x2F8).
// These are inline to avoid any stack/calling-convention issues in VMX root.
//
#define SHV_SERIAL_PORT 0x3F8

static void __forceinline
HvSerialInit (
    void
    )
{
    __outbyte(SHV_SERIAL_PORT + 1, 0x00);
    __outbyte(SHV_SERIAL_PORT + 3, 0x80);
    __outbyte(SHV_SERIAL_PORT + 0, 0x01);
    __outbyte(SHV_SERIAL_PORT + 1, 0x00);
    __outbyte(SHV_SERIAL_PORT + 3, 0x03);
    __outbyte(SHV_SERIAL_PORT + 2, 0xC7);
    __outbyte(SHV_SERIAL_PORT + 4, 0x0B);
}

static volatile long hvSerialInitDone = 0;

static void __forceinline
HvSerialPutChar (
    _In_ char c
    )
{
    if (_InterlockedCompareExchange(&hvSerialInitDone, 1, 0) == 0)
    {
        HvSerialInit();
    }
    while ((__inbyte(SHV_SERIAL_PORT + 5) & 0x20) == 0);
    __outbyte(SHV_SERIAL_PORT, (unsigned char)c);
}

static void __forceinline
HvSerialPrint (
    _In_ const char* s
    )
{
    while (*s)
    {
        if (*s == '\n') HvSerialPutChar('\r');
        HvSerialPutChar(*s++);
    }
}

static void __forceinline
HvSerialPrintHex (
    _In_ UINT64 v
    )
{
    const char hex[] = "0123456789ABCDEF";
    char buf[19];
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
    HvSerialPrint(buf);
}

DECLSPEC_NORETURN
VOID
ShvVmxResume (
    VOID
    )
{
    //
    // Issue a VMXRESUME. The reason that we've defined an entire function for
    // this sole instruction is both so that we can use it as the target of the
    // VMCS when re-entering the VM After a VM-Exit, as well as so that we can
    // decorate it with the DECLSPEC_NORETURN marker, which is not set on the
    // intrinsic (as it can fail in case of an error).
    //
    __vmx_vmresume();
}

uintptr_t
FORCEINLINE
ShvVmxRead (
    _In_ UINT32 VmcsFieldId
    )
{
    size_t FieldData;

    //
    // Because VMXREAD returns an error code, and not the data, it is painful
    // to use in most circumstances. This simple function simplifies it use.
    //
    __vmx_vmread(VmcsFieldId, &FieldData);
    return FieldData;
}

INT32
ShvVmxLaunch (
    VOID
    )
{
    INT32 failureCode;

    //
    // Launch the VMCS
    //
    __vmx_vmlaunch();

    //
    // If we got here, either VMCS setup failed in some way, or the launch
    // did not proceed as planned.
    //
    failureCode = (INT32)ShvVmxRead(VM_INSTRUCTION_ERROR);
    __vmx_off();

    //
    // Return the error back to the caller
    //
    return failureCode;
}

VOID
ShvVmxHandleInvd (
    VOID
    )
{
    //
    // This is the handler for the INVD instruction. Technically it may be more
    // correct to use __invd instead of __wbinvd, but that intrinsic doesn't
    // actually exist. Additionally, the Windows kernel (or HAL) don't contain
    // any example of INVD actually ever being used. Finally, Hyper-V itself
    // handles INVD by issuing WBINVD as well, so we'll just do that here too.
    //
    __wbinvd();
}

VOID
ShvVmxHandleCpuid (
    _In_ PSHV_VP_STATE VpState
    )
{
    INT32 cpu_info[4];

    //
    // Check for the magic CPUID sequence, and check that it is coming from
    // Ring 0. Technically we could also check the RIP and see if this falls
    // in the expected function, but we may want to allow a separate "unload"
    // driver or code at some point.
    //
    if ((VpState->VpRegs->Rax == 0x41414141) &&
        (VpState->VpRegs->Rcx == 0x42424242) &&
        ((ShvVmxRead(GUEST_CS_SELECTOR) & RPL_MASK) == DPL_SYSTEM))
    {
        VpState->ExitVm = TRUE;
        return;
    }

    //
    // Otherwise, issue the CPUID to the logical processor based on the indexes
    // on the VP's GPRs.
    //
    __cpuidex(cpu_info, (INT32)VpState->VpRegs->Rax, (INT32)VpState->VpRegs->Rcx);

    //
    // Check if this was CPUID 1h, which is the features request.
    //
    if (VpState->VpRegs->Rax == 1)
    {
        //
        // CLEAR the Hypervisor Present-bit. In a nested VM (VMware/Hyper-V),
        // the outer hypervisor already sets this bit. If Windows sees it
        // during boot, it loads a Hyper-V aware HAL that expects working
        // synthetic timers, reference TSC, and hypercalls. Since SimpleVisor
        // doesn't implement any of those, the kernel hangs waiting for a
        // timer that never fires.
        //
        // By clearing the bit, Windows boots with the standard hardware HAL
        // and uses native APIC timers, which work correctly.
        //
        // SimpleVisor can still be detected via the magic CPUID sequence
        // (EAX=0x41414141, ECX=0x42424242) used for unloading.
        //
        cpu_info[2] &= ~HYPERV_HYPERVISOR_PRESENT_BIT;
    }
    else if (VpState->VpRegs->Rax == HYPERV_CPUID_INTERFACE)
    {
        //
        // Return our interface identifier. Windows won't query this leaf
        // since the hypervisor present bit is cleared, but our own
        // ShvIsOurHypervisorPresent() uses it to verify we're loaded.
        //
        cpu_info[0] = ' vhS';
        cpu_info[1] = 0;
        cpu_info[2] = 0;
        cpu_info[3] = 0;
    }
    else if ((VpState->VpRegs->Rax >= HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS) &&
             (VpState->VpRegs->Rax <= HYPERV_CPUID_MAX))
    {
        //
        // Zero out all other hypervisor CPUID leaves (0x40000000,
        // 0x40000002+) to hide the outer hypervisor's enlightenment data.
        //
        cpu_info[0] = 0;
        cpu_info[1] = 0;
        cpu_info[2] = 0;
        cpu_info[3] = 0;
    }

    //
    // Copy the values from the logical processor registers into the VP GPRs.
    //
    VpState->VpRegs->Rax = cpu_info[0];
    VpState->VpRegs->Rbx = cpu_info[1];
    VpState->VpRegs->Rcx = cpu_info[2];
    VpState->VpRegs->Rdx = cpu_info[3];
}

VOID
ShvVmxHandleXsetbv (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // XSETBV requires CR4.OSXSAVE to be set. On VM-Exit, HOST_CR4 is loaded,
    // which may not have OSXSAVE if it wasn't set when the hypervisor captured
    // the initial state (e.g., UEFI didn't enable OSXSAVE, but Windows did
    // later without causing a VM-Exit). Ensure OSXSAVE is set before issuing
    // XSETBV, otherwise we get a #GP in VMX root mode.
    //
    __writecr4(__readcr4() | 0x40000);

    _xsetbv((UINT32)VpState->VpRegs->Rcx,
            VpState->VpRegs->Rdx << 32 |
            VpState->VpRegs->Rax);
}

VOID
ShvVmxHandleVmx (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Set the CF flag, which is how VMX instructions indicate failure
    //
    VpState->GuestEFlags |= 0x1; // VM_FAIL_INVALID

    //
    // RFLAGs is actually restored from the VMCS, so update it here
    //
    __vmx_vmwrite(GUEST_RFLAGS, VpState->GuestEFlags);
}

static const char*
HvExitReasonToString (
    _In_ UINT16 reason
    )
{
    switch (reason)
    {
    case 0:  return "EXCEPTION_NMI";
    case 1:  return "EXT_INTERRUPT";
    case 2:  return "TRIPLE_FAULT";
    case 3:  return "INIT";
    case 4:  return "SIPI";
    case 7:  return "VIRT_INTR_PENDING";
    case 8:  return "VIRT_NMI_PENDING";
    case 9:  return "TASK_SWITCH";
    case 10: return "CPUID";
    case 12: return "HLT";
    case 13: return "INVD";
    case 14: return "INVLPG";
    case 15: return "RDPMC";
    case 16: return "RDTSC";
    case 18: return "VMCALL";
    case 19: return "VMCLEAR";
    case 20: return "VMLAUNCH";
    case 21: return "VMPTRLD";
    case 22: return "VMPTRST";
    case 23: return "VMREAD";
    case 24: return "VMRESUME";
    case 25: return "VMWRITE";
    case 26: return "VMXOFF";
    case 27: return "VMXON";
    case 28: return "CR_ACCESS";
    case 29: return "DR_ACCESS";
    case 30: return "IO_INSTR";
    case 31: return "MSR_READ";
    case 32: return "MSR_WRITE";
    case 33: return "INVALID_GUEST";
    case 34: return "MSR_LOADING";
    case 36: return "MWAIT";
    case 37: return "MTF";
    case 39: return "MONITOR";
    case 40: return "PAUSE";
    case 41: return "MCE_VMENTRY";
    case 43: return "TPR_BELOW";
    case 44: return "APIC_ACCESS";
    case 46: return "GDTR_IDTR";
    case 47: return "LDTR_TR";
    case 48: return "EPT_VIOLATION";
    case 49: return "EPT_MISCONFIG";
    case 50: return "INVEPT";
    case 51: return "RDTSCP";
    case 52: return "PREEMPT_TIMER";
    case 53: return "INVVPID";
    case 54: return "WBINVD";
    case 55: return "XSETBV";
    case 58: return "INVPCID";
    case 63: return "XSAVES";
    case 64: return "XRSTORS";
    default: return "UNKNOWN";
    }
}

VOID
ShvVmxHandleExit (
    _In_ PSHV_VP_STATE VpState
    )
{
    //
    // Log exit reasons to serial for debugging. Use a static counter to
    // only log the first handful of each type to avoid flooding serial.
    //
    static volatile long exitCount = 0;
    long count = _InterlockedIncrement(&exitCount);
    if (count <= 200 || (count % 1000) == 0)
    {
        HvSerialPrint("[HV] EXIT ");
        HvSerialPrint(HvExitReasonToString(VpState->ExitReason));
        HvSerialPrint(" rip=");
        HvSerialPrintHex(VpState->GuestRip);
        if (VpState->ExitReason <= 1)
        {
            HvSerialPrint(" intrInfo=");
            HvSerialPrintHex(ShvVmxRead(VM_EXIT_INTR_INFO));
        }
        HvSerialPrint(" #");
        HvSerialPrintHex(count);
        HvSerialPrint("\n");
    }

    //
    // This is the generic VM-Exit handler. Decode the reason for the exit and
    // call the appropriate handler. Instruction-based exits (CPUID, INVD,
    // XSETBV, VMX) advance RIP past the instruction. Event-based exits
    // (external interrupts, NMIs, exceptions) must NOT advance RIP -- they
    // are re-injected into the guest instead.
    //
    switch (VpState->ExitReason)
    {
    case EXIT_REASON_CPUID:
        ShvVmxHandleCpuid(VpState);
        break;
    case EXIT_REASON_INVD:
        ShvVmxHandleInvd();
        break;
    case EXIT_REASON_XSETBV:
        ShvVmxHandleXsetbv(VpState);
        break;
    case EXIT_REASON_VMCALL:
    case EXIT_REASON_VMCLEAR:
    case EXIT_REASON_VMLAUNCH:
    case EXIT_REASON_VMPTRLD:
    case EXIT_REASON_VMPTRST:
    case EXIT_REASON_VMREAD:
    case EXIT_REASON_VMRESUME:
    case EXIT_REASON_VMWRITE:
    case EXIT_REASON_VMXOFF:
    case EXIT_REASON_VMXON:
        ShvVmxHandleVmx(VpState);
        break;

    case EXIT_REASON_EXTERNAL_INTERRUPT:
    {
        //
        // External interrupt. With ACK_INTR_ON_EXIT enabled, the vector
        // is in VM_EXIT_INTR_INFO. Re-inject into the guest.
        // Mask to bits 0-11 and bit 31 (valid) to avoid reserved bit issues.
        //
        uintptr_t intrInfo = ShvVmxRead(VM_EXIT_INTR_INFO);
        __vmx_vmwrite(VM_ENTRY_INTR_INFO, intrInfo & 0x80000FFF);
        return;
    }

    case EXIT_REASON_EXCEPTION_NMI:
    {
        //
        // NMI or hardware exception. Re-inject into the guest.
        //
        uintptr_t intrInfo = ShvVmxRead(VM_EXIT_INTR_INFO);
        __vmx_vmwrite(VM_ENTRY_INTR_INFO, intrInfo & 0x80000FFF);
        if (intrInfo & (1ULL << 11))
        {
            __vmx_vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE,
                          ShvVmxRead(VM_EXIT_INTR_ERROR_CODE));
        }
        return;
    }

    default:
        //
        // Unknown/unexpected exit reason. Do not advance RIP as the exit
        // may not be instruction-based. Just resume the guest.
        //
        HvSerialPrint("[HV] UNHANDLED EXIT ");
        HvSerialPrint(HvExitReasonToString(VpState->ExitReason));
        HvSerialPrint(" rip=");
        HvSerialPrintHex(VpState->GuestRip);
        HvSerialPrint("\n");
        return;
    }

    //
    // Move the instruction pointer to the next instruction after the one that
    // caused the exit. This is ONLY done for instruction-based exits above
    // (CPUID, INVD, XSETBV, VMX instructions).
    //
    VpState->GuestRip += ShvVmxRead(VM_EXIT_INSTRUCTION_LEN);
    __vmx_vmwrite(GUEST_RIP, VpState->GuestRip);
}

DECLSPEC_NORETURN
VOID
ShvVmxEntryHandler (
    _In_ PCONTEXT Context
    )
{
    SHV_VP_STATE guestContext;
    PSHV_VP_DATA vpData;

    //
    // Because we had to use RCX when calling ShvOsCaptureContext, its value
    // was actually pushed on the stack right before the call. Go dig into the
    // stack to find it, and overwrite the bogus value that's there now.
    //
    Context->Rcx = *(UINT64*)((uintptr_t)Context - sizeof(Context->Rcx));

    //
    // Get the per-VP data for this processor.
    //
    vpData = (VOID*)((uintptr_t)(Context + 1) - KERNEL_STACK_SIZE);

    //
    // Build a little stack context to make it easier to keep track of certain
    // guest state, such as the RIP/RSP/RFLAGS, and the exit reason. The rest
    // of the general purpose registers come from the context structure that we
    // captured on our own with RtlCaptureContext in the assembly entrypoint.
    //
    guestContext.GuestEFlags = ShvVmxRead(GUEST_RFLAGS);
    guestContext.GuestRip = ShvVmxRead(GUEST_RIP);
    guestContext.GuestRsp = ShvVmxRead(GUEST_RSP);
    guestContext.ExitReason = ShvVmxRead(VM_EXIT_REASON) & 0xFFFF;
    guestContext.VpRegs = Context;
    guestContext.ExitVm = FALSE;

    //
    // Call the generic handler
    //
    ShvVmxHandleExit(&guestContext);

    //
    // Did we hit the magic exit sequence, or should we resume back to the VM
    // context?
    //
    if (guestContext.ExitVm != FALSE)
    {
        //
        // Return the VP Data structure in RAX:RBX which is going to be part of
        // the CPUID response that the caller (ShvVpUninitialize) expects back.
        // Return confirmation in RCX that we are loaded
        //
        Context->Rax = (uintptr_t)vpData >> 32;
        Context->Rbx = (uintptr_t)vpData & 0xFFFFFFFF;
        Context->Rcx = 0x43434343;

        //
        // Perform any OS-specific CPU uninitialization work
        //
        ShvOsUnprepareProcessor(vpData);

        //
        // Our callback routine may have interrupted an arbitrary user process,
        // and therefore not a thread running with a systemwide page directory.
        // Therefore if we return back to the original caller after turning off
        // VMX, it will keep our current "host" CR3 value which we set on entry
        // to the PML4 of the SYSTEM process. We want to return back with the
        // correct value of the "guest" CR3, so that the currently executing
        // process continues to run with its expected address space mappings.
        //
        __writecr3(ShvVmxRead(GUEST_CR3));

        //
        // Finally, restore the stack, instruction pointer and EFLAGS to the
        // original values present when the instruction causing our VM-Exit
        // execute (such as ShvVpUninitialize). This will effectively act as
        // a longjmp back to that location.
        //
        Context->Rsp = guestContext.GuestRsp;
        Context->Rip = (UINT64)guestContext.GuestRip;
        Context->EFlags = (UINT32)guestContext.GuestEFlags;

        //
        // Turn off VMX root mode on this logical processor. We're done here.
        //
        __vmx_off();
    }
    else
    {
        //
        // Because we won't be returning back into assembly code, nothing will
        // ever know about the "pop rcx" that must technically be done (or more
        // accurately "add rsp, 4" as rcx will already be correct thanks to the
        // fixup earlier. In order to keep the stack sane, do that adjustment
        // here.
        //
        Context->Rsp += sizeof(Context->Rcx);

        //
        // Return into a VMXRESUME intrinsic, which we broke out as its own
        // function, in order to allow this to work. No assembly code will be
        // needed as RtlRestoreContext will fix all the GPRs, and what we just
        // did to RSP will take care of the rest.
        //
        Context->Rip = (UINT64)ShvVmxResume;
    }

    //
    // Restore the context to either ShvVmxResume, in which case the CPU's VMX
    // facility will do the "true" return back to the VM (but without restoring
    // GPRs, which is why we must do it here), or to the original guest's RIP,
    // which we use in case an exit was requested. In this case VMX must now be
    // off, and this will look like a longjmp to the original stack and RIP.
    //
    ShvOsRestoreContext(Context);
}

