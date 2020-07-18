// Copyright (c) 2015-2018, Satoshi Tanda. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements shadow hook functions.

#include "shadow_hook.h"
#include <ntimage.h>
#define NTSTRSAFE_NO_CB_FUNCTIONS
#include <ntstrsafe.h>
#include "../HyperPlatform/HyperPlatform/common.h"
#include "../HyperPlatform/HyperPlatform/log.h"
#include "../HyperPlatform/HyperPlatform/util.h"
#include "../HyperPlatform/HyperPlatform/ept.h"
#undef _HAS_EXCEPTIONS
#define _HAS_EXCEPTIONS 0
#include <vector>
#include <memory>
#include <algorithm>
#include "capstone.h"

////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//
enum HOOK_TYPE {
  FUNC_HOOK,
  MEM_HOOK,
};

////////////////////////////////////////////////////////////////////////////////
//
// types
//

using MEMMONITOR = void(*)(ULONG64, ULONG64);

// Copy of a page seen by a guest as a result of memory shadowing
struct Page {
  UCHAR* page;  // A page aligned copy of a page
  Page();
  ~Page();
};

struct MemBPInformation {
  ULONG64 mem_address;
  ULONG64 mem_len;
  MEMMONITOR handler;
  std::shared_ptr<Page> shadow_page_base_for_rw;
  ULONG64 pa_base_for_rw;
};

struct MemHookInformation {
  void* va_base_page_hook;
  HOOK_TYPE hook_type;
};

// Contains a single steal hook information
struct FunctionHookInformation {
  void* patch_address;  // An address where a hook is installed
  void* handler;        // An address of the handler routine
  ULONG64 patch_length;
  UCHAR* new_code;  //a pointer to the patch code

  // A copy of a pages where patch_address belongs to. shadow_page_base_for_rw
  // is exposed to a guest for read and write operation against the page of
  // patch_address, and shadow_page_base_for_exec is exposed for execution.
  std::shared_ptr<Page> shadow_page_base_for_rw;
  std::shared_ptr<Page> shadow_page_base_for_exec;

  // Physical address of the above two copied pages
  ULONG64 pa_base_for_rw;
  ULONG64 pa_base_for_exec;
};

// Data structure shared across all processors
struct SharedShadowHookPatchData {
  std::vector<std::unique_ptr<MemHookInformation>> all_page_hooks;  // Hold installed hooks
  std::vector<std::unique_ptr<FunctionHookInformation>> func_hooks;  // Hold all hooks include the hooks with the same page
  std::vector<std::unique_ptr<MemBPInformation>> mem_hooks;  // Hold all hooks include the hooks with the same page

};

// Data structure for each processor
struct LastShadowHookData {
  const MemHookInformation* last_page_hook_info;  // Remember which hook hit the last
};

// A structure reflects inline hook code.
#include <pshpack1.h>
#if defined(_AMD64_)

struct TrampolineCode {
  UCHAR nop;
  UCHAR jmp[6];
  void* address;
};
static_assert(sizeof(TrampolineCode) == 15, "Size check");

#else

struct TrampolineCode {
  UCHAR nop;
  UCHAR push;
  void* address;
  UCHAR ret;
};
static_assert(sizeof(TrampolineCode) == 7, "Size check");

#endif
#include <poppack.h>

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_IRQL_requires_max_(PASSIVE_LEVEL) static std::unique_ptr<
  FunctionHookInformation> ShpCreateHookInformationPatch(_In_ SharedShadowHookPatchData*
    shared_sh_data,
    _In_ void* address,
    _In_ ShadowPatchTarget* target,
    _In_ MemHookInformation *mem_hook_info);

_IRQL_requires_max_(PASSIVE_LEVEL) static std::unique_ptr<
  FunctionHookInformation> ShpCreateHookInformation(_In_ SharedShadowHookPatchData*
    shared_sh_data,
    _In_ void* address,
    _In_ ShadowHookTarget* target,
    _In_ MemHookInformation *mem_hook_info);


_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(return) static bool ShpSetupPatch(_In_ void* patch_address,
  _In_ UCHAR* shadow_exec_page,
  _In_ UCHAR* patch_code,
  _In_ ULONG64 code_len);

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
_Success_(return) static bool ShpSetupInlineHook(
  _In_ void* patch_address, _In_ UCHAR* shadow_exec_page,
  _Out_ void** original_call_ptr);

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C static SIZE_T
ShpGetInstructionSize(_In_ void* address);

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C static TrampolineCode
ShpMakeTrampolineCode(_In_ void* hook_handler);

