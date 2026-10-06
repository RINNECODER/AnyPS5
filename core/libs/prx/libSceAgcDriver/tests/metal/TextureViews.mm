#include "MetalTestSupport.hpp"
#include "MetalShaderResources.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
using MetalTests::Require;
using namespace AgcDriver;
using namespace ShaderRecompiler;

constexpr const char* source = R"(
#include <metal_stdlib>
using namespace metal;
kernel void writeArray(texture2d_array<float,access::write> t [[texture(0)]],uint2 p [[thread_position_in_grid]]) { t.write(float4(17,31,73,127)/255.0f,p,0); }
kernel void readArray(texture2d_array<float,access::read> t [[texture(0)]],device uchar4* out [[buffer(0)]],constant uint4& at [[buffer(1)]]) { out[0]=uchar4(round(t.read(uint2(0),at.x,at.y)*255.0f)); }
kernel void readUint(texture2d_array<uint,access::read> t [[texture(0)]],device uint4* out [[buffer(0)]]) { out[0]=t.read(uint2(0),0); }
kernel void readSrgb(texture2d_array<float,access::read> t [[texture(0)]],device float4* out [[buffer(0)]]) { out[0]=t.read(uint2(0),0); }
kernel void compare(depth2d<float> t [[texture(0)]],device float* out [[buffer(0)]],uint i [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::nearest,compare_func::less_equal); out[i]=t.sample_compare(s,float2(.5f),i==0?.1f:.5f); }
kernel void read2D(texture2d<float> t [[texture(0)]],device uchar4* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized,filter::nearest); out[p.y*8+p.x]=uchar4(round(t.sample(s,(float2(p)+.5f)/8.0f)*255.0f)); }
kernel void readR32Uint(texture2d<uint,access::read> t [[texture(0)]],device uint* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { out[p.y*8+p.x]=t.read(p).x; }
kernel void readR32Float(texture2d<float,access::read> t [[texture(0)]],device uint* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { out[p.y*8+p.x]=as_type<uint>(t.read(p).x); }
kernel void write2D(texture2d<float,access::write> t [[texture(0)]],uint2 p [[thread_position_in_grid]]) { t.write(float4(17,31,73,127)/255.0f,p); }
)";

id<MTLComputePipelineState> Pipeline(const Metal::MetalDevice& backend,id<MTLLibrary> library,NSString* name) {
    NSError* error=nil;
    auto state=[backend.Device() newComputePipelineStateWithFunction:[library newFunctionWithName:name] error:&error];
    Require(state!=nil,error.localizedDescription.UTF8String ?: "texture view probe compilation failed");
    return state;
}

void Encode(const Metal::MetalDevice& backend,id<MTLLibrary> library,id<MTLCommandBuffer> commands,NSString* name,
            id<MTLTexture> texture,id<MTLBuffer> output,MTLSize grid=MTLSizeMake(1,1,1),const void* parameters=nullptr,std::size_t bytes=0) {
    auto encoder=[commands computeCommandEncoder];
    [encoder setComputePipelineState:Pipeline(backend,library,name)];
    [encoder setTexture:texture atIndex:0];
    if(output!=nil) [encoder setBuffer:output offset:0 atIndex:0];
    if(parameters!=nullptr) [encoder setBytes:parameters length:bytes atIndex:1];
    [encoder dispatchThreads:grid threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder endEncoding];
}

Graphics::GuestTextureResource Descriptor(std::uint32_t format=56) {
    Graphics::GuestTextureResource d{};
    d.baseAddress=0x200000;
    d.width=d.height=8;
    d.mipCount=d.allocatedMipCount=1;
    d.dimension=Graphics::TextureDimension::k2D;
    d.tileMode=Graphics::TextureTileMode::kLinear;
    d.format=format;
    d.dstSelX=4; d.dstSelY=5; d.dstSelZ=6; d.dstSelW=7;
    return d;
}

void Pixels(const void* actual,std::array<std::uint8_t,4> expected,std::size_t count,const char* message) {
    const auto* bytes=static_cast<const std::uint8_t*>(actual);
    for(std::size_t i=0;i<count*4;++i) Require(bytes[i]==expected[i%4],message);
}

void SharedViews(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    auto d=Descriptor();
    d.dimension=Graphics::TextureDimension::k2DArray;
    d.depthOrLastArray=2;
    d.mipCount=d.allocatedMipCount=3;
    d.lastLevel=2;
    const auto geometry=Graphics::DescribeSurface(d);
    std::vector<std::byte> allocation(geometry.guestBytes+128,std::byte{0xa5});
    auto guest=std::span(allocation).subspan(64,geometry.guestBytes);
    for(unsigned layer=0;layer<3;++layer) for(unsigned level=0;level<3;++level) {
        const auto& mip=geometry.mips[level];
        const std::array<std::uint8_t,4> color{std::uint8_t(3+layer*11+level*7),9,27,255};
        for(unsigned y=0;y<mip.height;++y) for(unsigned x=0;x<mip.width;++x)
            std::memcpy(guest.data()+geometry.GuestLayerOffset(layer)+mip.tiledOffset+y*mip.pitchBytes+x*4,color.data(),4);
    }
    auto expected=allocation;
    const NativeGuestMemory::BorrowedRange borrow{d.baseAddress,guest,true};
    Metal::MetalShaderResources resources(backend,std::span(&borrow,1));
    auto full=resources.Texture(d);
    auto selected=d;
    selected.baseArray=1; selected.baseLevel=selected.lastLevel=1;
    auto storage=resources.Texture(selected,true);
    auto write=backend.CommandBuffer();
    Encode(backend,library,write,@"writeArray",storage->StorageView(),nil,MTLSizeMake(4,4,1));
    backend.Wait(write);
    auto swizzled=selected;
    swizzled.dstSelX=6; swizzled.dstSelZ=4;
    auto sampled=resources.Texture(swizzled);
    auto integer=selected; integer.format=60;
    auto integerView=resources.Texture(integer);
    auto srgb=selected; srgb.format=130;
    auto srgbView=resources.Texture(srgb);
    auto commands=backend.CommandBuffer();
    auto unchanged=backend.Buffer(4),plain=backend.Buffer(4),swizzle=backend.Buffer(4),ints=backend.Buffer(16),linear=backend.Buffer(16);
    const std::array<std::uint32_t,4> untouchedAt{0,1,0,0},writtenAt{1,1,0,0},rebasedAt{0,0,0,0};
    Encode(backend,library,commands,@"readArray",full->SampledView(),unchanged,MTLSizeMake(1,1,1),untouchedAt.data(),sizeof(untouchedAt));
    Encode(backend,library,commands,@"readArray",full->SampledView(),plain,MTLSizeMake(1,1,1),writtenAt.data(),sizeof(writtenAt));
    Encode(backend,library,commands,@"readArray",sampled->SampledView(),swizzle,MTLSizeMake(1,1,1),rebasedAt.data(),sizeof(rebasedAt));
    Encode(backend,library,commands,@"readUint",integerView->SampledView(),ints);
    Encode(backend,library,commands,@"readSrgb",srgbView->SampledView(),linear);
    backend.Wait(commands);
    Pixels(unchanged.contents,{10,9,27,255},1,"shared image view changed an unrelated layer");
    Pixels(plain.contents,{17,31,73,127},1,"full image view sees stale storage writes");
    Pixels(swizzle.contents,{73,31,17,127},1,"rebased component view sees stale storage writes");
    const std::array<std::uint32_t,4> integerExpected{17,31,73,127};
    Require(std::memcmp(ints.contents,integerExpected.data(),sizeof(integerExpected))==0,"integer image view lost shared pixel bits");
    for(unsigned channel=0;channel<4;++channel) {
        float value=float(integerExpected[channel])/255;
        if(channel<3) value=value<=.04045f?value/12.92f:std::pow((value+.055f)/1.055f,2.4f);
        Require(std::abs(static_cast<const float*>(linear.contents)[channel]-value)<.00005f,"sRGB image view lost shared transfer semantics");
    }
    Require(allocation==expected,"guest image bytes published before completion");
    Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"shared image completion faulted");
    const auto& mip=geometry.mips[1];
    const std::array<std::uint8_t,4> written{17,31,73,127};
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x)
        std::memcpy(expected.data()+64+geometry.GuestLayerOffset(1)+mip.tiledOffset+y*mip.pitchBytes+x*4,written.data(),4);
    Require(allocation==expected,"shared image copyback changed other mips, layers, padding, or guards");
}

