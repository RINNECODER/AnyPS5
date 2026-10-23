# libSceBgft technical debt

### Unknown function info

- `sceBgftServiceDownloadGetProgress`, `sceBgftServiceIntDownloadGetProgress`, `sceBgftServiceIntDownloadGetGameAndGameAcProgress` and `sceBgftServiceIntDownloadGetPlayGoProgress`: signatures and the shared 88-byte output are confirmed. The library forwards the output through IPC without accessing individual fields; their types remain unresolved.
- `sceBgftServiceIntDownloadRegisterTaskByStorageEx`: confirmed parameter pointer and 32-bit task ID output. The input contains 32-bit scalars at offsets 0, 4, 56 and 104, strings at offsets 8, 16, 32, 40, 48, 64, 72, 80 and 88, and a 64-bit scalar at offset 96. Scalar meanings and the field at offset 24 remain unresolved; the parameter type is forward-declared.
- `sceBgftServiceIntDownloadGetPatchProgress` and `sceBgftServiceIntDownloadGetTaskInfo`: neither export nor a caller was found in the supplied decompiled library corpus. Their signatures remain unresolved.