static MemHookInformation* ShpFindPageHookInfoByPage(
  _In_ const SharedShadowHookPatchData* shared_sh_data, _In_ void* address);

static void ShpEnablePageShadowingForExec(_In_ void* patch_address,
  _In_ ULONG64 pa_base_for_exec,
  _In_ EptData* ept_data);

static void ShpEnablePageShadowingForRW(_In_ const FunctionHookInformation& info,
  _In_ EptData* ept_data);

static void ShpDisablePageShadowingForFuncHook(_In_ const FunctionHookInformation& info,
  _In_ EptData* ept_data);

static void ShpSetMonitorTrapFlag(_In_ LastShadowHookData* sh_data,
  _In_ bool enable);

static void ShpSaveLastHookInfo(_In_ LastShadowHookData* sh_data,
  _In_ const MemHookInformation& info);

static const MemHookInformation* ShpRestoreLastHookInfo(
  _In_ LastShadowHookData* sh_data);

static bool ShpIsShadowHookActive(
  _In_ const SharedShadowHookPatchData* shared_sh_data);

static FunctionHookInformation* ShpFindFuncHookInfoByPage(
  _In_ const SharedShadowHookPatchData* shared_sh_data, _In_ void *address);

static FunctionHookInformation* ShpFindFuncHookInfoByAddress(
  _In_ const SharedShadowHookPatchData* shared_sh_data, _In_ void *address);

static void ShpDisablePageMonitorForRW(
  _In_ MemBPInformation *info, _In_ EptData* ept_data);

static std::unique_ptr<MemBPInformation> ShpCreateMemMonitorInformation(
  SharedShadowHookPatchData* shared_sh_data,
  ShadowMemMonitorTarget* target, MemHookInformation *mem_hook_info);

static MemBPInformation* ShpFindMemMonInfoByPage(
  const SharedShadowHookPatchData* shared_sh_data, void* address);

static void ShpEnablePageMonitorForRW(
  MemBPInformation* info, EptData* ept_data);

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(PAGE, ShAllocateShadowHookData)
#pragma alloc_text(PAGE, ShAllocateSharedShaowHookData)
#pragma alloc_text(PAGE, ShEnableHooks)
#pragma alloc_text(PAGE, ShInstallHook)
#pragma alloc_text(PAGE, ShpSetupInlineHook)
#pragma alloc_text(PAGE, ShpGetInstructionSize)
#pragma alloc_text(PAGE, ShpMakeTrampolineCode)
#pragma alloc_text(PAGE, ShFreeShadowHookData)
#pragma alloc_text(PAGE, ShFreeSharedShadowHookData)
#pragma alloc_text(PAGE, ShDisableHooks)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// Allocates per-processor shadow hook data
_Use_decl_annotations_ LastShadowHookData* ShAllocateShadowHookData() {
  PAGED_CODE();

  auto p = new LastShadowHookData();
  RtlFillMemory(p, sizeof(LastShadowHookData), 0);
  return p;
}

// Frees per-processor shadow hook data
_Use_decl_annotations_ void ShFreeShadowHookData(LastShadowHookData* sh_data) {
  PAGED_CODE();

  delete sh_data;
}

// Allocates processor-shared shadow hook data
_Use_decl_annotations_ EXTERN_C SharedShadowHookPatchData*
ShAllocateSharedShaowHookData() {
  PAGED_CODE();

  auto p = new SharedShadowHookPatchData();
  RtlFillMemory(p, sizeof(SharedShadowHookPatchData), 0);
  return p;
}

// Frees processor-shared shadow hook data
_Use_decl_annotations_ void ShFreeSharedShadowHookData(
  SharedShadowHookPatchData* shared_sh_data) {
  PAGED_CODE();

  delete shared_sh_data;
}

// Enables page shadowing for all hooks
_Use_decl_annotations_ NTSTATUS ShEnableHooks() {
  PAGED_CODE();

  return UtilForEachProcessor(
    [](void* context) {
    UNREFERENCED_PARAMETER(context);
    return UtilVmCall(HypercallNumber::kShEnablePageShadowing, nullptr);
  },
    nullptr);
}

// Disables page shadowing for all hooks
_Use_decl_annotations_ NTSTATUS ShDisableHooks() {
  PAGED_CODE();

  return UtilForEachProcessor(
    [](void* context) {
    UNREFERENCED_PARAMETER(context);
    return UtilVmCall(HypercallNumber::kShDisablePageShadowing, nullptr);
  },
    nullptr);
}

