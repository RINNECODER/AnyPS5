# libScePngDec technical debt

### Functional

- [libScePngDec](../../../core/libs/prx/libScePngDec/Export.cpp): the supplied module confirms the work-memory size formula, 16-byte handle alignment, the first 24 bytes of its context, and Create/QueryMemorySize validation order. The host now exposes these values but still allocates decoder working storage on the host instead of using the guest work region.
- [scePngDecDecode](../../../core/libs/prx/libScePngDec/Export.cpp): 16-bit output and the original's accepted pixel format 132 remain unsupported and throw. Malformed-image diagnostics and decoder scratch-memory behavior are not fully matched to the supplied module.

### Unknown function info

- [libScePngDec](../../../core/libs/prx/libScePngDec/Export.cpp) (libScePngDec) - PNG image decoding interfaces; scePngDecDecodeWithInputControl throws until stream input control is implemented
