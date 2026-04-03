/*++

Copyright (c) Alex Ionescu.  All rights reserved.

Module Name:

    shvos.c

Abstract:

    This module implements the OS-facing UEFI stubs for SimpleVisor.

Author:

    Alex Ionescu (@aionescu) 29-Aug-2016 - Initial version

Environment:

    Kernel mode only.

--*/

//
// Basic UEFI Libraries
//
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>

//
// Boot and Runtime Services
//
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

//
// Multi-Processor Service
//
#include <Pi/PiDxeCis.h>
#include <Protocol/MpService.h>

//
// Device Path and Loaded Image protocols for chain-loading
//
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>

//
// Variable Arguments (CRT)
//
#include <varargs.h>

//
// External SimpleVisor Headers
//
#include "..\ntint.h"
#include "..\shv_x.h"

//
// We run on any UEFI Specification
//
extern CONST UINT32 _gUefiDriverRevision = 0;

//
// We support unload
//
const UINT8 _gDriverUnloadImageCount = 1;

//
// Our name
//
CHAR8 *gEfiCallerBaseName = "SimpleVisor";

//
// PI Multi Processor Services Protocol
//
EFI_MP_SERVICES_PROTOCOL* _gPiMpService;

//
// Serial port I/O for debug output (COM1 = 0x3F8)
//
#define SHV_SERIAL_PORT   0x3F8

VOID
SerialPortInitialize (
    VOID
    )
{
    //
    // Standard 16550 UART initialization: 115200 baud, 8N1
    //
    __outbyte(SHV_SERIAL_PORT + 1, 0x00);   // Disable all interrupts
    __outbyte(SHV_SERIAL_PORT + 3, 0x80);   // Enable DLAB (set baud rate divisor)
    __outbyte(SHV_SERIAL_PORT + 0, 0x01);   // Divisor low byte: 115200 baud
    __outbyte(SHV_SERIAL_PORT + 1, 0x00);   // Divisor high byte
    __outbyte(SHV_SERIAL_PORT + 3, 0x03);   // 8 bits, no parity, 1 stop bit
    __outbyte(SHV_SERIAL_PORT + 2, 0xC7);   // Enable FIFO, clear, 14-byte threshold
    __outbyte(SHV_SERIAL_PORT + 4, 0x0B);   // IRQs enabled, RTS/DSR set
}

VOID
SerialPutChar (
    _In_ CHAR8 c
    )
{
    //
    // Wait for transmit holding register to be empty
    //
    while ((__inbyte(SHV_SERIAL_PORT + 5) & 0x20) == 0);
    __outbyte(SHV_SERIAL_PORT, (UINT8)c);
}

VOID
SerialPrint (
    _In_ CONST CHAR8* String
    )
{
    while (*String != '\0')
    {
        if (*String == '\n')
        {
            SerialPutChar('\r');
        }
        SerialPutChar(*String);
        String++;
    }
}

VOID
SerialPrintHex64 (
    _In_ UINT64 Value
    )
{
    CHAR8 hex[] = "0123456789ABCDEF";
    CHAR8 buffer[19];
    INT32 i;

    buffer[0] = '0';
    buffer[1] = 'x';
    for (i = 0; i < 16; i++)
    {
        buffer[2 + i] = hex[(Value >> (60 - i * 4)) & 0xF];
    }
    buffer[18] = '\0';
    SerialPrint(buffer);
}

//
// TSS Segment we will use
//
#define KGDT64_SYS_TSS          0x60

EFI_STATUS
__forceinline
ShvOsErrorToError (
    INT32 Error
    )
{
    //
    // Convert the possible SimpleVisor errors into NT Hyper-V Errors
    //
    if (Error == SHV_STATUS_NOT_AVAILABLE)
    {
        return EFI_NOT_AVAILABLE_YET;
    }
    if (Error == SHV_STATUS_NO_RESOURCES)
    {
        return EFI_OUT_OF_RESOURCES;
    }
    if (Error == SHV_STATUS_NOT_PRESENT)
    {
        return EFI_NOT_FOUND;
    }
    if (Error == SHV_STATUS_SUCCESS)
    {
        return EFI_SUCCESS;
    }

    //
    // Unknown/unexpected error
    //
    return EFI_LOAD_ERROR;
}

VOID
_str (
    _In_ UINT16* Tr
    )
{
    //
    // Use the UEFI framework function
    //
    *Tr = AsmReadTr();
}