// Enables page shadowing for all hooks
_Use_decl_annotations_ NTSTATUS ShEnablePageShadowing(
  EptData* ept_data, const SharedShadowHookPatchData* shared_sh_data) {
  // HYPERPLATFORM_COMMON_DBG_BREAK();

  for (auto& info : shared_sh_data->all_page_hooks) {
    switch (info->hook_type) {
      case FUNC_HOOK: {
        FunctionHookInformation *func_hook_info = ShpFindFuncHookInfoByPage(shared_sh_data, info->va_base_page_hook);
        ShpEnablePageShadowingForExec(func_hook_info->patch_address, func_hook_info->pa_base_for_exec,
          ept_data);
        break;
      }
      case MEM_HOOK: {
        MemBPInformation *mem_hook_info = ShpFindMemMonInfoByPage(shared_sh_data, info->va_base_page_hook);
        ShpDisablePageMonitorForRW(mem_hook_info, ept_data);
        break;
      }
    }
  }

  return STATUS_SUCCESS;
}

// Disables page shadowing for all hooks
_Use_decl_annotations_ void ShVmCallDisablePageShadowing(
  EptData* ept_data, const SharedShadowHookPatchData* shared_sh_data) {
  // HYPERPLATFORM_COMMON_DBG_BREAK();

  for (auto& info : shared_sh_data->all_page_hooks) {
    switch (info->hook_type) {
    case FUNC_HOOK:
      ShpDisablePageShadowingForFuncHook(*ShpFindFuncHookInfoByPage(shared_sh_data, info->va_base_page_hook), ept_data);
      break;
    case MEM_HOOK:
      MemBPInformation *mem_monitor_info= ShpFindMemMonInfoByPage(shared_sh_data, info->va_base_page_hook);
      ShpEnablePageMonitorForRW(mem_monitor_info, ept_data);
      break;
    }

  }
}

// Handles #BP. Checks if the #BP happened on where DdiMon set a break point,
// and if so, modifies the contents of guest's IP to execute a corresponding
// hook handler.
_Use_decl_annotations_ bool ShHandleBreakpoint(
  LastShadowHookData* sh_data, const SharedShadowHookPatchData* shared_sh_data,
  void* guest_ip) {
  UNREFERENCED_PARAMETER(sh_data);

  if (!ShpIsShadowHookActive(shared_sh_data)) {
    return false;
  }

  const auto info = ShpFindPageHookInfoByPage(shared_sh_data, guest_ip);
  if (!info) {
    return false;
  }

  FunctionHookInformation *func_hook_info = ShpFindFuncHookInfoByAddress(shared_sh_data, guest_ip);
  if (!func_hook_info) {
    return false;
  }

  // Update guest's IP
  UtilVmWrite(VmcsField::kGuestRip, reinterpret_cast<ULONG_PTR>(func_hook_info->handler));
  return true;
}

// Handles MTF VM-exit. Re-enables the shadow hook and clears MTF.
_Use_decl_annotations_ void ShHandleMonitorTrapFlag(
  LastShadowHookData* sh_data, const SharedShadowHookPatchData* shared_sh_data,
  EptData* ept_data) {
  NT_VERIFY(ShpIsShadowHookActive(shared_sh_data));

  HYPERPLATFORM_LOG_INFO_SAFE("ShHandleMonitorTrapFlag");
  const auto info = ShpRestoreLastHookInfo(sh_data);
  switch (info->hook_type)
  {
    case FUNC_HOOK:
    {
      FunctionHookInformation *func_hook_info = ShpFindFuncHookInfoByPage(shared_sh_data, info->va_base_page_hook);
      ShpEnablePageShadowingForExec(func_hook_info->patch_address, func_hook_info->pa_base_for_exec,
        ept_data);
      break;
    }
    case MEM_HOOK:
    {
      MemBPInformation *mem_monitor_info = ShpFindMemMonInfoByPage(shared_sh_data, info->va_base_page_hook);
      ShpDisablePageMonitorForRW(mem_monitor_info, ept_data);
      break;
    }

  }

  ShpSetMonitorTrapFlag(sh_data, false);
}

