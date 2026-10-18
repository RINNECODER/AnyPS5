# libSceAudiodecCpuHevag technical debt

### Not implemented

- `libSceAudiodecCpuHevag` (PS5 system library, sysmodule id `0x000000e1`) is not implemented in AnyPS5. Only its entry in the sysmodule table exists; there is no `core/libs/prx/libSceAudiodecCpuHevag` implementation and no host `.prx` is shipped. Titles that link it directly (observed with PPSA03332) fail to load with `libSceAudiodecCpuHevag.prx: cannot open shared object file`.

Note: aerolib contains only internal/mangled symbols for this library, so the public API list is unknown.
