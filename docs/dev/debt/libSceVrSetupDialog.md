# libSceVrSetupDialog technical debt

### Silent stubs

- [libSceVrSetupDialog](../../../core/libs/prx/libSceVrSetupDialog/Export.cpp) - the VR setup dialog runs the common-dialog state machine but shows nothing and reports the user as having cancelled, as no headset is supported (see libSceHmd2). The signatures are assumed from the other common dialogs; PPSA12544 imports it
- [libSceVrSetupDialog](../../../core/libs/prx/libSceVrSetupDialog/Export.cpp) - the VR setup dialog runs the common-dialog state machine but shows nothing and reports the user as having cancelled, as no headset is supported (see libSceHmd2). Initialization does not check the native process eligibility record, and the 46-byte setup payload remains opaque. Open validates the recovered 56-byte parameter header. GetResult is absent from the supplied module, so its signature and cancellation result remain inferred.