// Handles EPT violation VM-exit.
_Use_decl_annotations_ void ShHandleEptViolation(
  LastShadowHookData* sh_data, const SharedShadowHookPatchData* shared_sh_data,
  EptData* ept_data, void* fault_va) {
  HYPERPLATFORM_LOG_INFO_SAFE("ShHandleEptViolation");
  if (!ShpIsShadowHookActive(shared_sh_data)) {
    return;
  }

  const auto info = ShpFindPageHookInfoByPage(shared_sh_data, fault_va);
  if (!info) {
    return;
  }
  // EPT violation was caused because a guest tried to read or write to a page
  // where currently set as execute only for protecting a hook. Let a guest
  // read or write a page from a read/write shadow page and run a single
  // instruction.
  switch (info->hook_type) {
    case FUNC_HOOK:
    {
      ShpEnablePageShadowingForRW(*ShpFindFuncHookInfoByPage(shared_sh_data, info->va_base_page_hook), ept_data);
      ShpSetMonitorTrapFlag(sh_data, true);
      ShpSaveLastHookInfo(sh_data, *info);
      break;
    }
    case MEM_HOOK:
    {
      MemBPInformation *mem_monitor_info = ShpFindMemMonInfoByPage(shared_sh_data, info->va_base_page_hook);
      ShpEnablePageMonitorForRW(mem_monitor_info, ept_data);
      ShpSetMonitorTrapFlag(sh_data, true);
      ShpSaveLastHookInfo(sh_data, *info);
      if ((ULONG64)fault_va >= mem_monitor_info->mem_address &&
        (ULONG64)fault_va <= mem_monitor_info->mem_address + mem_monitor_info->mem_len) {
        mem_monitor_info->handler((ULONG64)fault_va, UtilVmRead(VmcsField::kGuestRip));
        
      }
      break;
    }

  }

}


// Set up inline hook at the address without activating it
_Use_decl_annotations_ bool ShInstallPatch(
  SharedShadowHookPatchData* shared_sh_data, void* address,
  ShadowPatchTarget* target) {
  PAGED_CODE();

  auto mem_info = ShpFindPageHookInfoByPage(shared_sh_data, address);
  auto info = ShpCreateHookInformationPatch(
    shared_sh_data, reinterpret_cast<void*>(address), target, mem_info);
  if (!info) {
    return false;
  }

  if (!ShpSetupPatch(info->patch_address, info->shadow_page_base_for_exec->page,
    info->new_code, info->patch_length)) {
    return false;
  }

  if (!mem_info) {
    auto new_mem_info = std::make_unique<MemHookInformation>();
    new_mem_info->va_base_page_hook = PAGE_ALIGN((void*)target->target_address);
    new_mem_info->hook_type = FUNC_HOOK;
    shared_sh_data->all_page_hooks.push_back(std::move(new_mem_info));
  }

  HYPERPLATFORM_LOG_DEBUG(
    "Patch = %p, Exec = %p, RW = %p", info->patch_address,
    info->shadow_page_base_for_exec->page + BYTE_OFFSET(info->patch_address),
    info->shadow_page_base_for_rw->page + BYTE_OFFSET(info->patch_address));
  shared_sh_data->func_hooks.push_back(std::move(info));
  return true;
}

// Set up inline hook at the address without activating it
_Use_decl_annotations_ bool ShInstallHook(
  SharedShadowHookPatchData* shared_sh_data, void* address,
  ShadowHookTarget* target) {
  PAGED_CODE();

  auto mem_info = ShpFindPageHookInfoByPage(shared_sh_data, address);
  auto info = ShpCreateHookInformation(
    shared_sh_data, reinterpret_cast<void*>(address), target, mem_info);
  if (!info) {
    return false;
  }

  if (!ShpSetupInlineHook(info->patch_address,
    info->shadow_page_base_for_exec->page,
    &target->original_call)) {
    return false;
  }

  HYPERPLATFORM_LOG_DEBUG(
    "Patch = %p, Exec = %p, RW = %p, Trampoline = %p", info->patch_address,
    info->shadow_page_base_for_exec->page + BYTE_OFFSET(info->patch_address),
    info->shadow_page_base_for_rw->page + BYTE_OFFSET(info->patch_address),
    target->original_call);

  if (!mem_info) {
    auto new_mem_info = std::make_unique<MemHookInformation>();
    new_mem_info->va_base_page_hook = (void*)target->target_address;
    new_mem_info->hook_type = FUNC_HOOK;
    shared_sh_data->all_page_hooks.push_back(std::move(new_mem_info));
  }

  shared_sh_data->func_hooks.push_back(std::move(info));
  return true;
}



