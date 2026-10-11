#pragma once

#include "SceTypes.hpp"
#include "prx/libSceAgc/Misc/include/PacketInfo.hpp"
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
int APS5_VABI sceAgcInit_0090(std::uint32_t* state, std::uint32_t version);
int APS5_VABI sceAgcGetIsTrinityMode(bool* isTrinityMode);
std::uint32_t APS5_VABI sceAgcCbNopGetSize(std::uint32_t size_in_dwords);
std::uint32_t APS5_VABI sceAgcCbSetShRegisterRangeDirectGetSize(std::uint32_t num_values);
std::uint32_t APS5_VABI sceAgcCbSetUcRegisterRangeDirectGetSize(std::uint32_t numValues);
std::uint32_t APS5_VABI sceAgcCbSetShRegistersDirectGetSize(std::uint32_t numRegs);
std::uint32_t APS5_VABI sceAgcCbSetUcRegistersDirectGetSize(std::uint32_t numRegs);
std::uint32_t APS5_VABI sceAgcDcbSetShRegistersIndirectGetSize(std::uint32_t numRegs);
std::uint32_t APS5_VABI sceAgcDcbSetCxRegistersIndirectGetSize(std::uint32_t numRegs);
std::uint32_t APS5_VABI sceAgcDcbSetUcRegistersIndirectGetSize(std::uint32_t numRegs);
std::uint64_t APS5_VABI sceAgcDcbContextStateOpGetSize(std::uint32_t operation);
std::uint32_t APS5_VABI sceAgcDcbWriteDataGetSize(std::uint32_t num_dwords);
std::uint64_t APS5_VABI sceAgcDcbEventWriteGetSize(std::uint8_t eventType);
std::uint32_t APS5_VABI sceAgcDcbWaitOnAddressGetSize(std::uint32_t size);
std::uint64_t APS5_VABI sceAgcAcbWaitOnAddressGetSize(std::uint8_t size);
std::uint32_t APS5_VABI sceAgcGetPacketSize(std::uint32_t* packet);
int APS5_VABI sceAgcGetDataPacketPayloadAddress_0090(std::uint32_t** addr, std::uint32_t* cmd, int type);
int APS5_VABI sceAgcGetDataPacketPayloadRange(SceAgcMemoryRange* range, std::uint32_t* cmd, int type);
int APS5_VABI sceAgcSetRangePredication(std::uint32_t* start, const volatile std::uint32_t* end, std::uint32_t predication);
std::uint64_t APS5_VABI sceAgcAcbEventWriteGetSize(std::uint8_t eventType);
int APS5_VABI sceAgcSetPacketPredication(std::uint32_t* packet, std::uint32_t predication);
std::uint32_t* APS5_VABI sceAgcCbBranch(CommandBuffer* buf, std::uint8_t mode, std::uint8_t compareFunction, const volatile std::uint64_t* compareAddress, std::uint64_t mask, std::uint64_t reference, std::uint8_t cachePolicy1, const volatile std::uint32_t* buffer1, std::uint32_t sizeInDwords1, std::uint8_t cachePolicy2, const volatile std::uint32_t* buffer2, std::uint32_t sizeInDwords2);
std::uint32_t* APS5_VABI sceAgcCbCondWrite(CommandBuffer* buf, std::uint32_t compareFunction, std::uint32_t writeSpace, const volatile void* writeAddress, std::uint32_t writeData, const volatile void* pollAddress, std::uint32_t reference, std::uint32_t mask);
std::uint32_t* APS5_VABI sceAgcCbSetShRegistersDirect(CommandBuffer* buf, const volatile ShaderRegister* regs, std::uint32_t numRegs);
std::uint32_t* APS5_VABI sceAgcCbSetUcRegistersDirect(CommandBuffer* buf, const volatile ShaderRegister* regs, std::uint32_t numRegs);
std::uint32_t* APS5_VABI sceAgcDcbContextStateOp_0100(CommandBuffer* buf, std::uint32_t operation);
std::uint32_t* APS5_VABI sceAgcDcbJump(CommandBuffer* buf, std::uint8_t mode, std::uint8_t cache_policy, const std::uint32_t* target, std::uint32_t size_in_dwords);
std::uint32_t* APS5_VABI sceAgcDcbSetShRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, std::uint32_t numRegs);
std::uint32_t* APS5_VABI sceAgcDcbSetCxRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, std::uint32_t numRegs);
std::uint32_t* APS5_VABI sceAgcDcbSetUcRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, std::uint32_t numRegs);
std::uint32_t* APS5_VABI sceAgcDcbDispatchIndirect(CommandBuffer* buf, std::uint32_t data_offset_in_bytes, std::uint32_t flags);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndexAuto(CommandBuffer* buf, std::uint32_t indexCount, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndex(CommandBuffer* buf, std::uint32_t indexCount, const volatile void* indexAddress, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndexOffset(CommandBuffer* buf, std::uint32_t indexOffset, std::uint32_t indexCount, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndirect(CommandBuffer* buf, std::uint32_t dataOffsetInBytes, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndirectMulti(CommandBuffer* buf, std::uint32_t dataOffsetInBytes, std::uint32_t countIndirect, std::uint32_t maxCountOrCount, const volatile void* countAddress, std::uint32_t strideInBytes, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndexIndirectMulti(CommandBuffer* buf, std::uint32_t data_offset_in_bytes, std::uint32_t count_indirect, std::uint32_t max_count_or_count, const volatile void* count_addr, std::uint32_t stride_in_bytes, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndexIndirect(CommandBuffer* buf, std::uint32_t data_offset_in_bytes, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbDrawIndexMultiInstanced(CommandBuffer* buf, std::uint32_t index_count, const volatile void* index_addr, const volatile void* object_ids, std::uint32_t instance_count, std::uint64_t modifier);
std::uint32_t* APS5_VABI sceAgcDcbEventWrite(CommandBuffer* buf, std::uint8_t eventType, const volatile void* address);
std::uint32_t* APS5_VABI sceAgcDcbGetLodStats(CommandBuffer* buf, std::uint8_t cachePolicy, const volatile void* buffer, std::uint32_t bufferSizeInBytes, std::uint32_t resetCount, std::uint8_t forceReset, std::uint8_t reportAndReset, std::uint32_t reportingIntervalIn100kClocks);
std::uint32_t* APS5_VABI sceAgcDcbSetBaseIndirectArgs(CommandBuffer* buf, std::uint32_t shader_type, const volatile void* indirect_base_addr);
std::uint32_t* APS5_VABI sceAgcDcbSetIndexSize_0600(CommandBuffer* buf, std::uint8_t indexSize, std::uint8_t cachePolicy, std::uint8_t perInstanceObjectId);
std::uint32_t* APS5_VABI sceAgcDcbSetIndexBuffer(CommandBuffer* buf, std::uint64_t indexAddress);
std::uint32_t* APS5_VABI sceAgcDcbSetIndexCount(CommandBuffer* buf, std::uint32_t indexCount);
std::uint32_t* APS5_VABI sceAgcDcbSetNumInstances(CommandBuffer* buf, std::uint32_t numInstances);
std::uint32_t* APS5_VABI sceAgcDcbStallCommandBufferParser(CommandBuffer* buf);
std::uint32_t* APS5_VABI sceAgcDcbRewind(CommandBuffer* buf, std::uint32_t initial_state);
std::uint32_t* APS5_VABI sceAgcDcbPrimeUtcl2(CommandBuffer* buf, const volatile void* address, std::uint32_t size_in_bytes);
std::uint32_t* APS5_VABI sceAgcDcbResetQueue(CommandBuffer* buf, std::uint32_t op, std::uint32_t state);
std::uint32_t* APS5_VABI sceAgcDcbSetPredication(CommandBuffer* buf, std::uint8_t condition, std::uint8_t op, std::uint8_t wait_op, const volatile void* address, std::uint32_t count_in_dwords);
std::uint32_t* APS5_VABI sceAgcDcbSetMarker(CommandBuffer* buf, const char* str, std::uint32_t color);
std::uint32_t* APS5_VABI sceAgcDcbPushMarker(CommandBuffer* buf, const char* str, std::uint32_t color);
std::uint32_t* APS5_VABI sceAgcDcbPopMarker(CommandBuffer* buf);
std::uint32_t* APS5_VABI sceAgcDcbMemSemaphore(CommandBuffer* buf, std::uint8_t action, std::uint8_t clientCode, const volatile void* address);
std::uint32_t* APS5_VABI sceAgcAcbResetQueue(CommandBuffer* buf, std::uint32_t op, std::uint32_t value);
std::uint32_t* APS5_VABI sceAgcAcbMemSemaphore(CommandBuffer* buf, std::uint8_t action, std::uint8_t clientCode, const volatile void* address);
std::uint32_t* APS5_VABI sceAgcAcbDispatchIndirect(CommandBuffer* buf, const volatile void* indirect_args, std::uint32_t modifier);
std::uint32_t* APS5_VABI sceAgcAcbSetMarker(CommandBuffer* buf, const char* str, std::uint32_t color);
std::uint32_t* APS5_VABI sceAgcAcbRewind(CommandBuffer* buf, std::uint32_t initialState);
std::uint32_t* APS5_VABI sceAgcAcbPrimeUtcl2(CommandBuffer* buf, const volatile void* address, std::uint32_t sizeInBytes);
std::uint32_t* APS5_VABI sceAgcAcbPushMarker(CommandBuffer* buf, const char* str, std::uint32_t color);
std::uint32_t* APS5_VABI sceAgcAcbJump(CommandBuffer* buf, std::uint8_t cachePolicy, const std::uint32_t* target, std::uint32_t sizeInDwords);
std::uint32_t* APS5_VABI sceAgcAcbEventWrite(CommandBuffer* buf, std::uint8_t eventType, const volatile void* address);
std::uint32_t* APS5_VABI sceAgcAcbPopMarker(CommandBuffer* buf);
std::uint32_t* APS5_VABI sceAgcSetNop(CommandBuffer* buf, std::uint32_t sizeDw);
std::uint32_t APS5_VABI sceAgcCbDispatchGetSize();
std::uint32_t APS5_VABI sceAgcCbBranchGetSize();
std::uint32_t APS5_VABI sceAgcCbCondWriteGetSize();
std::uint32_t APS5_VABI sceAgcCbQueueEndOfPipeActionGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetShRegisterDirectGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetCxRegisterDirectGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetUcRegisterDirectGetSize();
std::uint32_t APS5_VABI sceAgcDcbAcquireMemGetSize();
std::uint32_t APS5_VABI sceAgcDcbAtomicGdsGetSize();
std::uint32_t APS5_VABI sceAgcDcbAtomicMemGetSize();
std::uint32_t APS5_VABI sceAgcDcbCondExecGetSize();
std::uint32_t APS5_VABI sceAgcDcbJumpGetSize();
std::uint64_t APS5_VABI sceAgcDcbCopyDataGetSize();
std::uint32_t APS5_VABI sceAgcDcbDmaDataGetSize();
std::uint32_t APS5_VABI sceAgcDcbQueueEndOfShaderActionGetSize();
std::uint32_t APS5_VABI sceAgcDcbDispatchIndirectGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndexAutoGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndexGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndexOffsetGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndirectGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndirectMultiGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndexIndirectMultiGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndexIndirectGetSize();
std::uint32_t APS5_VABI sceAgcDcbDrawIndexMultiInstancedGetSize();
std::uint32_t APS5_VABI sceAgcDcbGetLodStatsGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetBaseDispatchIndirectArgsGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetBaseDrawIndirectArgsGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetIndexSizeGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetIndexBufferGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetIndexCountGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetNumInstancesGetSize();
std::uint32_t APS5_VABI sceAgcDcbStallCommandBufferParserGetSize();
std::uint32_t APS5_VABI sceAgcDcbRewindGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetZPassPredicationEnableGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetBoolPredicationEnableGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetPredicationDisableGetSize();
std::uint32_t APS5_VABI sceAgcDcbPrimeUtcl2GetSize();
std::uint32_t APS5_VABI sceAgcAcbQueueEndOfShaderActionGetSize();
std::uint64_t APS5_VABI sceAgcAcbCopyDataGetSize();
std::uint32_t APS5_VABI sceAgcAcbAtomicMemGetSize();
std::uint32_t APS5_VABI sceAgcAcbDispatchIndirectGetSize();
std::uint32_t APS5_VABI sceAgcAcbRewindGetSize();
std::uint32_t APS5_VABI sceAgcAcbCondExecGetSize();
std::uint32_t APS5_VABI sceAgcAcbPrimeUtcl2GetSize();
std::uint32_t APS5_VABI sceAgcAcbDmaDataGetSize();
std::uint32_t APS5_VABI sceAgcAcbAtomicGdsGetSize();
std::uint32_t APS5_VABI sceAgcAcbJumpGetSize();
std::uint32_t APS5_VABI sceAgcAcbAcquireMemGetSize();
int APS5_VABI sceAgcSetCxRegIndirectPatchSetNumRegisters(std::uint32_t* cmd, std::uint32_t numRegs);
int APS5_VABI sceAgcWriteDataPatchSetAddressOrOffset(std::uint32_t* cmd, std::uint64_t address);
int APS5_VABI sceAgcSetUcRegIndirectPatchSetAddress(std::uint32_t* cmd, const volatile ShaderRegister* regs);
int APS5_VABI sceAgcWriteDataPatchSetCachePolicy(std::uint32_t* cmd, std::uint8_t cachePolicy);
int APS5_VABI sceAgcWaitRegMemPatchCompareFunction(std::uint32_t* cmd, std::uint8_t compareFunction);
int APS5_VABI sceAgcQueueEndOfPipeActionPatchGcrCntl(std::uint32_t* cmd, std::uint16_t gcrControl);
int APS5_VABI sceAgcQueueEndOfPipeActionPatchAddress(std::uint32_t* cmd, const volatile Label* address);
int APS5_VABI sceAgcSetCxRegIndirectPatchSetAddress(std::uint32_t* cmd, const volatile ShaderRegister* regs);
int APS5_VABI sceAgcAsyncWriteDataPatchSetCachePolicy(std::uint32_t* cmd, std::uint8_t cachePolicy);
int APS5_VABI sceAgcSetUcRegIndirectPatchAddRegisters(std::uint32_t* cmd, std::uint32_t numRegs);
int APS5_VABI sceAgcBranchPatchSetCompareAddress(std::uint32_t* cmd, const volatile std::uint64_t* compareAddress);
int APS5_VABI sceAgcJumpPatchSetTarget_0300(std::uint32_t* cmd, std::uint64_t cache_policy, const volatile std::uint32_t* target, std::uint32_t size_in_dwords);
int APS5_VABI sceAgcQueueEndOfPipeActionPatchData(std::uint32_t* cmd, std::uint64_t data);
int APS5_VABI sceAgcAsyncWriteDataPatchSetDst(std::uint32_t* cmd, std::uint8_t dst);
int APS5_VABI sceAgcWaitRegMemPatchAddress(std::uint32_t* cmd, const volatile void* address);
int APS5_VABI sceAgcWriteDataPatchSetDst(std::uint32_t* cmd, std::uint8_t dst);
int APS5_VABI sceAgcSetCxRegIndirectPatchAddRegisters(std::uint32_t* cmd, std::uint32_t numRegs);
int APS5_VABI sceAgcAsyncCondExecPatchSetEnd(std::uint32_t* cmd, const volatile std::uint32_t* buffer);
int APS5_VABI sceAgcSetUcRegIndirectPatchSetNumRegisters(std::uint32_t* cmd, std::uint32_t numRegs);
int APS5_VABI sceAgcAsyncCondExecPatchSetCommandAddress(std::uint32_t* cmd, const volatile std::uint32_t* command);
int APS5_VABI sceAgcCondExecPatchSetCommandAddress(std::uint32_t* cmd, const volatile std::uint32_t* command);
int APS5_VABI sceAgcQueueEndOfPipeActionPatchType(std::uint32_t* cmd, std::uint8_t action);
int APS5_VABI sceAgcCondExecPatchSetEnd(std::uint32_t* cmd, const volatile std::uint32_t* buffer);
int APS5_VABI sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate(std::uint32_t* cmd, std::uint64_t address);
int APS5_VABI sceAgcAsyncRewindPatchSetRewindState(std::uint32_t* cmd, std::uint8_t state);
int APS5_VABI sceAgcAsyncWriteDataPatchSetAddressOrOffset(std::uint32_t* cmd, std::uint64_t address);
int APS5_VABI sceAgcRewindPatchSetRewindState(std::uint32_t* cmd, std::uint8_t state);
int APS5_VABI sceAgcSetShRegIndirectPatchAddRegisters(std::uint32_t* cmd, std::uint32_t numRegs);
int APS5_VABI sceAgcSetShRegIndirectPatchSetAddress(std::uint32_t* cmd, const volatile ShaderRegister* regs);
int APS5_VABI sceAgcWaitRegMemPatchReference(std::uint32_t* cmd, std::uint64_t reference);
int APS5_VABI sceAgcDmaDataPatchSetDstAddressOrOffset(std::uint32_t* cmd, std::uint64_t address);
int APS5_VABI sceAgcSetShRegIndirectPatchSetNumRegisters(std::uint32_t* cmd, std::uint32_t numRegs);
}
