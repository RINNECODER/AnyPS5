# libSceSulpha technical debt

### Not implemented

- `libSceSulpha` (PS5 system library, sysmodule id `0x0000009f`) is not implemented in AnyPS5. Only its entry in the sysmodule table exists; there is no `core/libs/prx/libSceSulpha` implementation and no host `.prx` is shipped. Titles that link it directly (observed with PPSA03332) fail to load with `libSceSulpha.prx: cannot open shared object file`.

No public signatures found in the orbital NID database or online references; needs disassembly or hardware measurement.
Known public exports (from the aerolib NID database):
- `sceSulphaGetClientInfo`
- `sceSulphaGetConnectedClientCount`
- `sceSulphaGetDefaultConfig`
- `sceSulphaGetNeededMemory`
- `sceSulphaInit`
- `sceSulphaMessage`
- `sceSulphaSetBookmark`
- `sceSulphaShutdown`