// Creates or reuses a couple of copied pages and initializes HookInformation
_Use_decl_annotations_ static std::unique_ptr<FunctionHookInformation>
ShpCreateHookInformationPatch(SharedShadowHookPatchData* shared_sh_data,
  void* address, ShadowPatchTarget* target, MemHookInformation *mem_hook_info) {

  auto info = std::make_unique<FunctionHookInformation>();
  FunctionHookInformation *reusable_info = nullptr;
  bool page_hook_exist = false;
  if (mem_hook_info != nullptr) {
    page_hook_exist = true;
    reusable_info = ShpFindFuncHookInfoByPage(shared_sh_data, address);
    if (reusable_info) {
      // Found an existing HookInformation object targeting the same page as this
      // one. re-use shadow pages.
      info->shadow_page_base_for_rw = reusable_info->shadow_page_base_for_rw;
      info->shadow_page_base_for_exec = reusable_info->shadow_page_base_for_exec;
    }
  }

  if (page_hook_exist == false) {
    // This hook is for a page that is not currently have any hooks (i.e., not
    // shadowed). Creates shadow pages.
    info->shadow_page_base_for_rw = std::make_shared<Page>();
    info->shadow_page_base_for_exec = std::make_shared<Page>();
    auto page_base = PAGE_ALIGN(address);
    RtlCopyMemory(info->shadow_page_base_for_rw->page, page_base, PAGE_SIZE);
    RtlCopyMemory(info->shadow_page_base_for_exec->page, page_base, PAGE_SIZE);
  }

  info->patch_address = address;
  info->pa_base_for_rw = UtilPaFromVa(info->shadow_page_base_for_rw->page);
  info->pa_base_for_exec = UtilPaFromVa(info->shadow_page_base_for_exec->page);
  info->patch_length = target->patch_length;
  info->new_code = target->new_code;

  return info;
}

// Creates or reuses a couple of copied pages and initializes HookInformation
_Use_decl_annotations_ static std::unique_ptr<FunctionHookInformation>
ShpCreateHookInformation(SharedShadowHookPatchData* shared_sh_data,
  void* address, ShadowHookTarget* target, MemHookInformation *mem_hook_info) {

  auto info = std::make_unique<FunctionHookInformation>();
  FunctionHookInformation *reusable_info = nullptr;
  bool page_hook_exist = false;
  if (mem_hook_info != nullptr) {
    page_hook_exist = true;
    reusable_info = ShpFindFuncHookInfoByPage(shared_sh_data, address);
    if (reusable_info) {
      // Found an existing HookInformation object targeting the same page as this
      // one. re-use shadow pages.
      info->shadow_page_base_for_rw = reusable_info->shadow_page_base_for_rw;
      info->shadow_page_base_for_exec = reusable_info->shadow_page_base_for_exec;
    }
  }

  if (page_hook_exist == false) {
    // This hook is for a page that is not currently have any hooks (i.e., not
    // shadowed). Creates shadow pages.

    info->shadow_page_base_for_rw = std::make_shared<Page>();
    info->shadow_page_base_for_exec = std::make_shared<Page>();
    auto page_base = PAGE_ALIGN(address);
    RtlCopyMemory(info->shadow_page_base_for_rw->page, page_base, PAGE_SIZE);
    RtlCopyMemory(info->shadow_page_base_for_exec->page, page_base, PAGE_SIZE);
  }

  info->patch_address = address;
  info->pa_base_for_rw = UtilPaFromVa(info->shadow_page_base_for_rw->page);
  info->pa_base_for_exec = UtilPaFromVa(info->shadow_page_base_for_exec->page);
  info->handler = target->handler;

  return info;
}

// Builds a trampoline code for calling an original code and embeds 0xcc on the
// shadow_exec_page
_Use_decl_annotations_ static bool ShpSetupPatch(void* patch_address,
  UCHAR* shadow_exec_page,
  UCHAR* patch_code,
  ULONG64 code_len) {
  PAGED_CODE();

  if (!code_len) {
    return false;
  }

  RtlCopyMemory(shadow_exec_page + BYTE_OFFSET(patch_address), patch_code,
    code_len);

  KeInvalidateAllCaches();
  return true;
}

