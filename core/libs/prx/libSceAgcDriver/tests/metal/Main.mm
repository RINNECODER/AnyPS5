#include "MetalTestSupport.hpp"
#include <iostream>

void RunTextureDetileTests(const MetalTests::Context& context);
void RunColorTransferTests(const MetalTests::Context& context);
void RunMeshArgumentsTests(const MetalTests::Context& context);
void RunSampleCounterTests(const MetalTests::Context& context);
void RunPresentationTests(const MetalTests::Context& context);
void RunGuestMemoryTests(const MetalTests::Context& context);
#if ANYPS5_METAL_SHADER_BRIDGE
void RunShaderPipelineTests(const MetalTests::Context& context);
void RunShaderAlignmentTests(const MetalTests::Context& context);
#endif
#if ANYPS5_METAL_GUEST_REPLAY
void RunTextureResourceTests(const MetalTests::Context& context);
void RunTextureViewsTests(const MetalTests::Context& context);
void RunSamplerResourceTests(const MetalTests::Context& context);
void RunDepthResourceTests(const MetalTests::Context& context);
#endif

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if (argc != 2) throw std::runtime_error("Usage: metal_utility_tests <AnyPS5Utilities.metallib>");
            auto device = MTLCreateSystemDefaultDevice();
            MetalTests::Require(device != nil, "No Metal device available");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            if (library == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Cannot load Metal library");
            MetalTests::Context context(device, library);
            std::cout << "Metal GPU: " << device.name.UTF8String << '\n';
            RunTextureDetileTests(context);
            std::cout << "Texture detile/retile GPU output passed\n";
            RunColorTransferTests(context);
            std::cout << "Color transfer GPU output passed\n";
            RunMeshArgumentsTests(context);
            std::cout << "Mesh arguments GPU output passed\n";
            RunSampleCounterTests(context);
            std::cout << "Sample counter GPU output passed\n";
            RunPresentationTests(context);
            std::cout << "Presentation GPU output passed\n";
            RunGuestMemoryTests(context);
            std::cout << "Guest memory GPU output passed\n";
#if ANYPS5_METAL_SHADER_BRIDGE
            RunShaderPipelineTests(context);
            std::cout << "Converted shader pipeline GPU output passed\n";
            RunShaderAlignmentTests(context);
            std::cout << "Shader buffer alignment GPU output passed\n";
#endif
#if ANYPS5_METAL_GUEST_REPLAY
            RunTextureResourceTests(context);
            std::cout << "Guest texture resource GPU output passed\n";
            RunTextureViewsTests(context);
            std::cout << "Shared texture views and DCC GPU output passed\n";
            RunSamplerResourceTests(context);
            std::cout << "Guest sampler resource GPU output passed\n";
            RunDepthResourceTests(context);
            std::cout << "Depth surface GPU output passed\n";
#endif
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
}
