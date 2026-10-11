#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include <cpu/SceElf.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// Engine-side frame capture keeper. Presents a known four-quadrant pattern through the
// real compiled-caller -> native VideoOut -> Metal path with ANYPS5_FRAME_CAPTURE_DIR set,
// then checks the PNG of the final window image and the frame_captured diagnostics event.
// A second flip inside the capture interval must not produce another capture.
namespace {
constexpr auto RW = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr auto RX = Cpu::Permission::Read | Cpu::Permission::Execute;
constexpr std::uint64_t Control = 0x100002000ULL, Attribute = Control + 128, Rows = Control + 256;
constexpr std::uint64_t Display = 0x400000000ULL;
constexpr std::uint32_t Width = 64, Height = 64, Pitch = 80;
constexpr std::size_t Storage = 65536, CommandOffset = Storage - 64;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void store(Cpu::Machine& m, std::uint64_t address, const T& value) {
    m.Write(address, std::as_bytes(std::span(&value, 1)));
}
// Guest bytes {b0,b1,b2,255}; the window texture is BGRA so the PNG red channel is b2.
struct Quadrant { std::array<unsigned char, 3> guest; };
constexpr std::array<Quadrant, 4> Quadrants{{{{0, 0, 0}}, {{40, 60, 230}}, {{60, 200, 40}}, {{230, 200, 20}}}};
unsigned quadrant(unsigned x, unsigned y) { return (y >= Height / 2 ? 2u : 0u) + (x >= Width / 2 ? 1u : 0u); }
struct Backing {
    std::unique_ptr<std::byte, decltype(&std::free)> data{nullptr, &std::free};
    Backing() {
        void* allocation = nullptr;
        require(posix_memalign(&allocation, 65536, Storage) == 0, "Cannot allocate display backing");
        data.reset(static_cast<std::byte*>(allocation));
        std::memset(data.get(), 0xa7, Storage);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            const auto& c = Quadrants[quadrant(x, y)].guest;
            const std::array<unsigned char, 4> pixel{c[0], c[1], c[2], 255};
            std::memcpy(data.get() + (static_cast<std::size_t>(y) * Pitch + x) * 4, pixel.data(), 4);
        }
    }
    std::span<std::byte> bytes() { return {data.get(), Storage}; }
};
Cpu::SceImport identity(const char* nid) {
    Cpu::SceImport i; i.Nid = nid; i.LibraryName = i.ModuleName = "libSceVideoOut";
    i.LibraryVersion = 1; i.ModuleMajor = i.ModuleMinor = 1; i.LibraryId = 39; i.ModuleId = 40; return i;
}
struct Caller {
    Cpu::Machine& m; Cpu::SceNativeGraphicsSession& s; std::array<std::vector<char>, 7> sections;
    Caller(Cpu::Machine& machine, Cpu::SceNativeGraphicsSession& session, const char* const* files) : m(machine), s(session) {
        m.Map(0x1000, 4096, RX); m.Map(0x4000, 4096, RX); m.Map(0x5000, 4096, RW); m.Map(Control, 4096, RW);
        for (unsigned i = 0; i < 7; ++i) {
            std::ifstream input(files[i], std::ios::binary); require(input.good(), "Missing compiler-produced VideoOut caller");
            sections[i] = std::vector<char>((std::istreambuf_iterator<char>(input)), {});
            require(!sections[i].empty() && sections[i].size() < 4096, "Compiled caller bound differs");
        }
    }
    std::uint64_t call(unsigned index, const char* nid, std::array<std::uint64_t, 8> a) {
        const auto gate = s.ResolveVideoOutPublicFixture(identity(nid));
        m.Write(0x4000, std::as_bytes(std::span(sections[index]))); store(m, Control, a);
        m.Set(Cpu::Register::Rdi, gate); m.Set(Cpu::Register::Rsi, Control); m.Set(Cpu::Register::Rsp, 0x5fc8);
        store(m, 0x5fc8, std::uint64_t{0x1000});
        require(m.Run(0x4000, 0x1000, 1000) == Cpu::StopReason::Address && m.Get(Cpu::Register::Rsp) == 0x5fd0,
                "Compiled caller did not return");
        return m.Get(Cpu::Register::Rax);
    }
};
struct Completion { std::promise<void> done; };
std::uint64_t clock(void*) { return 101; }
std::uint64_t counter(void*) { return 202; }
void completed(void* context, VideoOutConfig&, std::int64_t) { static_cast<Completion*>(context)->done.set_value(); }
void present(Cpu::Machine& m, Cpu::SceNativeGraphicsSession& s, Completion& c, std::uint32_t handle, unsigned index) {
    c.done = std::promise<void>{}; auto finished = c.done.get_future();
    const std::array<std::uint32_t, 6> packet{AgcDriver::FlipPacketHeader, handle, index, 1, 37, 0};
    m.Write(Display + CommandOffset, std::as_bytes(std::span(packet)));
    s.Driver().SubmitCommandBuffer(Display + CommandOffset, packet.size(), 0, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (finished.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && std::chrono::steady_clock::now() < deadline)
        s.Window().PumpMainThread(std::chrono::milliseconds(1));
    require(finished.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, "No frame completion");
    finished.get(); s.Driver().WaitIdle();
}
std::string slurp(const std::string& path) { std::ifstream in(path); std::stringstream out; out << in.rdbuf(); return out.str(); }
std::vector<std::string> linesContaining(const std::string& text, const std::string& needle) {
    std::vector<std::string> found; std::istringstream stream(text); std::string line;
    while (std::getline(stream, line)) if (line.find(needle) != std::string::npos) found.push_back(line);
    return found;
}
void run(const char* utility, const char* const* files, const std::string& directory, const std::string& events) {
    Cpu::Machine machine; auto backing = std::make_shared<Backing>();
    machine.MapBorrowed(Display, backing->bytes(), RW);
    AnyPS5::Host::NativeMetalSessionConfiguration config; config.window = {"Frame capture fixture", Width, Height};
    config.utilityMetallib = utility; const std::array ranges{AgcDriver::NativeGuestMemory::BorrowedRange{Display, backing->bytes(), false, 171}};
    config.initialRanges = ranges;
    config.initialRangeOwner = backing; config.initialGeneration = 17;
    Completion completion; const VideoOutCompletionCallbacks callbacks{&completion, clock, counter, completed};
    auto session = Cpu::SceNativeGraphicsSession::CreateMainThread(machine, config, callbacks, {}, 0x7ffdfd000000, Cpu::TargetVideoOutAdmissions());
    Caller guest(machine, *session, files);
    const auto handle = guest.call(0, "Up36PTk687E", {255, 0, 0, 0});
    guest.call(3, "PjS5uASwcV8", {Attribute, 0x8000000000000000ULL, 1, Width, Height, 0, 0, 0});
    Cpu::SceVideoOutAttribute attr; machine.Read(Attribute, std::as_writable_bytes(std::span(&attr, 1)));
    attr.PitchInPixel = Pitch; store(machine, Attribute, attr);
    const Cpu::SceVideoOutBuffer row{Display, 0, {}}; store(machine, Rows, row);
    require(guest.call(4, "rKBUtgRrtbk", {handle, 2, 0, Rows, 1, Attribute, 0, 0}) == 0, "Linear registration failed");
    present(machine, *session, completion, handle, 0);
    // The readback completes asynchronously after presentation.
    std::vector<std::string> captured;
    for (int i = 0; i < 200 && captured.empty(); ++i) {
        session->Window().PumpMainThread(std::chrono::milliseconds(10));
        captured = linesContaining(slurp(events), "\"event\":\"frame_captured\"");
    }
    require(captured.size() == 1, "First presented frame was not captured");
    // Second flip is inside the interval and must not capture again.
    present(machine, *session, completion, handle, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    require(linesContaining(slurp(events), "\"event\":\"frame_captured\"").size() == 1, "Capture ignored the interval");
    require(linesContaining(slurp(events), "frame_capture_failed").empty(), "Capture reported a failure");
    session->RequestStop(); session->ShutdownAfterCpuStoppedMainThread();

    NSData* json = [NSData dataWithBytes:captured[0].data() length:captured[0].size()];
    NSDictionary* event = [NSJSONSerialization JSONObjectWithData:json options:0 error:nil];
    require(event != nil, "frame_captured is not JSON");
    NSString* path = event[@"path"];
    require([path hasPrefix:[NSString stringWithUTF8String:directory.c_str()]] && [path.lastPathComponent hasPrefix:@"frame-0-"] &&
            [path hasSuffix:@".png"], "Event path differs from the frame-<flip>-<ms>.png contract");
    require([event[@"flip_index"] integerValue] == 0 && [event[@"guest_width"] integerValue] == Width &&
            [event[@"guest_height"] integerValue] == Height, "Event flip/guest extent differs");
    NSArray* listing = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:[NSString stringWithUTF8String:directory.c_str()] error:nil];
    require(listing.count == 1, "Capture directory holds more than the single captured frame");
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], nullptr);
    require(source != nullptr, "PNG is missing or unreadable");
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    require(image != nullptr, "PNG has no image");
    const auto w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    require(w == [event[@"width"] unsignedIntegerValue] && h == [event[@"height"] unsignedIntegerValue], "PNG extent differs from event");
    require(w >= Width && h >= Height && w == h && w % Width == 0, "PNG is not the scaled window image");
    std::vector<unsigned char> rgba(w * h * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
    // Quadrant centres are far from the filtered seams. Expected red = guest b2, blue = guest b0.
    for (unsigned q = 0; q < 4; ++q) {
        const auto x = (q % 2 ? 3 : 1) * w / 4, y = (q / 2 ? 3 : 1) * h / 4;
        const auto* p = rgba.data() + (y * w + x) * 4;
        const auto& g = Quadrants[q].guest;
        const auto close = [](int a, int b) { return std::abs(a - b) <= 4; };
        require(close(p[0], g[2]) && close(p[1], g[1]) && close(p[2], g[0]), "PNG quadrant colour differs from the presented pattern");
    }
    CGContextRelease(context); CGColorSpaceRelease(space); CGImageRelease(image); CFRelease(source);
    const double ratio = [event[@"nonblack_ratio"] doubleValue];
    const auto distinct = [event[@"distinct_colors"] integerValue];
    require(ratio > 0.70 && ratio < 0.80, "nonblack_ratio differs from the three-quarter pattern");
    require(distinct >= 4 && distinct <= 16, "distinct_colors differs from the four-colour pattern");
    std::cout << "PASS frame capture: first flip -> window-sized PNG, pattern quadrants, nonblack_ratio=" << ratio
              << " distinct_colors=" << distinct << ", interval suppressed second capture\n";
}
}
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            require(argc == 9, "Frame capture fixture requires metallib and seven compiled callers");
            char pattern[] = "/tmp/anyps5-frame-capture-XXXXXX";
            require(mkdtemp(pattern) != nullptr, "Cannot create capture directory");
            const std::string directory = std::string(pattern) + "/frames", events = std::string(pattern) + "/events.log";
            require(setenv("ANYPS5_FRAME_CAPTURE_DIR", directory.c_str(), 1) == 0 &&
                    setenv("ANYPS5_FRAME_CAPTURE_INTERVAL_MS", "600000", 1) == 0, "Cannot set capture environment");
            require(freopen(events.c_str(), "w", stderr) != nullptr, "Cannot redirect diagnostics");
            try { run(argv[1], argv + 2, directory, events); }
            catch (...) { fflush(stderr); throw; }
            return 0;
        } catch (const std::exception& error) { std::cout << "FAIL " << error.what() << '\n'; return 1; }
    }
}