VOID
_sldt (
    _In_ UINT16* Ldtr
    )
{
    //
    // Use the UEFI framework function
    //
    *Ldtr = AsmReadLdtr();
}

VOID
__lgdt (
    _In_ IA32_DESCRIPTOR* Gdtr
    )
{
    //
    // Use the UEFI framework function
    //
    AsmWriteGdtr(Gdtr);
}

VOID
ShvOsUnprepareProcessor (
    _In_ PSHV_VP_DATA VpData
    )
{
    UNREFERENCED_PARAMETER(VpData);

    //
    // Nothing to do
    //
}

INT32
ShvOsBuildHostPageTables (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT64 *Pml4, *Pdpt, *Pd;
    UINT32 i, j;

    SerialPrint("[SHV] Building host page tables (identity-mapped, 2MB pages, 512GB)\n");

    //
    // Allocate PML4 (1 page)
    //
    Pml4 = AllocateAlignedRuntimePages(1, EFI_PAGE_SIZE);
    if (Pml4 == NULL)
    {
        SerialPrint("[SHV] ERROR: Failed to allocate PML4\n");
        return SHV_STATUS_NO_RESOURCES;
    }
    ZeroMem(Pml4, EFI_PAGE_SIZE);
    SerialPrint("[SHV]   PML4 at "); SerialPrintHex64((UINT64)Pml4); SerialPrint("\n");

    //
    // Allocate PDPT (1 page, 512 entries covering 512GB)
    //
    Pdpt = AllocateAlignedRuntimePages(1, EFI_PAGE_SIZE);
    if (Pdpt == NULL)
    {
        SerialPrint("[SHV] ERROR: Failed to allocate PDPT\n");
        FreeAlignedPages(Pml4, 1);
        return SHV_STATUS_NO_RESOURCES;
    }
    ZeroMem(Pdpt, EFI_PAGE_SIZE);
    SerialPrint("[SHV]   PDPT at "); SerialPrintHex64((UINT64)Pdpt); SerialPrint("\n");

    //
    // Allocate PD pages (512 pages, each with 512 entries of 2MB pages)
    //
    Pd = AllocateAlignedRuntimePages(PDPTE_ENTRY_COUNT, EFI_PAGE_SIZE);
    if (Pd == NULL)
    {
        SerialPrint("[SHV] ERROR: Failed to allocate PD pages\n");
        FreeAlignedPages(Pml4, 1);
        FreeAlignedPages(Pdpt, 1);
        return SHV_STATUS_NO_RESOURCES;
    }
    ZeroMem(Pd, PDPTE_ENTRY_COUNT * EFI_PAGE_SIZE);
    SerialPrint("[SHV]   PD pages at "); SerialPrintHex64((UINT64)Pd); SerialPrint("\n");

    //
    // PML4[0] -> PDPT (Present | Read/Write)
    //
    Pml4[0] = (UINT64)Pdpt | 0x23;

    //
    // Fill PDPT entries, each pointing to a PD page
    //
    for (i = 0; i < PDPTE_ENTRY_COUNT; i++)
    {
        Pdpt[i] = ((UINT64)Pd + (i * EFI_PAGE_SIZE)) | 0x23;
    }

    //
    // Fill PD entries with 2MB identity-mapped large pages
    // Each entry: Present | Read/Write | Page Size (2MB)
    //
    for (i = 0; i < PDPTE_ENTRY_COUNT; i++)
    {
        UINT64 *PdPage = (UINT64*)((UINT64)Pd + (i * EFI_PAGE_SIZE));
        for (j = 0; j < PDE_ENTRY_COUNT; j++)
        {
            UINT64 PhysAddr = ((UINT64)i * PDE_ENTRY_COUNT + j) * _2MB;
            PdPage[j] = PhysAddr | 0x83;
        }
    }

    //
    // Override SystemDirectoryTableBase with our runtime page tables.
    //
    VpData->SystemDirectoryTableBase = (UINT64)Pml4;
    SerialPrint("[SHV]   HOST_CR3 = "); SerialPrintHex64((UINT64)Pml4); SerialPrint("\n");
    SerialPrint("[SHV] Host page tables built OK\n");
    return SHV_STATUS_SUCCESS;
}

