#pragma once

// Stateless DXGI_FORMAT predicates, shared by every hooks_*.cpp.
//
// These are the only symbols promoted out of hooks.cpp's anonymous
// namespace by the split. They are `inline`, not `static`: FormatName
// carries a function-local thread_local ring buffer, and C++17 guarantees
// exactly one copy of that across translation units for an inline function.
// With `static` each TU would get its own, which is the kind of difference
// that shows up as an impossible log line months later.

#include <dxgiformat.h>

#include <cstdio>

namespace segcap {

// Never returns "other".
//
// It used to, and that hid evidence for the third time in this project. On
// inZOI, 800 of the observed render targets printed as "other" -- a single
// bucket covering every format this switch did not happen to list. Nanite's
// visibility buffer is an R32G32_UINT render target, so the one question worth
// asking of a UE5 census ("is the Nanite route available here?") was
// unanswerable from a log that had already thrown the answer away.
//
// Unknown formats now print their numeric DXGI_FORMAT, which is always enough
// to look up. A name I did not anticipate is not the same as no name.
inline const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        // Integer formats matter specifically: UE writes per-object and
        // per-cluster ids into UINT targets, so these are the candidates for an
        // id source that needs no engine mutation at all.
        case DXGI_FORMAT_R32G32_UINT: return "R32G32_UINT";
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R16G16_UINT: return "R16G16_UINT";
        case DXGI_FORMAT_R16_UINT: return "R16_UINT";
        case DXGI_FORMAT_R8_UINT: return "R8_UINT";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        default: break;
    }
    // Thread-safe enough for logging: a small ring of buffers so several
    // formats can appear in one log line without clobbering each other.
    static thread_local char buf[4][24];
    static thread_local int which = 0;
    which = (which + 1) & 3;
    _snprintf_s(buf[which], sizeof(buf[which]), _TRUNCATE, "DXGI_FMT_%d",
                static_cast<int>(f));
    return buf[which];
}
// Integer-typed render targets. UE uses these for identity data -- object ids,
// primitive ids, and Nanite's packed visible-cluster index.
inline bool IsIntegerFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8_UINT:
            return true;
        default:
            return false;
    }
}
inline bool HasStencilPlane(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

}  // namespace segcap
