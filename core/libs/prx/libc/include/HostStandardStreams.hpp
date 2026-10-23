#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTSTANDARDSTREAMS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTSTANDARDSTREAMS_HPP

#include <stdio.h>

#ifdef _WIN32
#undef stdout
#undef stderr
#ifdef __cplusplus
extern "C" {
#endif
FILE* HostStandardOutputFile_nid_no_patch(void);
FILE* HostStandardErrorFile_nid_no_patch(void);
#ifdef __cplusplus
}
#endif
#define stdout HostStandardOutputFile_nid_no_patch()
#define stderr HostStandardErrorFile_nid_no_patch()
#endif

#endif