// Builds a trampoline code for calling an original code and embeds 0xcc on the
// shadow_exec_page
_Use_decl_annotations_ static bool ShpSetupInlineHook(
  void* patch_address, UCHAR* shadow_exec_page, void** original_call_ptr) {
  PAGED_CODE();

  const auto patch_size = ShpGetInstructionSize(patch_address);
  if (!patch_size) {
    return false;
  }

  // Build trampoline code (copied stub -> in the middle of original)
  const auto jmp_to_original = ShpMakeTrampolineCode(
    reinterpret_cast<UCHAR*>(patch_address) + patch_size);
#pragma warning(push)
#pragma warning(disable : 30030)  // Allocating executable POOL_TYPE memory
  const auto original_call = ExAllocatePoolWithTag(
    NonPagedPoolExecute, patch_size + sizeof(jmp_to_original),
    kHyperPlatformCommonPoolTag);
#pragma warning(pop)
  if (!original_call) {
    return false;
  }

  // Copy original code and embed jump code following original code
  RtlCopyMemory(original_call, patch_address, patch_size);
#pragma warning(push)
#pragma warning(disable : 6386)
  // Buffer overrun while writing to 'reinterpret_cast<UCHAR
  // *>original_call+patch_size':  the writable size is
  // 'patch_size+sizeof((jmp_to_original))' bytes, but '15' bytes might be
  // written.
  RtlCopyMemory(reinterpret_cast<UCHAR*>(original_call) + patch_size,
    &jmp_to_original, sizeof(jmp_to_original));
#pragma warning(pop)

  // install patch to shadow page
  static const UCHAR kBreakpoint[] = {
      0xcc,
  };
  RtlCopyMemory(shadow_exec_page + BYTE_OFFSET(patch_address), kBreakpoint,
    sizeof(kBreakpoint));

  KeInvalidateAllCaches();

  *original_call_ptr = original_call;
  return true;
}

// Returns a size of an instruction at the address
_Use_decl_annotations_ static SIZE_T ShpGetInstructionSize(void* address) {
  PAGED_CODE();

  // Save floating point state
  KFLOATING_SAVE float_save = {};
  auto status = KeSaveFloatingPointState(&float_save);
  if (!NT_SUCCESS(status)) {
    return 0;
  }

  // Disassemble at most 15 bytes to get an instruction size
  csh handle = {};
  const auto mode = IsX64() ? CS_MODE_64 : CS_MODE_32;
  if (cs_open(CS_ARCH_X86, mode, &handle) != CS_ERR_OK) {
    KeRestoreFloatingPointState(&float_save);
    return 0;
  }

  static const auto kLongestInstSize = 15;
  cs_insn* instructions = nullptr;
  const auto count =
    cs_disasm(handle, reinterpret_cast<uint8_t*>(address), kLongestInstSize,
      reinterpret_cast<uint64_t>(address), 1, &instructions);
  if (count == 0) {
    cs_close(&handle);
    KeRestoreFloatingPointState(&float_save);
    return 0;
  }

  // Get a size of the first instruction
  const auto size = instructions[0].size;
  cs_free(instructions, count);
  cs_close(&handle);

  // Restore floating point state
  KeRestoreFloatingPointState(&float_save);
  return size;
}

// Returns code bytes for inline hooking
_Use_decl_annotations_ static TrampolineCode ShpMakeTrampolineCode(
  void* hook_handler) {
  PAGED_CODE();

#if defined(_AMD64_)
  // 90               nop
  // ff2500000000     jmp     qword ptr cs:jmp_addr
  // jmp_addr:
  // 0000000000000000 dq 0
  return {
      0x90,
      {
          0xff,
          0x25,
          0x00,
          0x00,
          0x00,
          0x00,
      },
      hook_handler,
  };
#else
  // 90               nop
  // 6832e30582       push    offset nt!ExFreePoolWithTag + 0x2 (8205e332)
  // c3               ret
  return {
      0x90,
      0x68,
      hook_handler,
      0xc3,
  };
#endif
}

// Find a HookInformation instance by address
_Use_decl_annotations_ static MemHookInformation* ShpFindPageHookInfoByPage(
  const SharedShadowHookPatchData* shared_sh_data, void* address) {
  const auto found = std::find_if(
    shared_sh_data->all_page_hooks.cbegin(), shared_sh_data->all_page_hooks.cend(),
    [address](const auto& info) {
    return PAGE_ALIGN(info->va_base_page_hook) == PAGE_ALIGN(address);
  });
  if (found == shared_sh_data->all_page_hooks.cend()) {
    return nullptr;
  }
  return found->get();
}