INT32
ShvOsBuildHostIdt (
    _In_ PSHV_VP_DATA VpData
    )
{
    VOID *HostIdt;

    //
    // The UEFI firmware's IDT resides in boot services memory and will be
    // destroyed after ExitBootServices. Allocate a host IDT in runtime memory.
    // The hypervisor runs with interrupts disabled, so entries are zeroed --
    // but the memory itself must be valid to avoid triple faults if an NMI
    // arrives during VMX root mode execution.
    //
    SerialPrint("[SHV] Building host IDT in runtime memory\n");
    HostIdt = AllocateRuntimeZeroPool(EFI_PAGE_SIZE);
    if (HostIdt == NULL)
    {
        SerialPrint("[SHV] ERROR: Failed to allocate host IDT\n");
        return SHV_STATUS_NO_RESOURCES;
    }

    //
    // Store the host IDT base for VMCS HOST_IDTR_BASE
    //
    VpData->HostIdtBase = (UINT64)HostIdt;
    SerialPrint("[SHV]   HOST_IDTR_BASE = "); SerialPrintHex64((UINT64)HostIdt); SerialPrint("\n");
    SerialPrint("[SHV] Host IDT built OK\n");
    return SHV_STATUS_SUCCESS;
}

INT32
ShvOsPrepareProcessor (
    _In_ PSHV_VP_DATA VpData
    )
{
    PKGDTENTRY64 TssEntry, NewGdt;
    PKTSS64 Tss;
    KDESCRIPTOR Gdtr;
    INT32 status;

    SerialPrint("[SHV] === ShvOsPrepareProcessor START ===\n");

    //
    // Clear AC in case it's not been reset yet
    //
    __writeeflags(__readeflags() & ~EFLAGS_ALIGN_CHECK);

    //
    // Capture the current GDT
    //
    _sgdt(&Gdtr.Limit);
    SerialPrint("[SHV] Original GDTR base = "); SerialPrintHex64((UINT64)Gdtr.Base);
    SerialPrint(", limit = "); SerialPrintHex64((UINT64)Gdtr.Limit); SerialPrint("\n");

    //
    // Allocate a new GDT as big as the old one, or to cover selector 0x60
    //
    NewGdt = AllocateRuntimeZeroPool(MAX(Gdtr.Limit + 1,
                                     KGDT64_SYS_TSS + sizeof(*TssEntry)));
    if (NewGdt == NULL)
    {
        return SHV_STATUS_NO_RESOURCES;
    }

    //
    // Copy the old GDT
    //
    CopyMem(NewGdt, Gdtr.Base, Gdtr.Limit + 1);

    //
    // Allocate a TSS
    //
    Tss = AllocateRuntimeZeroPool(sizeof(*Tss));
    if (Tss == NULL)
    {
        FreePool(NewGdt);
        return SHV_STATUS_NO_RESOURCES;
    }

    //
    // Fill out the TSS Entry
    //
    TssEntry = (PKGDTENTRY64)((uintptr_t)NewGdt + KGDT64_SYS_TSS);
    TssEntry->BaseLow = (uintptr_t)Tss & 0xffff;
    TssEntry->Bits.BaseMiddle = ((uintptr_t)Tss >> 16) & 0xff;
    TssEntry->Bits.BaseHigh = ((uintptr_t)Tss >> 24) & 0xff;
    TssEntry->BaseUpper = (uintptr_t)Tss >> 32;
    TssEntry->LimitLow = sizeof(KTSS64) - 1;
    TssEntry->Bits.Type = AMD64_TSS;
    TssEntry->Bits.Dpl = 0;
    TssEntry->Bits.Present = 1;
    TssEntry->Bits.System = 0;
    TssEntry->Bits.LongMode = 0;
    TssEntry->Bits.DefaultBig = 0;
    TssEntry->Bits.Granularity = 0;
    TssEntry->MustBeZero = 0;

    //
    // Load the new GDT
    //
    Gdtr.Base = NewGdt;
    Gdtr.Limit = KGDT64_SYS_TSS + sizeof(*TssEntry) - 1;
    _lgdt(&Gdtr.Limit);

    //
    // Load the task register
    //
    _ltr(KGDT64_SYS_TSS);
    SerialPrint("[SHV] New GDT at "); SerialPrintHex64((UINT64)NewGdt);
    SerialPrint(", TSS at "); SerialPrintHex64((UINT64)Tss); SerialPrint("\n");

    //
    // Build identity-mapped host page tables in runtime memory. The UEFI
    // firmware's CR3 page tables will be destroyed after ExitBootServices,
    // so HOST_CR3 must point to our own persistent page tables.
    //
    status = ShvOsBuildHostPageTables(VpData);
    if (status != SHV_STATUS_SUCCESS)
    {
        SerialPrint("[SHV] ERROR: ShvOsBuildHostPageTables failed\n");
        return status;
    }

    //
    // Build a host IDT in runtime memory. The UEFI firmware's IDT will also
    // be destroyed after ExitBootServices.
    //
    status = ShvOsBuildHostIdt(VpData);
    if (status != SHV_STATUS_SUCCESS)
    {
        SerialPrint("[SHV] ERROR: ShvOsBuildHostIdt failed\n");
        return status;
    }

    SerialPrint("[SHV] === ShvOsPrepareProcessor DONE ===\n");
    return SHV_STATUS_SUCCESS;
}

