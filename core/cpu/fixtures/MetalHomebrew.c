typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

enum { Width = 64, Height = 32, PixelBytes = Width * Height * 4 };
#define VertexAddress 0x100000ul
#define ColorAllocation 0x20f000ul
#define ColorAddress (ColorAllocation + 4096)
#define VertexCodeAddress 0x500000ul
#define PixelCodeAddress 0x600000ul
#define VertexHeaderAddress 0x700000ul
#define PixelHeaderAddress 0x710000ul
#define CommandAddress 0xa00000ul
#define PacketAddress 0xb00000ul
#define LabelAddress (PacketAddress + 512)
#define DisplayAddress (PacketAddress + 1024)

struct ShaderHeader {
    u32 file_header, version;
    u64 user_data, code, cx_registers, sh_registers, specials, input_semantics, output_semantics;
    u32 header_size, shader_size, embedded_constant_buffer_size_dqw, target, num_input_semantics;
    u16 scratch_size_dw_per_thread, num_output_semantics, special_sizes_bytes;
    u8 type, num_cx_registers, num_sh_registers;
};
struct Packet { u64 addr; u32 dw_num; u8 flags, reserved[3]; };
struct DisplayBuffer {
    u64 address, pixelFormat;
    u32 width, height, tilingMode, pitchInPixel;
    u64 dccAddress, dccClearColor;
};
_Static_assert(sizeof(struct ShaderHeader) == 96, "Shader guest ABI");
_Static_assert(sizeof(struct Packet) == 16, "Packet guest ABI");
_Static_assert(sizeof(struct DisplayBuffer) == 48, "Display guest ABI");

static u32 words;
static void reg(u32 opcode, u32 offset, const u32* data, u32 count) {
    volatile u32* commands = (volatile u32*)CommandAddress;
    commands[words++] = 0xc0000000u | (count << 16) | (opcode << 8);
    commands[words++] = offset;
    for (u32 i = 0; i < count; ++i) commands[words++] = data[i];
}
static void one(u32 opcode, u32 offset, u32 value) { reg(opcode, offset, &value, 1); }

static void shader(u64 address, u64 code, u32 bytes, u8 type) {
    volatile u8* storage = (volatile u8*)address;
    for (u32 i = 0; i < 152; ++i) storage[i] = 0;
    volatile struct ShaderHeader* header = (volatile struct ShaderHeader*)address;
    header->file_header = 0x34333231;
    header->version = 0x18;
    header->user_data = address + sizeof(struct ShaderHeader);
    header->code = code;
    header->header_size = 152;
    header->shader_size = bytes;
    header->type = type;
}

int GuestMain(u64 argc, char** argv) {
    (void)argc; (void)argv;
    volatile float* vertices = (volatile float*)VertexAddress;
    const float triangle[12] = {-1, -1, .5f, 1, 3, -1, .5f, 1, -1, 3, .5f, 1};
    for (u32 i = 0; i < 12; ++i) vertices[i] = triangle[i];
    volatile u8* pixels = (volatile u8*)ColorAllocation;
    for (u32 i = 0; i < 0x4000; ++i) pixels[i] = 0x7b;
    for (u32 i = 0; i < PixelBytes; ++i) pixels[4096 + i] = 0x40;
    const u32 vs[6] = {0xe0382000, 0x80020005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000};
    const u32 ps[4] = {0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000};
    for (u32 i = 0; i < 6; ++i) ((volatile u32*)VertexCodeAddress)[i] = vs[i];
    for (u32 i = 0; i < 4; ++i) ((volatile u32*)PixelCodeAddress)[i] = ps[i];
    shader(VertexHeaderAddress, VertexCodeAddress, sizeof(vs), 2);
    shader(PixelHeaderAddress, PixelCodeAddress, sizeof(ps), 1);
    one(0x79, 0x242, 4);
    const u32 context[][2] = {
        {0x2d5, 0x00402000}, {0x1b6, 0x8000}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240}, {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028}, {0x31b, 0}, {0x31d, 0},
        {0x3b0, ((Width - 1) << 14) | (Height - 1)}, {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, (Height << 16) | Width}, {0x81, 0x80000000}, {0x82, (Height << 16) | Width},
        {0x90, 0x80000000}, {0x91, (Height << 16) | Width}, {0x94, 0x80000000}, {0x95, (Height << 16) | Width},
        {0x318, ColorAddress >> 8}, {0x390, 0},
        {0x10f, 0x42000000}, {0x110, 0x42000000}, {0x111, 0xc1800000}, {0x112, 0x41800000},
        {0x113, 0x3f800000}, {0x114, 0}, {0xb4, 0}, {0xb5, 0x3f800000},
        {0x1b3, 0x300}, {0x1b4, 0x300}
    };
    for (u32 i = 0; i < sizeof(context) / sizeof(context[0]); ++i) one(0x69, context[i][0], context[i][1]);
    const u32 vertexProgram[2] = {VertexCodeAddress >> 8, 0}, pixelProgram[2] = {PixelCodeAddress >> 8, 0};
    const u32 vertexUsers[4] = {VertexAddress, 16u << 16, 3, 0x01016fac};
    reg(0x76, 0xc8, vertexProgram, 2);
    reg(0x76, 0x008, pixelProgram, 2);
    one(0x76, 0x08b, 8); one(0x76, 0x00b, 0);
    reg(0x76, 0x08c, vertexUsers, 4);
    volatile u32* commands = (volatile u32*)CommandAddress;
    commands[words++] = 0xc0012d00; commands[words++] = 3; commands[words++] = 2;
    ((volatile u32*)LabelAddress)[0] = 0;
    commands[words++] = 0xc0064900; commands[words++] = 0;
    commands[words++] = (1u << 29) | (1u << 24);
    commands[words++] = LabelAddress; commands[words++] = 0;
    commands[words++] = 0x12345678; commands[words++] = 0; commands[words++] = 0;
    volatile struct Packet* packet = (volatile struct Packet*)PacketAddress;
    packet->addr = CommandAddress; packet->dw_num = words; packet->flags = 0;
    packet->reserved[0] = packet->reserved[1] = packet->reserved[2] = 0;
    ((void (*)(u64))0x10000000ul)(VertexHeaderAddress);
    ((void (*)(u64))0x10000000ul)(PixelHeaderAddress);
    ((void (*)(u64, u32))0x10000010ul)(PacketAddress, 0);
    ((void (*)(void))0x10000020ul)();
    for (u32 i = 0; i < 0x4000; ++i) {
        const u8 expected = i >= 4096 && i < 4096 + PixelBytes ? 255 : 0x7b;
        if (pixels[i] != expected) return 10;
    }
    if (((volatile u32*)LabelAddress)[0] != 0x12345678) return 11;
    volatile struct DisplayBuffer* display = (volatile struct DisplayBuffer*)DisplayAddress;
    display->address = ColorAddress; display->pixelFormat = 0x8000000022000000ul;
    display->width = Width; display->height = Height;
    display->tilingMode = 1; display->pitchInPixel = Width;
    display->dccAddress = 0; display->dccClearColor = 0;
    ((void (*)(u64))0x10000030ul)(DisplayAddress);
    return 0;
}