_Use_decl_annotations_ static void ShpEnablePageMonitorForRW(
  MemBPInformation* info, EptData* ept_data) {
  const auto pa_base = UtilPaFromVa(PAGE_ALIGN(info->mem_address));
  const auto ept_pt_entry =
    EptGetEptPtEntry(ept_data, pa_base);

  // Allow the VMM to redirect read and write access to the address by denying
  // those accesses and handling them on EPT violation
  ept_pt_entry->fields.write_access = true;
  ept_pt_entry->fields.read_access = true;
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(pa_base);

  UtilInveptGlobal();
}

// Show a shadowed page for execution
_Use_decl_annotations_ static void ShpEnablePageShadowingForExec(
  void* patch_address, ULONG64 pa_base_for_exec, EptData* ept_data) {
  const auto ept_pt_entry =
    EptGetEptPtEntry(ept_data, UtilPaFromVa(patch_address));

  // Allow the VMM to redirect read and write access to the address by denying
  // those accesses and handling them on EPT violation
  ept_pt_entry->fields.write_access = false;
  ept_pt_entry->fields.read_access = false;

  // Only execution is allowed on the address. Show the copied page for exec
  // that has an actual breakpoint to the guest.
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(pa_base_for_exec);

  UtilInveptGlobal();
}

// Show a shadowed page for read and write
_Use_decl_annotations_ static void ShpEnablePageShadowingForRW(
  const FunctionHookInformation& info, EptData* ept_data) {


  const auto ept_pt_entry =
    EptGetEptPtEntry(ept_data, UtilPaFromVa(info.patch_address));

  // Allow a guest to read and write as well as execute the address. Show the
  // copied page for read/write that does not have an breakpoint but reflects
  // all modification by a guest if that happened.
  ept_pt_entry->fields.write_access = true;
  ept_pt_entry->fields.read_access = true;
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(info.pa_base_for_rw);

  UtilInveptGlobal();
}

// Stop showing a shadow page
_Use_decl_annotations_ static void ShpDisablePageShadowingForFuncHook(
  const FunctionHookInformation& info, EptData* ept_data) {
  const auto pa_base = UtilPaFromVa(PAGE_ALIGN(info.patch_address));
  const auto ept_pt_entry = EptGetEptPtEntry(ept_data, pa_base);
  ept_pt_entry->fields.write_access = true;
  ept_pt_entry->fields.read_access = true;
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(pa_base);

  UtilInveptGlobal();
}

// Stop showing a shadow page
_Use_decl_annotations_ static void ShpDisablePageMonitorForRW(
  MemBPInformation *info, EptData* ept_data) {
  const auto pa_base = UtilPaFromVa(PAGE_ALIGN(info->mem_address));
  const auto ept_pt_entry = EptGetEptPtEntry(ept_data, pa_base);
  ept_pt_entry->fields.write_access = false;
  ept_pt_entry->fields.read_access = false;
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(pa_base);

  UtilInveptGlobal();
}

// Set MTF on the current processor
_Use_decl_annotations_ static void ShpSetMonitorTrapFlag(
  LastShadowHookData* sh_data, bool enable) {
  VmxProcessorBasedControls vm_procctl = {
      static_cast<unsigned int>(UtilVmRead(VmcsField::kCpuBasedVmExecControl)) };
  vm_procctl.fields.monitor_trap_flag = enable;
  UtilVmWrite(VmcsField::kCpuBasedVmExecControl, vm_procctl.all);
}

// Saves HookInformation as the last one for reusing it on up coming MTF VM-exit
_Use_decl_annotations_ static void ShpSaveLastHookInfo(
  LastShadowHookData* sh_data, const MemHookInformation& info) {
  NT_ASSERT(!sh_data->last_page_hook_info);
  sh_data->last_page_hook_info = &info;
}

// Retrieves the last HookInformation
_Use_decl_annotations_ static const MemHookInformation* ShpRestoreLastHookInfo(
  LastShadowHookData* sh_data) {
  NT_ASSERT(sh_data->last_page_hook_info);
  auto info = sh_data->last_page_hook_info;
  sh_data->last_page_hook_info = nullptr;
  return info;
}

// Checks if DdiMon is already initialized
_Use_decl_annotations_ static bool ShpIsShadowHookActive(
  const SharedShadowHookPatchData* shared_sh_data) {
  return !!(shared_sh_data);
}

// Allocates a non-paged, page-aligned page. Issues bug check on failure
Page::Page()
  : page(reinterpret_cast<UCHAR*>(ExAllocatePoolWithTag(
    NonPagedPool, PAGE_SIZE, kHyperPlatformCommonPoolTag))) {
  if (!page) {
    HYPERPLATFORM_COMMON_BUG_CHECK(
      HyperPlatformBugCheck::kCritialPoolAllocationFailure, 0, 0, 0);
  }
}