VOID
ShvOsRunCallbackOnProcessors (
    _In_ PSHV_CPU_CALLBACK Routine,
    _In_ VOID* Context
    )
{
    //
    // Call the routine on the current CPU
    //
    Routine(Context);

    //
    // And then on all other processors
    //
    _gPiMpService->StartupAllAPs(_gPiMpService,
                                 Routine,
                                 TRUE,
                                 NULL,
                                 0,
                                 Context,
                                 NULL);
}

VOID
ShvOsFreeContiguousAlignedMemory (
    _In_ VOID* BaseAddress,
    _In_ size_t Size
    )
{
    //
    // Free the memory
    //
    FreeAlignedPages(BaseAddress, EFI_SIZE_TO_PAGES(Size));
}

VOID*
ShvOsAllocateContigousAlignedMemory (
    _In_ size_t Size
    )
{
    //
    // Allocate a contiguous chunk of RAM to back this allocation.
    //
    return AllocateAlignedRuntimePages(EFI_SIZE_TO_PAGES(Size), EFI_PAGE_SIZE);
}

UINT64
ShvOsGetPhysicalAddress (
    _In_ VOID* BaseAddress
    )
{
    //
    // UEFI runs with paging disabled
    //
    return (UINT64)BaseAddress;
}

INT32
ShvOsGetCurrentProcessorNumber (
    VOID
    )
{
    EFI_STATUS efiStatus;
    UINTN cpuIndex;

    //
    // Ask PI MP Services for the CPU index
    //
    efiStatus = _gPiMpService->WhoAmI(_gPiMpService, &cpuIndex);
    if (efiStatus != EFI_SUCCESS)
    {
        cpuIndex = ~0ULL;
    }

    //
    // Return the index
    //
    return (INT32)cpuIndex;
}

INT32
ShvOsGetActiveProcessorCount (
    VOID
    )
{
    EFI_STATUS efiStatus;
    UINTN cpuCount, enabledCpuCount;

    //
    // Ask PI MP Services for how many CPUs there are
    //
    efiStatus = _gPiMpService->GetNumberOfProcessors(_gPiMpService,
                                                     &cpuCount,
                                                     &enabledCpuCount);
    if (efiStatus != EFI_SUCCESS)
    {
        enabledCpuCount = 0;
    }

    //
    // Return the count
    //
    return (INT32)enabledCpuCount;
}

VOID
ShvOsDebugPrintWide (
    _In_ CHAR16* Format,
    ...
    )
{
    VA_LIST arglist;
    CHAR16* debugString;

    //
    // Call the debugger API
    //
    VA_START(arglist, Format);
    debugString = CatVSPrint(NULL, Format, arglist);
    Print(debugString);
    FreePool(debugString);
    VA_END(arglist);
}

EFI_STATUS
EFIAPI
UefiUnload (
    IN EFI_HANDLE ImageHandle
    )
{
    //
    // Call the hypervisor unloadpoint
    //
    ShvUnload();
    return EFI_SUCCESS;
}

