// Synthetic title for the engine launcher smoke (tools/rosetta/tests/launcher_smoke.py):
// returns 0 when anyps5_rosetta_run preloaded the --sce-module, stubbed an import no
// library exports and a library the build lacks, relinked the resource root's
// top-level module for sceKernelLoadStartModule and linked the resource root in as
// /app0. Any other value names the first failed check.
typedef unsigned long u64;
typedef unsigned int u32;

extern volatile u64 ModuleInitCount;
extern int sceKernelLoadStartModule(const char* path, u64 args, const void* argp, u32 flags, const void* option, int* result);
extern int sceKernelDlsym(int handle, const char* symbol, void** address);
extern int sceKernelOpen(const char* path, int flags, int mode);
extern long sceKernelRead(int fd, void* buffer, u64 size);
extern int sceKernelClose(int fd);
extern long Aps5SmokeUnimplemented(long value);   // libkernel, not exported by the x86_64 build
extern long Aps5SmokeMissingLibrary(long value);  // a library the build does not have

int GuestMain(int argc, char** argv) {
    if (argc < 1 || !argv[0]) return 2;
    if (ModuleInitCount != 1) return 3;
    if (Aps5SmokeUnimplemented(5) != 0) return 4;
    if (Aps5SmokeMissingLibrary(6) != 0) return 5;
    const unsigned char argument = 7;
    int started = 0;
    const int handle = sceKernelLoadStartModule("/app0/GuestDynamic.prx", 4, &argument, 0, 0, &started);
    if (handle <= 0) return 6;
    if (started != 407) return 7;
    u64 (*probe)(u64) = 0;
    if (sceKernelDlsym(handle, "DynamicProbe", (void**)&probe) != 0 || !probe) return 8;
    if (probe(0x900) != 0x3333) return 9;
    const int fd = sceKernelOpen("/app0/data/smoke.txt", 0, 0);
    if (fd < 0) return 10;
    char text[16];
    const long length = sceKernelRead(fd, text, sizeof text - 1);
    sceKernelClose(fd);
    if (length != 5 || text[0] != 's' || text[4] != 'e') return 11;
    return 0;
}