// De-allocates the allocated page
Page::~Page() { ExFreePoolWithTag(page, kHyperPlatformCommonPoolTag); }

_Use_decl_annotations_ static FunctionHookInformation* ShpFindFuncHookInfoByPage(
  const SharedShadowHookPatchData* shared_sh_data, void *address) {

  auto func_hook = std::find_if(
    shared_sh_data->func_hooks.cbegin(), shared_sh_data->func_hooks.cend(),
    [address](const auto& info) { return PAGE_ALIGN(info->patch_address) == PAGE_ALIGN(address); });
  if (func_hook == shared_sh_data->func_hooks.cend()) {
    return nullptr;
  }

  return func_hook->get();
}

_Use_decl_annotations_ static FunctionHookInformation* ShpFindFuncHookInfoByAddress(
  const SharedShadowHookPatchData* shared_sh_data, void *address) {

  auto func_hook = std::find_if(
    shared_sh_data->func_hooks.cbegin(), shared_sh_data->func_hooks.cend(),
    [address](const auto& info) { return info->patch_address == address; });
  if (func_hook == shared_sh_data->func_hooks.cend()) {
    return nullptr;
  }

  return func_hook->get();
}

_Use_decl_annotations_ EXTERN_C bool ShInstallMemMonitor(
  SharedShadowHookPatchData* shared_sh_data, ShadowMemMonitorTarget *target) {
  PAGED_CODE();

  auto mem_info = ShpFindPageHookInfoByPage(shared_sh_data, (void*)target->target_address);
  auto info = ShpCreateMemMonitorInformation(shared_sh_data, target, mem_info);
  if (!info) {
    return false;
  }

  if (!mem_info) {
    auto new_mem_info = std::make_unique<MemHookInformation>();
    new_mem_info->va_base_page_hook = PAGE_ALIGN((void*)target->target_address);
    new_mem_info->hook_type = MEM_HOOK;
    shared_sh_data->all_page_hooks.push_back(std::move(new_mem_info));
  }

  HYPERPLATFORM_LOG_DEBUG(
    "MemMon = %p, RW = %p", info->mem_address,
    info->shadow_page_base_for_rw->page + BYTE_OFFSET(info->mem_address));
  shared_sh_data->mem_hooks.push_back(std::move(info));
  return true;
}

// Creates or reuses a couple of copied pages and initializes HookInformation
_Use_decl_annotations_ static std::unique_ptr<MemBPInformation>
ShpCreateMemMonitorInformation(SharedShadowHookPatchData* shared_sh_data,
  ShadowMemMonitorTarget* target, MemHookInformation *mem_hook_info) {

  auto info = std::make_unique<MemBPInformation>();
  MemBPInformation *reusable_info = nullptr;
  bool page_hook_exist = false;
  if (mem_hook_info != nullptr) {
    page_hook_exist = true;
    reusable_info = ShpFindMemMonInfoByPage(shared_sh_data, (void*)target->target_address);
    if (reusable_info) {
      // Found an existing HookInformation object targeting the same page as this
      // one. re-use shadow pages.
      info->shadow_page_base_for_rw = reusable_info->shadow_page_base_for_rw;
    }
  }

  if (page_hook_exist == false) {
    // This hook is for a page that is not currently have any hooks (i.e., not
    // shadowed). Creates shadow pages.

    info->shadow_page_base_for_rw = std::make_shared<Page>();
    auto page_base = PAGE_ALIGN(target->target_address);
    RtlCopyMemory(info->shadow_page_base_for_rw->page, page_base, PAGE_SIZE);
  }

  info->mem_address = target->target_address;
  info->pa_base_for_rw = UtilPaFromVa(info->shadow_page_base_for_rw->page);
  info->mem_len = target->len;
  info->handler = (MEMMONITOR)target->handler;

  return info;
}

// Find a HookInformation instance by address
_Use_decl_annotations_ static MemBPInformation* ShpFindMemMonInfoByPage(
  const SharedShadowHookPatchData* shared_sh_data, void* address) {
  const auto found = std::find_if(
    shared_sh_data->mem_hooks.cbegin(), shared_sh_data->mem_hooks.cend(),
    [address](const auto& info) {
    return PAGE_ALIGN(info->mem_address) == PAGE_ALIGN(address);
  });
  if (found == shared_sh_data->mem_hooks.cend()) {
    return nullptr;
  }
  return found->get();
}