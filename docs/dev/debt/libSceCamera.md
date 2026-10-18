# libSceCamera technical debt

### Not implemented

- `libSceCamera` (PS5 system library, sysmodule id `0x8000001a`) is not implemented in AnyPS5. Only its entry in the sysmodule table exists; there is no `core/libs/prx/libSceCamera` implementation and no host `.prx` is shipped. Titles that link it directly (observed with PPSA03332) fail to load with `libSceCamera.prx: cannot open shared object file`.

No public signatures found in the orbital NID database or online references (orbital only has PS4 `sceCamera*`, not PS5 `sceCamera2*`); needs disassembly or hardware measurement.
Known public exports (from the aerolib NID database):
- `sceCamera2Close`
- `sceCamera2Finalize`
- `sceCamera2GetAttribute`
- `sceCamera2GetAutoExposureGain`
- `sceCamera2GetAutoWhiteBalance`
- `sceCamera2GetCalibrationData`
- `sceCamera2GetConfig`
- `sceCamera2GetContrast`
- `sceCamera2GetExposureGain`
- `sceCamera2GetFieldOfView`
- `sceCamera2GetFrameData`
- `sceCamera2GetGamma`
- `sceCamera2GetHue`
- `sceCamera2GetSaturation`
- `sceCamera2GetSharpness`
- `sceCamera2GetWhiteBalance`
- `sceCamera2Initialize`
- `sceCamera2IsAttached`
- `sceCamera2IsValidFrameData`
- `sceCamera2Open`
- `sceCamera2SetAttribute`
- `sceCamera2SetAutoExposureGain`
- `sceCamera2SetAutoWhiteBalance`
- `sceCamera2SetConfig`
- `sceCamera2SetContrast`
- `sceCamera2SetExposureGain`
- `sceCamera2SetGamma`
- `sceCamera2SetHue`
- `sceCamera2SetSaturation`
- `sceCamera2SetSharpness`
- `sceCamera2SetVideoSync`
- `sceCamera2SetWhiteBalance`
- `sceCamera2Start`
- `sceCamera2Stop`
