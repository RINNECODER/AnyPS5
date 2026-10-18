# libSceM4aacEnc technical debt

### Not implemented

- `libSceM4aacEnc` (PS5 system library, sysmodule id `0x000000bc`) is not implemented in AnyPS5. Only its entry in the sysmodule table exists; there is no `core/libs/prx/libSceM4aacEnc` implementation and no host `.prx` is shipped. Titles that link it directly (observed with PPSA03332) fail to load with `libSceM4aacEnc.prx: cannot open shared object file`.

Known public exports (from the aerolib NID database):
- `sceM4aacEncClearContext`
- `sceM4aacEncClearContextEx`
- `sceM4aacEncCreateEncoder`
- `sceM4aacEncCreateEncoderEx`
- `sceM4aacEncDeleteEncoder`
- `sceM4aacEncDeleteEncoderEx`
- `sceM4aacEncEncode`
- `sceM4aacEncEncodeEx`
- `sceM4aacEncEncodeWithPriority`
- `sceM4aacEncEncodeWithPriorityEx`
- `sceM4aacEncFlush`
- `sceM4aacEncFlushEx`
- `sceM4aacEncFlushWithPriority`
- `sceM4aacEncFlushWithPriorityEx`
- `sceM4aacEncGetContext`
- `sceM4aacEncInitialize`
- `sceM4aacEncTerminate`

## Signatures (orbital db_types.json)

- `int32_t sceM4aacEncClearContext(int32_t m4aacEncHandle)`
- `int32_t sceM4aacEncCreateEncoder(int32_t channels, int32_t samplingRate, int32_t bitRate, int32_t inputFormat, int32_t outputFormat, int32_t *pInternalError)`
- `int32_t sceM4aacEncDeleteEncoder(int32_t m4aacEncHandle, int32_t *pInternalError)`
- `int32_t sceM4aacEncEncode(int32_t m4aacEncHandle, void *pInput, uint32_t inputByte, void *pOutput, uint32_t *pOutputByte, int32_t *pInternalError)`
- `int32_t sceM4aacEncFlush(int32_t m4aacEncHandle, void *pOutput, uint32_t *pOutputByte, int32_t *pInternalError)`
