# libScePngEnc technical debt

### Functional

- [scePngEncEncode](../../../core/libs/prx/libScePngEnc/Export.cpp): native filter setup always includes the unfiltered candidate, including for partial masks. The host now permits it too; exact filter-selection ties, compressed bytes and partial-output behavior are not matched to the native encoder. Creation memory layout and encode validation still differ from the supplied module.
