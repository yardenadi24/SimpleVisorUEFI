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
#include <Protocol/LoadedImage.h>
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
// TSS Segment we will use
//
#define KGDT64_SYS_TSS          0x60

//
// Original ExitBootServices function pointer, saved before hooking.
//
EFI_EXIT_BOOT_SERVICES _gOriginalExitBootServices;

//
// Our loaded image base and size, used to protect our memory from reclamation.
//
EFI_PHYSICAL_ADDRESS _gImageBase;
UINT64 _gImageSize;

//
// UEFI runs with identity mapping, so physical == virtual.
// This helper converts a PFN-masked physical address to a pointer.
//
#define PA_TO_PTR(pa) ((UINT64*)(UINTN)(pa))

#define PTE_PRESENT  0x1ULL
#define PTE_LARGE    0x80ULL
#define PTE_PFN_MASK 0x000FFFFFFFFFF000ULL

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

INT32
ShvOsBuildHostState(
    _In_ PSHV_VP_DATA VpData
)
{
    //
    // === Host Page Tables (identity map, 2MB large pages) ===
    //
    // SDM Vol 3, 27.5.4: HOST_CR3 is loaded on every VM-exit.
    // We need our own page tables in RuntimeServices memory
    // so they survive ExitBootServices.
    //
    // Structure: PML4 -> 512 PDPTs -> 512*512 PDEs
    // Covers 512 GB which is sufficient for physical RAM access.
    //

    UINT64* pml4 = AllocateRuntimeZeroPool(PAGE_SIZE);
    UINT64* pdpt = AllocateRuntimeZeroPool(PAGE_SIZE);
    UINT64* pd = AllocateRuntimeZeroPool(512 * PAGE_SIZE);
    if (pml4 == NULL || pdpt == NULL || pd == NULL)
    {
        return SHV_STATUS_NO_RESOURCES;
    }

    //
    // PML4[0] -> PDPT
    //
    pml4[0] = (UINT64)pdpt | 0x3;  // Present | RW
    //
    // PDPT[i] -> PD[i]
    //
    for (UINT32 i = 0; i < 512; i++)
    {
        pdpt[i] = ((UINT64)pd + i * PAGE_SIZE) | 0x3;  // Present | RW
    }
    //
    // PD[i][j] -> 2MB identity map
    // Each entry: Present | RW | PS (bit 7 = large page)
    //
    for (UINT32 i = 0; i < 512; i++)
    {
        UINT64* table = (UINT64*)((UINT8*)pd + i * PAGE_SIZE);
        for (UINT32 j = 0; j < 512; j++)
        {
            UINT64 physAddr = ((UINT64)i * 512 + j) * _2MB;
            table[j] = physAddr | 0x83;  // Present | RW | PS
        }
    }
    VpData->HostCr3 = (UINT64)pml4;

    //
    // === Host IDT ===
    //
    // SDM Vol 3, 27.5.3: HOST_IDTR_BASE loaded on exit, limit
    // forced to 0xFFFF. We need at minimum a vector 2 (NMI) entry
    // to avoid triple-fault if NMI fires during hypervisor execution.
    //
    // For simplicity, allocate a zeroed IDT. An unhandled interrupt
    // in the host will triple-fault, but that only happens if NMI
    // fires in the tiny window between VM-exit and VMRESUME.
    // A proper NMI gate can be added later.
    //
    VpData->HostIdtBase = AllocateRuntimeZeroPool(PAGE_SIZE);
    if (VpData->HostIdtBase == NULL)
    {
        return SHV_STATUS_NO_RESOURCES;
    }
    return SHV_STATUS_SUCCESS;
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

UINT64*
ShvCopyPageTableLevel (
    _In_ UINT64* SourceTable,
    _In_ UINT32 Level
    )
{    
    UINT64* newTable;
    UINT32 i;
    //
    // Allocate a runtime copy of this table page
    //
    newTable = AllocateRuntimeZeroPool(PAGE_SIZE);
    if (newTable == NULL)
    {
        return NULL;
    }
    //
    // Copy the raw content first (preserves all flags/bits)
    //
    CopyMem(newTable, SourceTable, PAGE_SIZE);

    //
    // If we're at the lowest level that has children (Level 1 = PT),
    // the entries point to actual 4KB data pages, not sub-tables.
    // We don't copy data pages -- only table pages. So stop recursing.
    //
    if (Level == 1)
    {
        return newTable;
    }

    //
    // Walk each of the 512 entries in this table
    //
    for (i = 0; i < 512; i++)
    {
        //
        // Skip non-present entries -- nothing to copy
        //
        if ((newTable[i] & PTE_PRESENT) == 0)
        {
            continue;
        }

        //
        // Large/huge page entries are leaf entries (1GB at PDPT level,
        // 2MB at PD level). They map physical memory directly and have
        // no child table to copy. Skip them.
        //
        // Level 4 = PML4 (never large)
        // Level 3 = PDPT (bit 7 = 1GB huge page)
        // Level 2 = PD   (bit 7 = 2MB large page)
        // Level 1 = PT   (always leaf, handled above)
        //
        if ((Level <= 3) && (newTable[i] & PTE_LARGE))
        {
            continue;
        }

        //
        // This entry points to a child table. Get its physical address,
        // which under UEFI identity mapping IS the virtual address.
        //
        UINT64 childPhys = newTable[i] & PTE_PFN_MASK;
        UINT64* childTable = PA_TO_PTR(childPhys);

        //
        // Recursively copy the child table
        //
        UINT64* newChild = ShvCopyPageTableLevel(childTable, Level - 1);
        if (newChild == NULL)
        {
            //
            // Allocation failed. In a simple implementation, we just bail.
            // The partially copied tree will leak runtime pages, which is
            // acceptable for a simple hypervisor.
            //
            FreePool(newTable);
            return NULL;
        }

        //
        // Rewrite this entry to point to the copy instead of the original.
        // Preserve all flag bits (low 12 and high bits), replace only the PFN.
        //
        newTable[i] = ((UINT64)(UINTN)newChild & PTE_PFN_MASK) |
                      (newTable[i] & ~PTE_PFN_MASK);
    }

    return newTable;
}

INT32
ShvCopyPageTables (
    _In_ PSHV_VP_DATA VpData
    )
{
    UINT64 cr3 = __readcr3();
    UINT64 cr4 = __readcr4();
    UINT64* rootTable;
    UINT64* newRoot;
    UINT32 levels;

    //
    // Determine paging depth.
    // CR4.LA57 (bit 12) indicates 5-level paging (PML5).
    // Most systems use 4-level (PML4).
    //
    if (cr4 & (1ULL << 12))
    {
        levels = 5;
    }
    else
    {
        levels = 4;
    }

    //
    // CR3 contains the physical address of the root table.
    // Under UEFI identity mapping, physical == virtual.
    // Mask off the PCID/flag bits in CR3 (low 12 bits for non-PCID,
    // though UEFI typically doesn't use PCID).
    //
    rootTable = PA_TO_PTR(cr3 & PTE_PFN_MASK);

    //
    // Deep-copy the entire page table tree
    //
    newRoot = ShvCopyPageTableLevel(rootTable, levels);
    if (newRoot == NULL)
    {
        return SHV_STATUS_NO_RESOURCES;
    }

    //
    // Store the physical address of our copy.
    // Under identity mapping, the pointer IS the physical address.
    //
    VpData->HostCr3 = (UINT64)(UINTN)newRoot;
    return SHV_STATUS_SUCCESS;
}

INT32
ShvOsPrepareProcessor (
    _In_ PSHV_VP_DATA VpData
    )
{
    PKGDTENTRY64 TssEntry, NewGdt;
    PKTSS64 Tss;
    KDESCRIPTOR Gdtr, Idtr;
    void* NewIdt;
    INT32 status;


    //
    // Clear AC in case it's not been reset yet
    //
    __writeeflags(__readeflags() & ~EFLAGS_ALIGN_CHECK);

    //
    // Capture the current GDT
    //
    _sgdt(&Gdtr.Limit);

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

    __sidt(&Idtr.Limit);

    NewIdt = AllocateRuntimeZeroPool(Idtr.Limit + 1);
    if (NewIdt == NULL)
    {
        return SHV_STATUS_NO_RESOURCES;
    }

    CopyMem(NewIdt, Idtr.Base, Idtr.Limit + 1);
    VpData->HostIdtBase = NewIdt;

        status = ShvCopyPageTables(VpData);
    if (status != SHV_STATUS_SUCCESS)
    {
        return status;
    }

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

//
// ShvProtectImageMemory
//
// Converts our .efi image pages from EfiBootServicesCode to
// EfiRuntimeServicesCode in the firmware's internal memory map. This prevents
// the OS loader from reclaiming our code after ExitBootServices.
//
// Strategy: Free our image pages (marking them as available), then immediately
// re-allocate them at the same address as EfiRuntimeServicesCode.
//
VOID
ShvProtectImageMemory(
    VOID
)
{
    EFI_STATUS status;
    UINTN pageCount;
    EFI_PHYSICAL_ADDRESS allocAddress;

    pageCount = EFI_SIZE_TO_PAGES(_gImageSize);
    allocAddress = _gImageBase;

    //
    // Free the pages (changes type to EfiConventionalMemory in firmware map)
    //
    status = gBS->FreePages(_gImageBase, pageCount);
    if (EFI_ERROR(status))
    {
        Print(L"Warning: FreePages failed for image protection: %r\n", status);
        return;
    }

    //
    // Re-allocate at the same address as EfiRuntimeServicesCode
    //
    status = gBS->AllocatePages(AllocateAddress,
        EfiRuntimeServicesCode,
        pageCount,
        &allocAddress);
    if (EFI_ERROR(status))
    {
        Print(L"CRITICAL: AllocatePages failed for image protection: %r\n", status);
    }
}

//
// ShvExitBootServicesHook
//
// Hooked ExitBootServices. This is called by the OS loader (winload.efi) when
// it is ready to take over the machine. We restore the original pointer and
// call through. The real protection was already done by ShvProtectImageMemory
// above; this hook is an additional safety net and logging point.
//
EFI_STATUS
EFIAPI
ShvExitBootServicesHook(
    IN EFI_HANDLE ImageHandle,
    IN UINTN MapKey
)
{
    //
    // Restore the original ExitBootServices pointer before doing anything.
    // This ensures our hook only fires once and recursive calls go to firmware.
    //
    gBS->ExitBootServices = _gOriginalExitBootServices;

    //
    // Call the original ExitBootServices with the caller's MapKey.
    //
    return _gOriginalExitBootServices(ImageHandle, MapKey);
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
EFIAPI
UefiMain (
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
    )
{
    EFI_STATUS efiStatus;
    EFI_LOADED_IMAGE_PROTOCOL* loadedImage;
    //
    // Find the PI MpService protocol used for multi-processor startup
    //
    efiStatus = gBS->LocateProtocol(&gEfiMpServiceProtocolGuid,
                                    NULL,
                                    &_gPiMpService);
    if (EFI_ERROR(efiStatus))
    {
        Print(L"Unable to locate the MpServices protocol: %r\n", efiStatus);
        return efiStatus;
    }

    //
// Get our loaded image information so we know our base address and size.
// We need this to protect our image memory from being reclaimed after
// ExitBootServices.
//
    efiStatus = gBS->HandleProtocol(ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID**)&loadedImage);
    if (EFI_ERROR(efiStatus))
    {
        Print(L"Unable to get loaded image protocol: %r\n", efiStatus);
        return efiStatus;
    }

    _gImageBase = (EFI_PHYSICAL_ADDRESS)loadedImage->ImageBase;
    _gImageSize = loadedImage->ImageSize;

    //
    // Protect our image memory by converting it from EfiBootServicesCode to
    // EfiRuntimeServicesCode in the firmware's memory map. This must be done
    // BEFORE the OS loader calls GetMemoryMap.
    //
    ShvProtectImageMemory();

    //
    // Hook ExitBootServices as an additional safety net.
    //
    _gOriginalExitBootServices = gBS->ExitBootServices;
    gBS->ExitBootServices = ShvExitBootServicesHook;

    if (EFI_ERROR(efiStatus))
    {
        //
        // If we failed to load, restore ExitBootServices to the original
        //
        gBS->ExitBootServices = _gOriginalExitBootServices;
    }

    //
    // Call the hypervisor entrypoint
    //
    return ShvOsErrorToError(ShvLoad());
}