void Comparison(const Metal::MetalDevice& backend,id<MTLLibrary> library,std::uint32_t format) {
    auto d=Descriptor(format); d.width=d.height=4; d.dstSelX=0;
    const auto geometry=Graphics::DescribeSurface(d);
    std::vector<std::byte> guest(geometry.guestBytes,std::byte{0xa5});
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
        if(format==7) {const std::uint16_t depth=16384;std::memcpy(guest.data()+y*geometry.mips[0].pitchBytes+x*2,&depth,2);}
        else {const float depth=.25f;std::memcpy(guest.data()+y*geometry.mips[0].pitchBytes+x*4,&depth,4);}
    }
    const auto expected=guest;
    const NativeGuestMemory::BorrowedRange range{d.baseAddress,guest,false};
    Metal::MetalShaderResources resources(backend,std::span(&range,1));
    std::array<std::uint32_t,8> words{};
    words[0]=d.baseAddress>>8;
    words[1]=(format<<20)|(3u<<30);
    words[2]=3u<<14;
    words[3]=(5u<<3)|(6u<<6)|(7u<<9)|(9u<<28);
    DescriptorBinding binding{};
    binding.kind=DescriptorKind::SampledImage; binding.role=DescriptorRole::GuestImages;
    binding.count=1; binding.readOnly=true; binding.imageShape=DescriptorImageShape::Image2D;
    binding.imageDepthCompare={true}; binding.guestDescriptor.assign(words.begin(),words.end());
    MetalBackend::Result shader{}; shader.guest.bindings.push_back(binding);
    MetalBackend::ResourceMapping mapping{};
    mapping.count=1; mapping.kind=binding.kind; mapping.role=binding.role; mapping.texture=0; mapping.active=true;
    shader.resources.push_back(mapping);
    auto bindings=resources.Bindings(shader);
    Require(bindings.size()==1&&bindings[0].textures.size()==1,"comparison image resource binding is missing");
    auto output=backend.Buffer(8);
    auto commands=backend.CommandBuffer();
    Encode(backend,library,commands,@"compare",bindings[0].textures[0],output,MTLSizeMake(2,1,1));
    backend.Wait(commands);
    const auto* actual=static_cast<const float*>(output.contents);
    Require(actual[0]==1&&actual[1]==0,"guest zero-red swizzle changed depth comparison identity");
    Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty&&guest==expected,"readonly depth comparison changed guest bytes");
}

