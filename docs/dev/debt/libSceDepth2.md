# libSceDepth2 technical debt

### Not implemented

- `libSceDepth2` (PS5 system library, sysmodule id `0xunknown`) is not implemented in AnyPS5. Only its entry in the sysmodule table exists; there is no `core/libs/prx/libSceDepth2` implementation and no host `.prx` is shipped. Titles that link it directly (observed with PPSA03332) fail to load with `libSceDepth2.prx: cannot open shared object file`.

No public signatures found in the orbital NID database or online references; needs disassembly or hardware measurement.
Known public exports (from the aerolib NID database):
- `sceDepth2GetImage`
- `sceDepth2GetRectifiedImage`
- `sceDepth2Initialize`
- `sceDepth2InitializeVr`
- `sceDepth2QueryMemory`
- `sceDepth2SaveInternalBuffers`
- `sceDepth2SetCameraFactoryCalibrationData`
- `sceDepth2SetCommand`
- `sceDepth2SetRoi`
- `sceDepth2Submit`
- `sceDepth2Terminate`
- `sceDepth2WaitAndExecutePostProcess`
