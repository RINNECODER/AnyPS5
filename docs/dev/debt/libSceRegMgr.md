# libSceRegMgr technical debt

### Unknown function info

- [sceRegMgrGetBin and sceRegMgrSetBin](../../../core/libs/prx/libSceRegMgr/Export.cpp) (libSceRegMgr) - names and NIDs are present in the [PS5 3.20 export inventory](https://github.com/DNNDHH/PS5-3.20_Libs/blob/b2a77c09cd71c01f34c790cfbe98c751a13262ad/libSceRegMgr.c), but no credible prototypes were found. They are declared without parameters solely to resolve known imports; this is not an ABI claim. The string and integer prototypes follow [SharpProspero's SystemSettings bindings](https://github.com/SvenGDK/SharpProspero/blob/5dfdb4b0b166e83dc9ae275001afb695f12c7174/src/SharpProspero/Platform/SystemSettings.cs)