void Dcc(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    auto d=Descriptor(); d.dccAddress=0x400000;
    const auto geometry=Graphics::DescribeSurface(d);
    for(bool alphaLast:{false,true}) for(unsigned clear=0;clear<4;++clear) {
        d.dccAlphaOnMsb=alphaLast;
        std::vector<std::byte> guest(geometry.guestBytes,std::byte{0x93});
        std::vector<std::byte> keys(geometry.guestBytes/256,std::byte(clear*0x40));
        const auto expectedGuest=guest,expectedKeys=keys;
        std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{d.baseAddress,guest,true},{d.dccAddress,keys,true}}};
        {
            Metal::MetalShaderResources resources(backend,ranges);
            auto texture=resources.Texture(d);
            auto output=backend.Buffer(256);
            auto commands=backend.CommandBuffer();
            Encode(backend,library,commands,@"read2D",texture->SampledView(),output,MTLSizeMake(8,8,1));
            backend.Wait(commands);
            std::array<std::uint8_t,4> expected{}; expected.fill(clear>=2?255:0); expected[alphaLast?3:0]=(clear==1||clear==3)?255:0;
            Pixels(output.contents,expected,64,"DCC clear image did not synthesize the original uniform color");
            Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"readonly DCC image completion faulted");
            Require(guest==expectedGuest&&keys==expectedKeys,"readonly DCC image published texels or changed clear metadata");
        }
        {
            Metal::MetalShaderResources resources(backend,ranges);
            auto texture=resources.Texture(d,true);
            auto commands=backend.CommandBuffer();
            Encode(backend,library,commands,@"write2D",texture->StorageView(),nil,MTLSizeMake(8,8,1));
            backend.Wait(commands);
            Require(guest==expectedGuest&&keys==expectedKeys,"DCC image write published before resource completion");
            Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"written DCC image completion faulted");
            auto expected=expectedGuest;
            const std::array<std::uint8_t,4> color{17,31,73,127};
            for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) std::memcpy(expected.data()+y*geometry.mips[0].pitchBytes+x*4,color.data(),4);
            Require(guest==expected,"DCC image copyback lost GPU texels or row padding");
            Require(std::all_of(keys.begin(),keys.end(),[](auto key){return key==std::byte{0xff};}),"DCC image copyback did not publish uncompressed metadata");
        }
    }
    for(unsigned fallback=0;fallback<2;++fallback) {
        std::vector<std::byte> guest(geometry.guestBytes,std::byte{0x93});
        std::vector<std::byte> keys(geometry.guestBytes/256,std::byte{0x20});
        if(fallback==1) {std::fill(keys.begin(),keys.end(),std::byte{0xff});keys[1]=std::byte{0x40};}
        const auto expectedGuest=guest,expectedKeys=keys;
        std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{d.baseAddress,guest,false},{d.dccAddress,keys,false}}};
        Metal::MetalShaderResources resources(backend,ranges);
        auto texture=resources.Texture(d);
        auto output=backend.Buffer(256);
            auto commands=backend.CommandBuffer();
        Encode(backend,library,commands,@"read2D",texture->SampledView(),output,MTLSizeMake(8,8,1));
        backend.Wait(commands);
        Pixels(output.contents,{147,147,147,147},64,"mixed or register-dependent DCC metadata replaced stored guest texels");
        Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty&&guest==expectedGuest&&keys==expectedKeys,"DCC fallback sampling changed guest memory");
    }
}