EFI_STATUS
ShvOsChainLoadBootManager (
    IN EFI_HANDLE ImageHandle
    )
{
    EFI_STATUS efiStatus;
    EFI_LOADED_IMAGE_PROTOCOL* loadedImage;
    EFI_DEVICE_PATH_PROTOCOL* bootFilePath;
    EFI_HANDLE newImageHandle;

    SerialPrint("[SHV] Chain-loading real Windows Boot Manager\n");

    //
    // Get our loaded image protocol to find which device we were loaded from
    //
    efiStatus = gBS->HandleProtocol(ImageHandle,
                                    &gEfiLoadedImageProtocolGuid,
                                    (VOID**)&loadedImage);
    if (EFI_ERROR(efiStatus))
    {
        SerialPrint("[SHV] ERROR: Failed to get LoadedImage protocol\n");
        return efiStatus;
    }

    SerialPrint("[SHV]   Our DeviceHandle = ");
    SerialPrintHex64((UINT64)loadedImage->DeviceHandle);
    SerialPrint("\n");

    //
    // Build a file device path for the real boot manager on the same device.
    // Using FileDevicePath + LoadImage (with device path) is the correct way
    // to chain-load — it gives the boot manager full device/file context so
    // it can find BCD, winload.efi, and other boot files.
    //
    bootFilePath = FileDevicePath(loadedImage->DeviceHandle,
                                  L"\\EFI\\Microsoft\\Boot\\bootmgfwx.efi");
    if (bootFilePath == NULL)
    {
        SerialPrint("[SHV] ERROR: Failed to build device path\n");
        return EFI_OUT_OF_RESOURCES;
    }

    SerialPrint("[SHV]   Loading bootmgfwx.efi via device path\n");

    //
    // Load the real boot manager image using the device path
    //
    efiStatus = gBS->LoadImage(TRUE,
                               ImageHandle,
                               bootFilePath,
                               NULL,
                               0,
                               &newImageHandle);
    FreePool(bootFilePath);
    if (EFI_ERROR(efiStatus))
    {
        SerialPrint("[SHV] ERROR: LoadImage failed for bootmgfwx.efi\n");
        return efiStatus;
    }

    SerialPrint("[SHV]   Image loaded, handle = ");
    SerialPrintHex64((UINT64)newImageHandle);
    SerialPrint("\n");

    //
    // Start the real Windows Boot Manager. This call should not return
    // if Windows boots successfully.
    //
    SerialPrint("[SHV]   Calling StartImage (Windows Boot Manager)...\n");
    Print(L"SHV: Hypervisor active, starting Windows Boot Manager...\n");
    efiStatus = gBS->StartImage(newImageHandle, NULL, NULL);

    //
    // If we get here, the boot manager returned (error or exit)
    //
    SerialPrint("[SHV]   StartImage returned: ");
    SerialPrintHex64((UINT64)efiStatus);
    SerialPrint("\n");
    Print(L"SHV: Boot manager returned: %r\n", efiStatus);
    return efiStatus;
}

EFI_STATUS
EFIAPI
UefiMain (
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
    )
{
    EFI_STATUS efiStatus;

    SerialPortInitialize();
    SerialPrint("\n\n[SHV] ========================================\n");
    SerialPrint("[SHV] SimpleVisor UEFI Hypervisor Starting\n");
    SerialPrint("[SHV] ========================================\n");

    //
    // Find the PI MpService protocol used for multi-processor startup
    //
    SerialPrint("[SHV] Locating MP Services protocol...\n");
    efiStatus = gBS->LocateProtocol(&gEfiMpServiceProtocolGuid,
                                    NULL,
                                    &_gPiMpService);
    if (EFI_ERROR(efiStatus))
    {
        SerialPrint("[SHV] ERROR: MpServices not found\n");
        Print(L"Unable to locate the MpServices protocol: %r\n", efiStatus);
        return efiStatus;
    }
    SerialPrint("[SHV] MP Services located OK\n");

    //
    // Log current CR3 before hypervisor load
    //
    SerialPrint("[SHV] Current CR3 (UEFI page tables) = ");
    SerialPrintHex64(__readcr3());
    SerialPrint("\n");
    SerialPrint("[SHV] Current CR0 = ");
    SerialPrintHex64(__readcr0());
    SerialPrint("\n");
    SerialPrint("[SHV] Current CR4 = ");
    SerialPrintHex64(__readcr4());
    SerialPrint("\n");

    //
    // Start the hypervisor
    //
    SerialPrint("[SHV] Calling ShvLoad()...\n");
    efiStatus = ShvOsErrorToError(ShvLoad());
    if (EFI_ERROR(efiStatus))
    {
        SerialPrint("[SHV] ERROR: ShvLoad failed\n");
        Print(L"SHV: Failed to load hypervisor: %r\n", efiStatus);
        return efiStatus;
    }

    SerialPrint("[SHV] *** Hypervisor loaded successfully ***\n");
    Print(L"SHV: Hypervisor loaded successfully.\n");

    //
    // Chain-load the real Windows Boot Manager (bootmgfwx.efi)
    //
    return ShvOsChainLoadBootManager(ImageHandle);
}

