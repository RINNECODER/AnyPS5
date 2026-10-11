#pragma once

#include "SceTypes.hpp"
#include <cstdint>

// Upstream libSceAgc command builders (core/libs/prx/libSceAgc) linked into the
// CPU AGC bridge. Each encodes into a host CommandBuffer; address arguments are
// encoded as numbers and never dereferenced, except RegisterRange values.
extern "C" {
std::uint32_t* APS5_VABI sceAgcDcbAcquireMem(CommandBuffer* buf, std::uint8_t engine, std::uint32_t cbDbOp,
    std::uint32_t gcrControl, const volatile void* base, std::uint64_t sizeBytes, std::uint32_t pollCycles);
std::uint32_t* APS5_VABI sceAgcAcbAcquireMem(CommandBuffer* buf, std::uint32_t gcrControl, const volatile void* base,
    std::uint64_t sizeBytes, std::uint32_t pollCycles);
std::uint32_t* APS5_VABI sceAgcCbReleaseMem(CommandBuffer* buf, std::uint8_t action, std::uint16_t gcrControl,
    std::uint8_t dst, std::uint8_t cachePolicy, const volatile Label* address, std::uint8_t dataSelect, std::uint64_t data,
    std::uint16_t gdsOffset, std::uint16_t gdsSize, std::uint8_t interrupt, std::uint32_t interruptContextId);
std::uint32_t* APS5_VABI sceAgcDcbWaitRegMem(CommandBuffer* buf, std::uint8_t size, std::uint8_t compareFunction,
    std::uint8_t operation, std::uint8_t cachePolicy, const volatile void* address, std::uint64_t reference,
    std::uint64_t mask, std::uint32_t pollCycles);
std::uint32_t* APS5_VABI sceAgcAcbWaitRegMem(CommandBuffer* buf, std::uint8_t size, std::uint8_t compareFunction,
    std::uint8_t cachePolicy, const volatile void* address, std::uint64_t reference, std::uint64_t mask,
    std::uint32_t pollCycles);
std::uint32_t* APS5_VABI sceAgcDcbCopyData(CommandBuffer* buf, std::uint8_t dst, std::uint8_t dstCachePolicy,
    std::uint64_t dstAddress, std::uint8_t src, std::uint8_t srcCachePolicy, std::uint64_t srcAddressOrImmediate,
    std::uint8_t itemSize, std::uint8_t writeConfirm);
std::uint32_t* APS5_VABI sceAgcAcbCopyData(CommandBuffer* buf, std::uint8_t dst, std::uint8_t dstCachePolicy,
    std::uint64_t dstAddress, std::uint8_t src, std::uint8_t srcCachePolicy, std::uint64_t srcAddressOrImmediate,
    std::uint8_t itemSize, std::uint8_t writeConfirm);
std::uint32_t* APS5_VABI sceAgcDcbDmaData(CommandBuffer* buf, std::uint8_t engine, std::uint8_t dst,
    std::uint8_t dstCachePolicy, std::uint64_t dstAddress, std::uint8_t src, std::uint8_t srcCachePolicy,
    std::uint64_t srcAddress, std::uint32_t numBytes, std::uint8_t waitForPrevious, std::uint8_t writeConfirm,
    std::uint8_t blockEngine);
std::uint32_t* APS5_VABI sceAgcAcbDmaData(CommandBuffer* buf, std::uint8_t dst, std::uint8_t dstCachePolicy,
    std::uint64_t dstAddress, std::uint8_t src, std::uint8_t srcCachePolicy, std::uint64_t srcAddress,
    std::uint32_t numBytes, std::uint8_t waitForPrevious, std::uint8_t writeConfirm);
std::uint32_t* APS5_VABI sceAgcDcbAtomicMem(CommandBuffer* buf, std::uint8_t atomicOp, std::uint8_t command,
    std::uint8_t cachePolicy, const volatile void* address, std::uint64_t srcData, std::uint64_t compareData,
    std::uint16_t loopInterval);
std::uint32_t* APS5_VABI sceAgcAcbAtomicMem(CommandBuffer* buf, std::uint8_t atomicOp, std::uint8_t command,
    std::uint8_t cachePolicy, const volatile void* address, std::uint64_t srcData, std::uint64_t compareData,
    std::uint16_t loopInterval);
std::uint32_t* APS5_VABI sceAgcDcbCondExec(CommandBuffer* buf, const volatile std::uint32_t* address, std::uint32_t numDwords);
std::uint32_t* APS5_VABI sceAgcAcbCondExec(CommandBuffer* buf, const volatile std::uint32_t* address, std::uint32_t numDwords);
std::uint32_t* APS5_VABI sceAgcCbSetShRegisterRangeDirect(CommandBuffer* buf, std::uint32_t offset,
    const std::uint32_t* values, std::uint32_t numValues);
std::uint32_t* APS5_VABI sceAgcCbSetUcRegisterRangeDirect(CommandBuffer* buf, std::uint32_t offset,
    const std::uint32_t* values, std::uint32_t numValues);
}