void DccTypedViews(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    for(bool floatFirst:{false,true}) for(unsigned key:{0xc0u,0u,0xffu}) {
        auto d=Descriptor(floatFirst?22:20); d.dccAddress=0x400000;
        const auto geometry=Graphics::DescribeSurface(d);
        std::vector<std::byte> guest(geometry.guestBytes,std::byte{0xa5});
        const std::uint32_t stored=0x3e800000;
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x)
            std::memcpy(guest.data()+y*geometry.mips[0].pitchBytes+x*4,&stored,4);
        std::vector<std::byte> keys(geometry.guestBytes/256,std::byte(key));
        const auto expectedGuest=guest,expectedKeys=keys;
        std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{d.baseAddress,guest,false},{d.dccAddress,keys,false}}};
        Metal::MetalShaderResources resources(backend,ranges);
        auto first=resources.Texture(d);
        auto alternate=d; alternate.format=floatFirst?20:22;
        std::shared_ptr<Metal::MetalTexture> second;
        bool rejected=false;
        try {second=resources.Texture(alternate);} catch(const std::invalid_argument&) {rejected=true;}
        if(key==0xc0) Require(rejected,"incompatible typed DCC clear view exposed inherited pixel bits");
        else {
            Require(!rejected,"compatible typed DCC clear or uncompressed view was rejected");
            auto commands=backend.CommandBuffer();
            auto firstBits=backend.Buffer(256),secondBits=backend.Buffer(256);
            Encode(backend,library,commands,floatFirst?@"readR32Float":@"readR32Uint",first->SampledView(),firstBits,MTLSizeMake(8,8,1));
            Encode(backend,library,commands,floatFirst?@"readR32Uint":@"readR32Float",second->SampledView(),secondBits,MTLSizeMake(8,8,1));
            backend.Wait(commands);
            const auto expected=key==0?0u:stored;
            for(unsigned pixel=0;pixel<64;++pixel) {
                Require(static_cast<const std::uint32_t*>(firstBits.contents)[pixel]==expected,"first typed DCC view changed compatible clear or stored bits");
                Require(static_cast<const std::uint32_t*>(secondBits.contents)[pixel]==expected,"second typed DCC view changed compatible clear or stored bits");
            }
            Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"typed DCC view completion faulted");
        }
        Require(guest==expectedGuest&&keys==expectedKeys,"readonly typed DCC view changed guest texels or keys");
    }
}

void AccessGuards(const Metal::MetalDevice& backend) {
    auto d=Descriptor();
    const auto bytes=Graphics::DescribeSurface(d).guestBytes;
    std::vector<std::byte> guest(bytes*2,std::byte{0xa5});
    const NativeGuestMemory::BorrowedRange writable{d.baseAddress,guest,true},readonly{d.baseAddress,guest,false};
    for(unsigned mode=0;mode<5;++mode) {
        bool rejected=false;
        try {
            Metal::MetalShaderResources resources(backend,std::span(mode==3?&readonly:&writable,1));
            if(mode==0) {static_cast<void>(resources.Texture(d));auto partial=d;partial.baseAddress+=256;static_cast<void>(resources.Texture(partial));}
            if(mode==1) {static_cast<void>(resources.Buffer(d.baseAddress,4));static_cast<void>(resources.Texture(d));}
            if(mode==2) {static_cast<void>(resources.Texture(d));static_cast<void>(resources.Buffer(d.baseAddress,4));}
            if(mode==3) {static_cast<void>(resources.Texture(d));static_cast<void>(resources.Texture(d,true));}
            if(mode==4) {static_cast<void>(resources.Texture(d));auto incompatible=d;incompatible.width=4;static_cast<void>(resources.Texture(incompatible));}
        } catch(const std::exception&) {rejected=true;}
        Require(rejected,"incompatible image alias or readonly storage write was accepted");
    }
    d.dccAddress=0x400000;
    std::vector<std::byte> keys(bytes/256,std::byte{0x40});
    std::array<NativeGuestMemory::BorrowedRange,2> ranges{{writable,{d.dccAddress,keys,false}}};
    bool rejected=false;
    try {Metal::MetalShaderResources resources(backend,ranges);static_cast<void>(resources.Texture(d,true));} catch(const std::exception&) {rejected=true;}
    Require(rejected,"written clear-backed image accepted readonly DCC metadata");
}
}

void RunTextureViewsTests(const MetalTests::Context& context) {
    Metal::MetalDevice backend(context.device,context.library);
    NSError* error=nil;
    auto library=[context.device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];
    Require(library!=nil,error.localizedDescription.UTF8String ?: "texture resource contract probes failed to compile");
    SharedViews(backend,library);
    for(auto format:{7u,22u}) Comparison(backend,library,format);
    Dcc(backend,library);
    DccTypedViews(backend,library);
    AccessGuards(backend);
}
