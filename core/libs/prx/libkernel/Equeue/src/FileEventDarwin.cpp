// EVFILT_VNODE and sceKernelAddFileEvent need inotify (Linux) or ReadDirectoryChangesW (Windows).
// The macOS build reports them as not implemented the way the other unsupported kqueue filters are.
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libc/include/General.hpp"

void RequireVnodeNotes(std::uint32_t) {
    NotImplemented_nid_no_patch("EVFILT_VNODE on macOS");
}

void SetVnodeNotes(KernelEqueueEvent&, std::uint32_t) {
    NotImplemented_nid_no_patch("EVFILT_VNODE on macOS");
}

int AddVnodeEvent(const KernelEqueueRef&, int, std::uint32_t, std::uint16_t, void*) {
    NotImplemented_nid_no_patch("EVFILT_VNODE on macOS");
    return 0;
}

extern "C" {

int APS5_VABI sceKernelAddFileEvent(KernelEqueue, int, int, void*) {
    NotImplemented_nid_no_patch("sceKernelAddFileEvent on macOS");
    return 0;
}

int APS5_VABI sceKernelDeleteFileEvent(KernelEqueue, int) {
    NotImplemented_nid_no_patch("sceKernelDeleteFileEvent on macOS");
    return 0;
}

}
