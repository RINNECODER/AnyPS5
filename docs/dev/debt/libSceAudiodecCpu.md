# libSceAudiodecCpu technical debt

### Not implemented

- `libSceAudiodecCpu` (PS5 system library, sysmodule id `0x000000bd`) is not implemented in AnyPS5. Only its entry in the sysmodule table exists; there is no `core/libs/prx/libSceAudiodecCpu` implementation and no host `.prx` is shipped. Titles that link it directly (observed with PPSA03332) fail to load with `libSceAudiodecCpu.prx: cannot open shared object file`.

Known public exports (from the aerolib NID database):
- `sceAudiodecCpuClearContext`
- `sceAudiodecCpuDecode`
- `sceAudiodecCpuInitDecoder`
- `sceAudiodecCpuInternalClearContext`
- `sceAudiodecCpuInternalDecode`
- `sceAudiodecCpuInternalInitDecoder`
- `sceAudiodecCpuInternalQueryMemSize`
- `sceAudiodecCpuQueryMemSize`

## Signatures (orbital db_types.json)

- `int32_t sceAudiodecCpuClearContext(SceAudiodecCpuResource *pRes, uint32_t uiCodecType)`
- `int32_t sceAudiodecCpuDecode(SceAudiodecCpuCtrl *pCtrl, SceAudiodecCpuResource *pRes, uint32_t uiCodecType)`
- `int32_t sceAudiodecCpuInitDecoder(SceAudiodecCpuCtrl *pCtrl, SceAudiodecCpuResource *pRes, uint32_t uiCodecType)`
- `int32_t sceAudiodecCpuQueryMemSize(SceAudiodecCpuCtrl *pCtrl, SceAudiodecCpuResource *pRes, uint32_t uiCodecType)`
