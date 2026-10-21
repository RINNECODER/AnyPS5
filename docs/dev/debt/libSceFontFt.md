# libSceFontFt technical debt

### Functional

- [Font format support](../../../core/libs/prx/libSceFontFt/Export.cpp): format queries now take the font library, validate its magic and lock it while checking its actual FreeType modules. The host registers its default modules at creation rather than enabling formats individually, so format-selection state and guest allocator failures during format registration are not reproduced. Unsupported foreign drivers throw.
- [TrueType GX support](../../../core/libs/prx/libSceFontFt/Export.cpp): the GX query checks the bundled FreeType TrueType module, whose configuration enables GX variation support. The console's response for a FreeType build without GX variation support has not been measured.

### Unknown function info

- [sceFontFtSupportBdf, sceFontFtSupportCid, sceFontFtSupportOpenType, sceFontFtSupportOpenTypeOtf, sceFontFtSupportOpenTypeTtf, sceFontFtSupportPcf, sceFontFtSupportPfr, sceFontFtSupportTrueType, sceFontFtSupportType1, sceFontFtSupportType42 and sceFontFtSupportWinFonts](../../../core/libs/prx/libSceFontFt/Export.cpp) (libSceFontFt) are declared without parameters, signatures unknown; [shadPS4](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/font/fontft.cpp), another emulator, declares them the same way and only logs. They are assumed to enable one font format in the FontFt library, which here always registers FreeType's default modules, so each call returns 0 when the host FreeType has the module (`bdf`, `t1cid`, `truetype`, `cff`, `pcf`, `pfr`, `type1`, `type42`, `winfonts`; OpenType needs `cff` and `truetype`) and throws when it does not. The mapping of Sony's format names to FreeType modules, the return value and the error codes of the console are unverified, and no title is known to call them.
- [Alias and glyph selection exports](../../../core/libs/prx/libSceFontFt/Export.cpp): alias initialization/termination take a handle pointer; alias registration takes a string and font ID or path. Glyph selection takes a 32-bit selector and returns a table pointer whose layout remains unresolved. Their protected implementations are unchanged.
