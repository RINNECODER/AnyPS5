# libScePlayGoDialog technical debt

### Silent stubs

- [libScePlayGoDialog](../../../core/libs/prx/libScePlayGoDialog/Export.cpp) - the PlayGo dialog runs the common-dialog state machine without displaying UI
- [scePlayGoDialogOpen](../../../core/libs/prx/libScePlayGoDialog/Export.cpp) does not validate or copy the native nested parameters or query download progress. The host UI stub immediately reports completion; service-driven failures and transitions remain unimplemented.
