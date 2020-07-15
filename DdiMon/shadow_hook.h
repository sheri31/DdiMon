// Copyright (c) 2015-2018, Satoshi Tanda. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// @brief Declares interfaces to shadow hook functions.

#ifndef DDIMON_SHADOW_HOOK_H_
#define DDIMON_SHADOW_HOOK_H_

#include <fltKernel.h>

////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

enum HOOKED_FUNC_TYPE{
    UNEXPORT_FUNCTION,
    EXPORT_FUNCTION,
};

////////////////////////////////////////////////////////////////////////////////
//
// types
//

struct EptData;
struct LastShadowHookData;
struct SharedShadowHookPatchData;

// A callback type for g_ddimonp_hook_targets
using ShadowHookTargetInitCallbackType = bool(*)(
    ULONG64 *ptarget_address);


// Expresses where to patch by a function name or function address
struct ShadowPatchTarget {
    HOOKED_FUNC_TYPE function_type;
    UNICODE_STRING target_name;  // An exported name to hook
    ULONG64 target_address;  //An unexported function address to patch
    ULONG64 patch_length;  
    UCHAR new_code[0x100]; //
    ShadowHookTargetInitCallbackType target_init_callback; // only for unexported function which need to be located
};

// Expresses where to install hooks by a function name or function address, and its handlers
struct ShadowHookTarget {
    HOOKED_FUNC_TYPE function_type;
    UNICODE_STRING target_name;  // An exported name to hook
    ULONG64 target_address;  //An unexported function address to hook
    ShadowHookTargetInitCallbackType target_init_callback; // only for unexported function which need to be located
    void *handler;               // An address of a hook handler

    // An address of a trampoline code to call original function. Initialized by
    // a successful call of ShInstallHook().
    void *original_call;
};

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
LastShadowHookData* ShAllocateShadowHookData();

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
void ShFreeShadowHookData(_In_ LastShadowHookData* sh_data);

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
SharedShadowHookPatchData* ShAllocateSharedShaowHookData();

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
void ShFreeSharedShadowHookData(_In_ SharedShadowHookPatchData* shared_sh_data);

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C NTSTATUS ShEnableHooks();

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C NTSTATUS ShDisableHooks();

_IRQL_requires_min_(DISPATCH_LEVEL) NTSTATUS
ShEnablePageShadowing(_In_ EptData* ept_data,
    _In_ const SharedShadowHookPatchData* shared_sh_data);

_IRQL_requires_min_(DISPATCH_LEVEL) void ShVmCallDisablePageShadowing(
    _In_ EptData* ept_data, _In_ const SharedShadowHookPatchData* shared_sh_data);

_IRQL_requires_max_(PASSIVE_LEVEL)
bool ShInstallPatch(_In_ SharedShadowHookPatchData* shared_sh_data,
    _In_ void* address, _In_ ShadowPatchTarget *ShadowPatchTarget);

_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C
bool ShInstallHook(_In_ SharedShadowHookPatchData* shared_sh_data,
    _In_ void* address, _In_ ShadowHookTarget *ShadowHookTarget);

_IRQL_requires_min_(DISPATCH_LEVEL) bool ShHandleBreakpoint(
    _In_ LastShadowHookData* sh_data,
    _In_ const SharedShadowHookPatchData* shared_sh_data, _In_ void* guest_ip);

_IRQL_requires_min_(DISPATCH_LEVEL) void ShHandleMonitorTrapFlag(
    _In_ LastShadowHookData* sh_data,
    _In_ const SharedShadowHookPatchData* shared_sh_data, _In_ EptData* ept_data);

_IRQL_requires_min_(DISPATCH_LEVEL) void ShHandleEptViolation(
    _In_ LastShadowHookData* sh_data,
    _In_ const SharedShadowHookPatchData* shared_sh_data, _In_ EptData* ept_data,
    _In_ void* fault_va);

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

#endif  // DDIMON_SHADOW_HOOK_H_
