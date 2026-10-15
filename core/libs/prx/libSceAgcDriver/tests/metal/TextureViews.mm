#include "MetalTestSupport.hpp"
#include "MetalShaderResources.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>
#include <sys/mman.h>
#include <spirv/unified1/spirv.hpp>

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
kernel void readSrgb(texture2d_array<float> t [[texture(0)]],device float4* out [[buffer(0)]],uint i [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized,filter::nearest); out[i]=t.sample(s,(float2(i%t.get_width(),i/t.get_width())+.5f)/float2(t.get_width(),t.get_height()),0); }
kernel void compare(depth2d<float> t [[texture(0)]],device float* out [[buffer(0)]],uint i [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::nearest,compare_func::less_equal); out[i]=t.sample_compare(s,float2(.5f),i==0?.1f:.5f); }
kernel void read2D(texture2d<float> t [[texture(0)]],device uchar4* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized,filter::nearest); out[p.y*8+p.x]=uchar4(round(t.sample(s,(float2(p)+.5f)/8.0f)*255.0f)); }
kernel void readR32Uint(texture2d<uint,access::read> t [[texture(0)]],device uint* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { out[p.y*8+p.x]=t.read(p).x; }
kernel void writeColor2D(texture2d<float,access::write> t [[texture(0)]],constant uint4& color [[buffer(1)]],uint2 p [[thread_position_in_grid]]) { t.write(float4(color)/255.0f,p); }
kernel void copyWords(texture2d<float> unused [[texture(0)]],device uint* out [[buffer(0)]],device const uint* in [[buffer(1)]],uint i [[thread_position_in_grid]]) { out[i]=in[i]; }
kernel void readR32Float(texture2d<float,access::read> t [[texture(0)]],device uint* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { out[p.y*8+p.x]=as_type<uint>(t.read(p).x); }
struct BdaHeader { uint version,count,entryBytes,reserved; };
struct BdaRange { ulong begin,end; device uchar* address; uint permissions,reserved; };
kernel void physicalStore(device const BdaHeader* header [[buffer(0)]],device atomic_uint* fault [[buffer(1)]],constant ulong& address [[buffer(2)]]) {
    device const BdaRange* ranges=reinterpret_cast<device const BdaRange*>(header+1);
    for(uint i=0;i<header->count;++i) {
        if(address<ranges[i].begin||address>=ranges[i].end||4>ranges[i].end-address||(ranges[i].permissions&2)==0) continue;
        *reinterpret_cast<device uint*>(ranges[i].address+address-ranges[i].begin)=0x9172a3b4;
        uint key=uint(address>>17)+1u;
        uint slot=((key*0x9e3779b1u)>>21)&2047u;
        atomic_store_explicit(fault+10+2*slot,key,memory_order_relaxed);
        atomic_store_explicit(fault+10+2*slot+1,1u<<uint((address>>12)&31ul),memory_order_relaxed);
        atomic_store_explicit(fault+10+4096,slot,memory_order_relaxed);
        atomic_store_explicit(fault+9,1u,memory_order_relaxed);
        return;
    }
}
kernel void write2D(texture2d<float,access::write> t [[texture(0)]],uint2 p [[thread_position_in_grid]]) { t.write(float4(17,31,73,127)/255.0f,p); }
kernel void writeArrayColor(texture2d_array<float,access::write> t [[texture(0)]],constant float4& c [[buffer(1)]],uint2 p [[thread_position_in_grid]]) { t.write(c,p,0); }
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

    struct ChannelFormat { std::uint32_t srgb,unorm,integer,channels; };
    constexpr std::array<ChannelFormat,2> channelFormats{{{128,1,5,1},{129,14,18,2}}};
    constexpr std::array<unsigned,8> redBytes{0,1,10,11,64,128,192,255},greenBytes{255,192,128,64,11,10,1,0};
    for(const auto& format:channelFormats) for(bool srgbFirst:{false,true}) {
        auto narrow=Descriptor(format.unorm);
        narrow.dimension=Graphics::TextureDimension::k2DArray;
        narrow.depthOrLastArray=2;
        narrow.mipCount=narrow.allocatedMipCount=3;
        narrow.lastLevel=2;
        const auto surface=Graphics::DescribeSurface(narrow);
        std::vector<std::byte> bytes(surface.guestBytes+128,std::byte{0xa5});
        auto pixels=std::span(bytes).subspan(64,surface.guestBytes);
        for(unsigned layer=0;layer<3;++layer) for(unsigned level=0;level<3;++level) {
            const auto& plane=surface.mips[level];
            for(unsigned y=0;y<plane.height;++y) for(unsigned x=0;x<plane.width;++x) {
                auto* at=pixels.data()+surface.GuestLayerOffset(layer)+plane.tiledOffset+y*plane.pitchBytes+x*format.channels;
                at[0]=std::byte((redBytes[x]+layer*11+level*7+y*17)&255);
                if(format.channels==2) at[1]=std::byte((greenBytes[x]+level*19+layer*23+y*5)&255);
            }
        }
        auto expectedBytes=bytes;
        const NativeGuestMemory::BorrowedRange range{narrow.baseAddress,pixels,true};
        Metal::MetalShaderResources narrowResources(backend,std::span(&range,1));
        auto first=narrow; if(srgbFirst) first.format=format.srgb;
        static_cast<void>(narrowResources.Texture(first));
        auto gammaFull=narrow; gammaFull.format=format.srgb;
        auto gammaFullView=narrowResources.Texture(gammaFull);
        auto selectedGamma=gammaFull; selectedGamma.baseArray=1; selectedGamma.baseLevel=selectedGamma.lastLevel=1;
        auto storageGamma=narrowResources.Texture(selectedGamma,true);
        auto narrowCommands=backend.CommandBuffer();
        Encode(backend,library,narrowCommands,@"writeArray",storageGamma->StorageView(),nil,MTLSizeMake(4,4,1));
        auto selectedLinear=selectedGamma; selectedLinear.format=format.unorm;
        auto linearSelected=narrowResources.Texture(selectedLinear);
        auto selectedInteger=selectedGamma; selectedInteger.format=format.integer;
        auto integerSelected=narrowResources.Texture(selectedInteger);
        const auto guarded=[&](unsigned size=16) {
            auto result=backend.Buffer(size+16);
            std::memset(result.contents,0xa7,result.length);
            return result;
        };
        auto narrowUnchanged=guarded(64*16),narrowPlain=guarded(4),narrowInteger=guarded(),narrowLinear=guarded();
        Encode(backend,library,narrowCommands,@"readSrgb",gammaFullView->SampledView(),narrowUnchanged,MTLSizeMake(64,1,1));
        Encode(backend,library,narrowCommands,@"readArray",linearSelected->SampledView(),narrowPlain,MTLSizeMake(1,1,1),rebasedAt.data(),sizeof(rebasedAt));
        Encode(backend,library,narrowCommands,@"readUint",integerSelected->SampledView(),narrowInteger);
        Encode(backend,library,narrowCommands,@"readSrgb",storageGamma->SampledView(),narrowLinear);
        backend.Wait(narrowCommands);
        const auto decode=[](float encoded) {
            const float value=std::clamp(encoded/255,0.0f,1.0f);
            return value<=.04045f?value/12.92f:std::pow((value+.055f)/1.055f,2.4f);
        };
        const auto color=[&](const float* actual,unsigned red,unsigned green,const char* message) {
            const std::array<unsigned,2> encoded{red,green};
            for(unsigned channel=0;channel<format.channels;++channel)
                Require(std::isfinite(actual[channel])&&actual[channel]>=decode(float(encoded[channel])-.5f)-.00001f&&actual[channel]<=decode(float(encoded[channel])+.5f)+.00001f,message);
            for(unsigned channel=format.channels;channel<4;++channel)
                Require(actual[channel]==(channel==3?1.0f:0.0f),message);
        };
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x)
            color(static_cast<const float*>(narrowUnchanged.contents)+(y*8+x)*4,(redBytes[x]+y*17)&255,(greenBytes[x]+y*5)&255,"R8 or RG8 sRGB materialization changed original encoded-channel transfer or an unrelated mip");
        color(static_cast<const float*>(narrowLinear.contents),17,31,"R8 or RG8 sRGB sampled view lost shared storage writes or channel transfer");
        Pixels(narrowPlain.contents,{17,std::uint8_t(format.channels==2?31:0),0,255},1,"R8 or RG8 sRGB storage view encoded gamma instead of writing UNORM channel bits");
        const std::array<std::uint32_t,4> integerColor{17,format.channels==2?31u:0u,0,1};
        Require(std::memcmp(narrowInteger.contents,integerColor.data(),sizeof(integerColor))==0,"R8 or RG8 integer view changed shared sRGB storage bits");
        const std::array<id<MTLBuffer>,4> outputs{narrowUnchanged,narrowPlain,narrowInteger,narrowLinear};
        for(unsigned index=0;index<outputs.size();++index) {
            const auto* at=static_cast<const std::uint8_t*>(outputs[index].contents);
            const unsigned writtenBytes=index==0?64*16:index==1?4:16;
            Require(std::all_of(at+writtenBytes,at+outputs[index].length,[](auto value){return value==0xa7;}),"R8 or RG8 texture view probe overwrote its output guard");
        }
        Require(bytes==expectedBytes,"R8 or RG8 storage bits published before resource completion");
        Require(narrowResources.Complete(narrowCommands).state==BdaAbi::FaultState::Empty,"R8 or RG8 sRGB view completion faulted");
        const auto& writtenMip=surface.mips[1];
        for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
            auto* at=expectedBytes.data()+64+surface.GuestLayerOffset(1)+writtenMip.tiledOffset+y*writtenMip.pitchBytes+x*format.channels;
            at[0]=std::byte{17};
            if(format.channels==2) at[1]=std::byte{31};
        }
        Require(bytes==expectedBytes,"R8 or RG8 sRGB copyback changed other mips, layers, channel bits, row padding, or guards");
    }
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
    for(unsigned mode=0;mode<5;++mode) {
        auto descriptor=Descriptor(); descriptor.dccAddress=0x400000;
        const auto extent=Graphics::DescribeSurface(descriptor).guestBytes;
        std::vector<std::byte> guest(extent,std::byte{0x93});
        std::vector<std::byte> keys(Graphics::DccKeyBytes(extent),std::byte{0x00});
        const auto expectedGuest=guest,expectedKeys=keys;
        std::vector<NativeGuestMemory::BorrowedRange> ranges{
            {descriptor.baseAddress,std::span(guest).first(extent/2),true},
            {descriptor.dccAddress,keys,true}};
        if(mode!=0) ranges.push_back({descriptor.baseAddress+extent/2,std::span(guest).subspan(extent/2),mode==3});
        if(mode==4) ranges[0].host=guest;
        if(mode==4) ranges.pop_back();
        Metal::MetalShaderResources resources(backend,ranges);
        DescriptorBinding binding{};
        binding.kind=DescriptorKind::StorageImage; binding.role=DescriptorRole::GuestImages;
        binding.count=1; binding.imageShape=DescriptorImageShape::Image2D; binding.imageWritten={true};
        binding.guestDescriptor={static_cast<std::uint32_t>(descriptor.baseAddress>>8),
            (descriptor.format<<20)|(3u<<30),1u|(7u<<14),0xfacu|(9u<<28),0,0,
            (1u<<21)|static_cast<std::uint32_t>((descriptor.dccAddress>>8)&0xffu)<<24,
            static_cast<std::uint32_t>(descriptor.dccAddress>>16)};
        MetalBackend::Result shader{}; shader.guest.bindings.push_back(binding);
        MetalBackend::ResourceMapping mapping{};
        mapping.count=1; mapping.kind=binding.kind; mapping.role=binding.role; mapping.texture=0; mapping.active=true;
        shader.resources.push_back(mapping);
        if(mode==2) {
            shader.guest.bindings[0].imageWritten={false};
            shader.guest.bindings[0].readOnly=true;
            static_cast<void>(resources.Bindings(shader));
            shader.guest.bindings[0]=binding;
        }
        const auto residency=resources.Residency().size();
        bool rejected=false;
        std::vector<Metal::MetalShaderResourceBinding> bindings;
        try {bindings=resources.Bindings(shader);} catch(const std::invalid_argument&) {rejected=true;}
        if(mode<3) {
            Require(rejected,"partially writable DCC storage binding was accepted");
            Require(resources.Residency().size()==residency,"rejected DCC write published a native image view");
            auto commands=backend.CommandBuffer();
            backend.Wait(commands);
            Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"rejected DCC write left a completion fault");
            Require(guest==expectedGuest&&keys==expectedKeys,"rejected DCC write changed texels or clear metadata");
        } else {
            Require(!rejected&&bindings.size()==1&&bindings[0].textures.size()==1,"fully writable DCC storage binding was rejected");
            auto commands=backend.CommandBuffer();
            Encode(backend,library,commands,@"write2D",bindings[0].textures[0],nil,MTLSizeMake(8,8,1));
            backend.Wait(commands);
            Require(guest==expectedGuest&&keys==expectedKeys,"dense DCC storage published before completion");
            Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"dense DCC storage completion faulted");
            auto expected=expectedGuest;
            const std::array<std::uint8_t,4> color{17,31,73,127};
            for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x)
                std::memcpy(expected.data()+y*geometry.mips[0].pitchBytes+x*4,color.data(),4);
            Require(guest==expected,"fully writable DCC storage lost texels or row padding");
            Require(std::all_of(keys.begin(),keys.end(),[](auto key){return key==std::byte{0xff};}),"dense DCC storage did not publish uncompressed metadata");
        }
    }
    constexpr std::array<std::string_view,4> categories{"register","mixed","unreadable","1111"};
    for(unsigned unresolved=0;unresolved<categories.size();++unresolved) {
        auto descriptor=d;
        if(unresolved==3) descriptor.format=169;
        const auto extent=Graphics::DescribeSurface(descriptor).guestBytes;
        std::vector<std::byte> guest(extent,std::byte{0x93});
        std::vector<std::byte> keys(extent/256,std::byte{0x20});
        if(unresolved==1) {std::fill(keys.begin(),keys.end(),std::byte{0xff});keys[1]=std::byte{0x40};}
        if(unresolved==3) std::fill(keys.begin(),keys.end(),std::byte{0xc0});
        const auto expectedGuest=guest,expectedKeys=keys;
        std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{descriptor.baseAddress,guest,false},{descriptor.dccAddress,keys,false}}};
        Metal::MetalShaderResources resources(backend,std::span(ranges).first(unresolved==2?1:2));
        bool rejected=false;
        try {static_cast<void>(resources.Texture(descriptor));} catch(const std::runtime_error& error) {
            const std::string_view message(error.what());
            rejected=message.starts_with("Metal texture DCC metadata is unresolved (")&&message.find(categories[unresolved])!=std::string_view::npos;
        }
        Require(rejected,"unresolved DCC metadata exposed stored texels instead of an explicit categorized failure");
        Require(guest==expectedGuest&&keys==expectedKeys,"unresolved DCC rejection changed guest memory");
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

// A mip-chain pass binds one surface at the same base twice: one view names its DCC metadata, the
// other names none. With "uncompressed" keys both mean the same plain texels and share one backing
// (a write through either is seen by the other and copied back with the keys left uncompressed);
// with clear keys they disagree and the second binding is refused.
void DccMetadataViews(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    for(bool dccFirst:{false,true}) for(unsigned key:{0xffu,0x00u}) {
        auto plain=Descriptor(),dcc=Descriptor(); dcc.dccAddress=0x400000;
        const auto geometry=Graphics::DescribeSurface(plain);
        std::vector<std::byte> guest(geometry.guestBytes,std::byte{0x93});
        std::vector<std::byte> keys(geometry.guestBytes/256,std::byte(key));
        const auto expectedGuest=guest,expectedKeys=keys;
        std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{plain.baseAddress,guest,true},{dcc.dccAddress,keys,true}}};
        Metal::MetalShaderResources resources(backend,ranges);
        auto first=resources.Texture(dccFirst?dcc:plain);
        std::shared_ptr<Metal::MetalTexture> second;
        bool rejected=false;
        try {second=resources.Texture(dccFirst?plain:dcc,true);} catch(const std::invalid_argument&) {rejected=true;}
        if(key!=0xff) {
            Require(rejected,"a plain view shared the backing of a DCC-cleared view");
            auto commands=backend.CommandBuffer();
            backend.Wait(commands);
            Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"refused DCC/plain view left a completion fault");
            Require(guest==expectedGuest&&keys==expectedKeys,"refused DCC/plain view changed texels or keys");
            continue;
        }
        Require(!rejected&&second!=nullptr,"DCC and plain views of uncompressed texels did not share a backing");
        auto output=backend.Buffer(256);
        auto commands=backend.CommandBuffer();
        Encode(backend,library,commands,@"write2D",second->StorageView(),nil,MTLSizeMake(8,8,1));
        Encode(backend,library,commands,@"read2D",first->SampledView(),output,MTLSizeMake(8,8,1));
        backend.Wait(commands);
        Pixels(output.contents,{17,31,73,127},64,"a write through one view of a shared DCC/plain backing was not seen by the other");
        Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"shared DCC/plain view completion faulted");
        auto expected=expectedGuest;
        const std::array<std::uint8_t,4> color{17,31,73,127};
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) std::memcpy(expected.data()+y*geometry.mips[0].pitchBytes+x*4,color.data(),4);
        Require(guest==expected,"shared DCC/plain view copy-back lost texels");
        Require(std::all_of(keys.begin(),keys.end(),[](auto k){return k==std::byte{0xff};}),"shared DCC/plain view left compressed metadata");
    }
    // A mip-chain pass: this work renders one level of a mipmapped color target (bound without DCC)
    // and samples another level through a descriptor naming the target's stale cleared keys.
    for(bool written:{true,false}) {
        auto target=Descriptor(); target.mipCount=target.allocatedMipCount=4; target.lastLevel=3;
        const auto geometry=Graphics::DescribeSurface(target);
        std::vector<std::byte> guest(geometry.guestBytes,std::byte{0x93});
        std::vector<std::byte> keys(geometry.guestBytes/256,std::byte{0x00});
        std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{target.baseAddress,guest,true},{0x400000,keys,true}}};
        Metal::MetalShaderResources resources(backend,ranges);
        auto level=target; level.baseLevel=level.lastLevel=1;
        auto rendered=resources.Texture(level,written,false,false,written?std::optional<std::uint32_t>(1):std::nullopt);
        auto sampled=target; sampled.baseLevel=sampled.lastLevel=0; sampled.dccAddress=0x400000;
        bool rejected=false;
        try {static_cast<void>(resources.Texture(sampled));} catch(const std::invalid_argument&) {rejected=true;}
        Require(rejected!=written,written?"a sampled level of a written mipmapped target was refused over its stale DCC keys":
            "a plain unwritten mipmapped view shared the backing of a DCC-cleared view");
        auto commands=backend.CommandBuffer();
        backend.Wait(commands);
        Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"mip-chain DCC/plain views left a completion fault");
    }
}

MetalBackend::Result AliasProgram(const Metal::MetalDevice& backend,bool global,std::uint64_t address) {
    constexpr std::array<std::uint32_t,17> descriptorCode{
        0x34020082,0xe0302000,0x80000401,0xbf8c3f70,0x7e000000,0x7e003600,0x7e008200,0x4a080881,
        0xd5800000,0x00000000,0xd59b0000,0x00000000,0xd5c10000,0x00000000,0xe0702000,0x80010401,0xbf810000};
    constexpr std::array<std::uint32_t,22> globalCode{
        0xbeea0400,0x34020082,0x34040084,0x340c0085,0x4a0c0cff,0x00001000,0x4ad404ff,0x00000048,
        0xdc308010,0x046a0001,0xdcc98700,0x0c6a0201,0xdc3887b8,0x006a006a,0xbf8c3f70,0xdc788000,
        0x006a0006,0xdc708010,0x006a0406,0xdc708014,0x006a0c06,0xbf810000};
    std::vector<std::uint32_t> users;
    if(global) users={static_cast<std::uint32_t>(address),0};
    else users={0x800000,4u<<16,256,0x01016fac,0x900000,4u<<16,256,0x01016fac};
    const auto code=global?std::span<const std::uint32_t>(globalCode):std::span<const std::uint32_t>(descriptorCode);
    static constexpr std::array<std::uint32_t,3> capabilities{spv::CapabilityInt64,spv::CapabilityPhysicalStorageBufferAddresses,spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view,2> extensions{"SPV_KHR_physical_storage_buffer","SPV_KHR_8bit_storage"};
    const auto maximum=backend.Device().maxThreadsPerThreadgroup;
    const SpirvTarget target{0x00401000,0x00010300,32,BdaAbi::Version,capabilities,extensions,false,
        {std::uint32_t(maximum.width),std::uint32_t(maximum.height),std::uint32_t(maximum.depth)},
        std::uint32_t(maximum.width),std::uint32_t(backend.Device().maxThreadgroupMemoryLength),{}, {}};
    const std::array<MemoryRegion,1> memory{{{0x500000,std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{64,1,1},0,{false,false,false},false,1,{}};
    RecompileRequest request{{ShaderStage::Compute,0x500000,code,0,{}},{32,0,users,compute,{},{},memory},target,{0,0,0,128},{},false};
    MetalBackend::TargetOptions options;
    options.supportsInt64=options.supportsGpuAddresses=options.supportsSimdGroups=true;
    return MetalBackend::ConvertToMetal(Recompile(request),ShaderStage::Compute,options);
}

void PhysicalAllowed(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    {
        std::array<std::uint32_t,288> allocation;
        allocation.fill(0xdeadbeef);
        auto words=std::span(allocation).subspan(16,256);
        for(unsigned lane=0;lane<64;++lane) words[lane*4]=lane*0x01010101u+7;
        auto expected=allocation;
        for(unsigned lane=0;lane<64;++lane) ++expected[16+lane*4];
        auto bytes=std::as_writable_bytes(words);
        const std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{0x800000,bytes,false},{0x900000,bytes,true}}};
        Metal::MetalShaderResources resources(backend,ranges);
        const auto shader=AliasProgram(backend,false,0);
        Metal::MetalComputePipeline pipeline(backend.Device(),shader);
        auto commands=backend.CommandBuffer();
        pipeline.Encode(commands,resources.Bindings(shader),MTLSizeMake(64,1,1),{},resources.Residency());
        backend.Wait(commands);
        Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"aliased descriptor execution faulted");
        Require(allocation==expected,"legal descriptor aliases lost original load/add/store results or guards");
    }
    auto d=Descriptor();
    const auto count=Graphics::DescribeSurface(d).guestBytes;
    std::vector<std::byte> guest(count,std::byte{0x93});
    const auto expected=guest;
    const std::array<NativeGuestMemory::BorrowedRange,2> ranges{{{d.baseAddress,guest,false},{0x600000,guest,true}}};
    Metal::MetalShaderResources resources(backend,ranges);
    auto texture=resources.Texture(d);
    auto output=backend.Buffer(256);
    auto commands=backend.CommandBuffer();
    Encode(backend,library,commands,@"read2D",texture->SampledView(),output,MTLSizeMake(8,8,1));
    backend.Wait(commands);
    Pixels(output.contents,{147,147,147,147},64,"unused physical borrow alias changed active sampled image");
    Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty&&guest==expected,"unused physical alias changed guest bytes at completion");
}

void PhysicalGuards(const Metal::MetalDevice& backend) {
    auto d=Descriptor();
    const auto count=Graphics::DescribeSurface(d).guestBytes;
    constexpr std::uint64_t alias=0x600100;
    for(unsigned target=0;target<3;++target) for(bool bufferFirst:{false,true}) {
        std::vector<std::byte> guest(count,std::byte{0x93}),keys(count/256,std::byte{0xff});
        auto descriptor=d;
        if(target==1) descriptor.dccAddress=0x400000;
        auto physical=target==1?std::span(keys):std::span(guest).subspan(256,16);
        std::vector<NativeGuestMemory::BorrowedRange> ranges{{d.baseAddress,guest,false}};
        if(target==1) ranges.push_back({descriptor.dccAddress,keys,false});
        ranges.push_back({alias,target==2?std::span(guest):physical,true});
        Metal::MetalShaderResources resources(backend,ranges);
        bool rejected=false;
        try {
            if(target==2) {
                static_cast<void>(resources.Texture(descriptor));
                auto other=descriptor;other.baseAddress=alias;
                static_cast<void>(resources.Texture(other));
            } else {
                if(bufferFirst) static_cast<void>(resources.Buffer(alias,physical.size(),true));
                static_cast<void>(resources.Texture(descriptor));
                if(!bufferFirst) static_cast<void>(resources.Buffer(alias,physical.size(),true));
            }
        } catch(const std::invalid_argument& error) {
            const std::string_view message(error.what());
            rejected=message.find(target==1?"DCC metadata":"image")!=std::string_view::npos&&message.find(target==2?"overlapping":"alias")!=std::string_view::npos;
        }
        Require(rejected,"active physical image or metadata alias was accepted at a distinct guest address");
        Require(std::all_of(guest.begin(),guest.end(),[](auto byte){return byte==std::byte{0x93};})&&
            std::all_of(keys.begin(),keys.end(),[](auto byte){return byte==std::byte{0xff};}),"physical access rejection changed guest bytes");
        if(target==2) break;
    }
}

void PhysicalBdaGuards(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    constexpr std::uint64_t writeAddress=0x600080;
    const auto shader=AliasProgram(backend,true,writeAddress);
    for(bool metadata:{false,true}) for(bool adjacent:{false,true}) {
        auto d=Descriptor();
        const auto count=Graphics::DescribeSurface(d).guestBytes;
        std::vector<std::byte> guest(count,std::byte{0x93}),keys(count/256,std::byte{0xff});
        std::array<std::uint32_t,3> scalar{0xdeadbeef,0xcdcdcdcd,0xdeadbeef};
        auto destination=adjacent?std::as_writable_bytes(std::span(scalar)).subspan(4,4):
            (metadata?std::span(keys).first(4):std::span(guest).first(4));
        if(metadata) d.dccAddress=adjacent?0x600100:0x400000;
        else if(adjacent) d.baseAddress=0x600100;
        std::vector<NativeGuestMemory::BorrowedRange> ranges{{d.baseAddress,guest,false},{writeAddress,destination,true}};
        if(metadata) ranges.push_back({d.dccAddress,keys,false});
        const auto expectedGuest=guest,expectedKeys=keys;
        Metal::MetalShaderResources resources(backend,ranges);
        static_cast<void>(resources.Texture(d));
        id<MTLBuffer> table=nil,fault=nil;
        for(const auto& binding:resources.Bindings(shader)) {
            const auto found=std::find_if(shader.guest.bindings.begin(),shader.guest.bindings.end(),[&](const auto& guestBinding){
                return guestBinding.descriptorSet==binding.descriptorSet&&guestBinding.binding==binding.binding;
            });
            Require(found!=shader.guest.bindings.end()&&binding.buffers.size()==1,"physical write ABI binding is invalid");
            if(found->role==DescriptorRole::BdaPagetable) table=binding.buffers[0].buffer;
            if(found->role==DescriptorRole::FaultBuffer) fault=binding.buffers[0].buffer;
        }
        Require(table!=nil&&fault!=nil,"physical write ABI resources are missing");
        auto commands=backend.CommandBuffer();
        auto encoder=[commands computeCommandEncoder];
        [encoder setComputePipelineState:Pipeline(backend,library,@"physicalStore")];
        for(auto resource:resources.Residency()) [encoder useResource:resource usage:MTLResourceUsageRead|MTLResourceUsageWrite];
        [encoder setBuffer:table offset:0 atIndex:0]; [encoder setBuffer:fault offset:0 atIndex:1];
        [encoder setBytes:&writeAddress length:sizeof(writeAddress) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [encoder endEncoding]; backend.Wait(commands);
        bool rejected=false;
        try {Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"physical write completion faulted");}
        catch(const std::runtime_error& error) {
            const std::string_view message(error.what());
            rejected=message.starts_with("Metal draw BDA writes alias")&&message.find(metadata?"DCC metadata":"active image")!=std::string_view::npos;
            if(!rejected) throw;
        }
        Require(rejected!=adjacent,"dirty physical write accepted an active image alias or rejected an unrelated same-page neighbor");
        Require(guest==expectedGuest&&keys==expectedKeys,"dirty physical write modified active image texels or metadata");
        Require(scalar[0]==0xdeadbeef&&scalar[2]==0xdeadbeef&&scalar[1]==(adjacent?0x9172a3b4u:0xcdcdcdcdu),"partial-page copyback lost its actual GPU write or scalar guards");
    }
}

void StoragePermissions(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    const auto d=Descriptor();
    const auto geometry=Graphics::DescribeSurface(d);
    const auto bytes=geometry.guestBytes;
    // Initial binding, cached-view upgrade, and upgrade creating a distinct component view.
    for(unsigned access:{0u,1u,2u}) for(unsigned layout=0;layout<4;++layout) {
        std::vector<std::byte> allocation(bytes+128,std::byte{0xa5});
        auto guest=std::span(allocation).subspan(64,bytes);
        std::fill(guest.begin(),guest.end(),std::byte{0x93});
        const auto original=allocation;
        std::array<std::byte,64> neighbor;
        neighbor.fill(std::byte{0xb7});
        const auto originalNeighbor=neighbor;
        std::vector<NativeGuestMemory::BorrowedRange> ranges{
            {d.baseAddress,guest.first(bytes/2),layout!=0}};
        if(layout!=3) ranges.push_back({d.baseAddress+bytes/2,guest.subspan(bytes/2),layout!=1});
        ranges.push_back({d.baseAddress+bytes,neighbor,false});
        Metal::MetalShaderResources resources(backend,ranges);
        DescriptorBinding binding{};
        binding.kind=DescriptorKind::StorageImage; binding.role=DescriptorRole::GuestImages;
        binding.count=1; binding.imageShape=DescriptorImageShape::Image2D; binding.imageWritten={true};
        binding.guestDescriptor={static_cast<std::uint32_t>(d.baseAddress>>8),
            (d.format<<20)|(3u<<30),1u|(7u<<14),0xfacu|(9u<<28),0,0,0,0};
        MetalBackend::Result shader{}; shader.guest.bindings.push_back(binding);
        MetalBackend::ResourceMapping mapping{};
        mapping.count=1; mapping.kind=binding.kind; mapping.role=binding.role; mapping.texture=0; mapping.active=true;
        shader.resources.push_back(mapping);
        id<MTLTexture> captured=nil;
        if(access!=0) {
            shader.guest.bindings[0].imageWritten={false};
            shader.guest.bindings[0].readOnly=true;
            const auto read=resources.Bindings(shader);
            Require(read.size()==1&&read[0].textures.size()==1,"mixed or sparse storage read binding was rejected");
            captured=read[0].textures[0];
            auto output=backend.Buffer(256);
            auto commands=backend.CommandBuffer();
            Encode(backend,library,commands,@"read2D",captured,output,MTLSizeMake(8,8,1));
            backend.Wait(commands);
            Pixels(output.contents,{147,147,147,147},layout==3?32:64,"staged read lost committed read-only or writable pixels");
            if(layout==3) Pixels(static_cast<const std::byte*>(output.contents)+128,{0,0,0,0},32,"staged sparse read did not zero missing intervals");
            shader.guest.bindings[0]=binding;
            if(access==2) shader.guest.bindings[0].guestDescriptor[3]=(0xfacu&~7u)|6u|(9u<<28);
        }
        const std::vector<id<MTLResource>> residency(resources.Residency().begin(),resources.Residency().end());
        bool rejected=false;
        std::vector<Metal::MetalShaderResourceBinding> bound;
        try {bound=resources.Bindings(shader);} catch(const std::invalid_argument& error) {
            rejected=std::string_view(error.what()).find("read-only committed guest intervals")!=std::string_view::npos;
            if(!rejected) throw;
        }
        if(layout<2) {
            Require(rejected,access==0?"mixed committed non-DCC writable storage binding was accepted":
                "mixed committed non-DCC storage read-to-write upgrade was accepted");
            Require(std::equal(residency.begin(),residency.end(),resources.Residency().begin(),resources.Residency().end()),
                "rejected storage write mutated residency");
        } else Require(!rejected&&bound.size()==1&&bound[0].textures.size()==1,"writable split or sparse storage binding was rejected");
        auto commands=backend.CommandBuffer();
        // Mutating a retained read view after rejection must not grant host publication permission.
        auto target=layout<2?captured:bound[0].textures[0];
        if(target!=nil) Encode(backend,library,commands,@"write2D",target,nil,MTLSizeMake(8,8,1));
        backend.Wait(commands);
        Require(allocation==original&&neighbor==originalNeighbor,"storage bytes or guards published before completion");
        Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"storage permission completion faulted");
        auto expected=original;
        if(layout>=2) {
            const std::array<std::uint8_t,4> color{17,31,73,127};
            for(unsigned y=0;y<(layout==3?4:8);++y) for(unsigned x=0;x<8;++x)
                std::memcpy(expected.data()+64+y*geometry.mips[0].pitchBytes+x*4,color.data(),4);
        }
        Require(allocation==expected&&neighbor==originalNeighbor,"storage admission or rejected upgrade changed publication permissions, padding, or guards");
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

// One two-slice array image bound through descriptors that each address part of its slices, as a
// title's dispatch does: element 0 names slice 0 (last array slice 0) and element 1 names slice 1
// (base and last slice 1). Both bind as views of one backing, and each view's writes reach its own
// guest slice.
void ArraySliceDescriptors(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    auto d=Descriptor(); d.dimension=Graphics::TextureDimension::k2DArray; d.depthOrLastArray=1;
    const auto geometry=Graphics::DescribeSurface(d);
    Require(geometry.layers==2,"two-slice array geometry has the wrong slice count");
    std::vector<std::byte> guest(geometry.guestBytes,std::byte{0});
    const NativeGuestMemory::BorrowedRange range{d.baseAddress,guest,true};
    Metal::MetalShaderResources resources(backend,std::span(&range,1));
    const auto words=[&](std::uint32_t baseArray,std::uint32_t lastArray) {
        return std::array<std::uint32_t,8>{static_cast<std::uint32_t>(d.baseAddress>>8),(d.format<<20)|(3u<<30),1u|(7u<<14),
            0xfacu|(13u<<28),lastArray|(baseArray<<16),0,0,0};
    };
    DescriptorBinding binding{};
    binding.kind=DescriptorKind::StorageImage; binding.role=DescriptorRole::GuestImages;
    binding.count=2; binding.imageShape=DescriptorImageShape::Image2DArray; binding.imageWritten={true,true};
    for(const auto& element:{words(0,0),words(1,1)}) binding.guestDescriptor.insert(binding.guestDescriptor.end(),element.begin(),element.end());
    MetalBackend::Result shader{}; shader.guest.bindings.push_back(binding);
    MetalBackend::ResourceMapping mapping{};
    mapping.count=2; mapping.kind=binding.kind; mapping.role=binding.role; mapping.texture=0; mapping.active=true;
    shader.resources.push_back(mapping);
    const auto bindings=resources.Bindings(shader);
    Require(bindings.size()==1&&bindings[0].textures.size()==2,"array slice descriptors did not bind both elements");
    const std::array<float,4> first{17/255.0f,31/255.0f,73/255.0f,127/255.0f},second{200/255.0f,100/255.0f,50/255.0f,1};
    auto commands=backend.CommandBuffer();
    Encode(backend,library,commands,@"writeArrayColor",bindings[0].textures[0],nil,MTLSizeMake(8,8,1),first.data(),sizeof first);
    Encode(backend,library,commands,@"writeArrayColor",bindings[0].textures[1],nil,MTLSizeMake(8,8,1),second.data(),sizeof second);
    backend.Wait(commands);
    Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"array slice writes faulted");
    for(std::uint32_t layer=0;layer<2;++layer) {
        const std::array<std::uint8_t,4> expected=layer==0?std::array<std::uint8_t,4>{17,31,73,127}:std::array<std::uint8_t,4>{200,100,50,255};
        for(std::uint32_t y=0;y<8;++y) {
            const auto* row=guest.data()+geometry.GuestLayerOffset(layer)+y*geometry.mips[0].pitchBytes;
            Pixels(row,expected,8,layer==0?"slice 0 of the array image lost its descriptor's writes":"slice 1 of the array image lost its descriptor's writes");
        }
    }
}

// A per-slice pass binds "array k-k" (k + 1 layers) of a four-layer array whose whole texture the
// image cache already holds: the view shares that backing, addresses only slice k, and its copy-back
// rewrites only slice k's guest bytes.
void ArrayPrefixViews(const Metal::MetalDevice& backend,id<MTLLibrary> library) {
    auto d=Descriptor(); d.dimension=Graphics::TextureDimension::k2DArray; d.depthOrLastArray=3;
    const auto geometry=Graphics::DescribeSurface(d);
    Require(geometry.layers==4,"four-slice array geometry has the wrong slice count");
    std::vector<std::byte> guest(geometry.guestBytes);
    for(std::uint32_t layer=0;layer<4;++layer) for(std::uint32_t y=0;y<8;++y) for(std::uint32_t x=0;x<8;++x) {
        const std::array<std::uint8_t,4> color{std::uint8_t(10+layer),std::uint8_t(20+layer),std::uint8_t(30+layer),255};
        std::memcpy(guest.data()+geometry.GuestLayerOffset(layer)+y*geometry.mips[0].pitchBytes+x*4,color.data(),4);
    }
    const NativeGuestMemory::BorrowedRange range{d.baseAddress,guest,true};
    Metal::GuestImageCache<Metal::MetalTexture> cache;
    {
        Metal::MetalShaderResources whole(backend,std::span(&range,1),{},nullptr,&cache);
        static_cast<void>(whole.Texture(d));
        auto commands=backend.CommandBuffer();
        backend.Wait(commands);
        Require(whole.Complete(commands).state==BdaAbi::FaultState::Empty,"whole array upload faulted");
    }
    auto expected=guest;
    auto slice=d; slice.baseArray=2; slice.depthOrLastArray=2;
    Metal::MetalShaderResources resources(backend,std::span(&range,1),{},nullptr,&cache);
    auto view=resources.Texture(slice,true);
    Require(view->GuestBytes()==geometry.guestBytes,"a view of the first layers did not share the cached whole array");
    Require(view->StorageView().arrayLength==1&&view->SampledView().arrayLength==1,"a one-slice view of a larger backing addresses other slices");
    const std::array<float,4> color{200/255.0f,100/255.0f,50/255.0f,1};
    auto commands=backend.CommandBuffer();
    Encode(backend,library,commands,@"writeArrayColor",view->StorageView(),nil,MTLSizeMake(8,8,1),color.data(),sizeof color);
    resources.EncodeReadbacks(commands);
    backend.Wait(commands);
    Require(resources.Complete(commands).state==BdaAbi::FaultState::Empty,"array slice view write faulted");
    for(std::uint32_t y=0;y<8;++y) for(std::uint32_t x=0;x<8;++x) {
        const std::array<std::uint8_t,4> written{200,100,50,255};
        std::memcpy(expected.data()+geometry.GuestLayerOffset(2)+y*geometry.mips[0].pitchBytes+x*4,written.data(),4);
    }
    Require(guest==expected,"the slice view's copy-back did not write exactly its slice");
    auto larger=d; larger.depthOrLastArray=4;
    Require(!view->CanShareBacking(larger),"a backing served a view with more layers than it has");
}

}

// Lazy copy-backs (MetalLazyCopyBacks.hpp) through the resources a draw or dispatch binds. A
// deferred work writes a surface and its copy-back is recorded, not run: the guest bytes keep their
// old contents. A later pass sampling the surface with a compatible descriptor gets the texture
// itself from the image cache and leaves it recorded; a pass viewing the bytes in a format the
// texture cannot share converts through the copied-back bytes, and a buffer binding of the bytes
// (a compute or DMA consumer) copies them back ahead of itself. Two writes of one texture merge.
void LazyCopyBacks(Metal::MetalDevice& backend,id<MTLLibrary> library) {
    if(!Metal::MetalLazyCopyBacks::Enabled()) return;
    auto d=Descriptor();
    const auto geometry=Graphics::DescribeSurface(d);
    // Copy-backs defer only into a no-copy alias of the guest bytes: page-aligned host memory.
    constexpr std::size_t mappingBytes=16384;
    void* mapping=::mmap(nullptr,mappingBytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    Require(mapping!=MAP_FAILED,"lazy copy-back host mapping failed");
    const std::span<std::byte> guest(static_cast<std::byte*>(mapping),geometry.guestBytes);
    std::fill(guest.begin(),guest.end(),std::byte{0x40});
    const NativeGuestMemory::BorrowedRange borrow{d.baseAddress,guest,true};
    Metal::MetalGuestHostAliases aliases(backend.Device());
    Metal::GuestImageCache<Metal::MetalTexture> cache;
    Metal::MetalLazyCopyBacks lazy;
    const auto untouched=[&] { return std::all_of(guest.begin(),guest.end(),[](std::byte b) { return b==std::byte{0x40}; }); };
    // The texels' guest bytes (a linear row may be padded past them) hold `color`.
    const auto holds=[&](std::array<std::uint8_t,4> color) {
        const auto& mip=geometry.mips[0];
        for(unsigned y=0;y<mip.height;++y) for(unsigned x=0;x<mip.width;++x) for(unsigned c=0;c<4;++c)
            if(guest[y*mip.pitchBytes+x*4+c]!=std::byte{color[c]}) return false;
        return true;
    };
    // A deferred pass writing the whole surface in `color`; returns its texture.
    const auto write=[&](std::array<std::uint8_t,4> color) {
        auto pass=std::make_shared<Metal::MetalShaderResources>(backend,std::span(&borrow,1),Metal::MetalShaderResources::DepthTextureLookup{},&aliases,&cache);
        pass->SetLazyCopyBacks(&lazy);
        auto target=pass->Texture(d,true);
        auto commands=backend.CommandBuffer();
        const std::array<std::uint32_t,4> value{color[0],color[1],color[2],color[3]};
        Encode(backend,library,commands,@"writeColor2D",target->StorageView(),nil,MTLSizeMake(8,8,1),value.data(),sizeof(value));
        pass->EncodeReadbacks(commands,true);
        Require(!pass->NeedsSynchronousCompletion(),"a deferred copy-back makes its work complete synchronously");
        pass->CommittedWithoutWaiting();
        backend.Wait(commands);
        static_cast<void>(pass->Complete(commands));
        return target;
    };
    lazy.TakeStatistics();
    const auto first=write({17,31,73,127});
    Require(lazy.Size()==1,"a written surface's copy-back is not deferred");
    Require(untouched(),"a deferred copy-back wrote the guest bytes before anyone read them");
    write({17,31,73,127});
    const auto merged=lazy.TakeStatistics(false);
    Require(lazy.Size()==1 && merged.recorded==2 && merged.merged==1,"two copy-backs of one texture are not merged");
    {
        // A later pass samples the surface: the texture itself, no copy-back.
        Metal::MetalShaderResources pass(backend,std::span(&borrow,1),{},&aliases,&cache);
        pass.SetLazyCopyBacks(&lazy);
        auto sampled=pass.Texture(d);
        Require(sampled->Texture()==first->Texture(),"a compatible sampler of a deferred surface does not get its texture");
        auto output=backend.Buffer(64*4);
        auto commands=backend.CommandBuffer();
        Encode(backend,library,commands,@"read2D",sampled->SampledView(),output,MTLSizeMake(8,8,1));
        backend.Wait(commands);
        Pixels(output.contents,{17,31,73,127},64,"a sampler of a deferred surface does not see what the earlier pass rendered");
        Require(lazy.Size()==1 && untouched(),"sampling a deferred surface through its texture copied it back");
    }
    {
        // Another format the texture cannot share: converted through the copied-back bytes, on the GPU.
        Metal::MetalShaderResources pass(backend,std::span(&borrow,1),{},&aliases,&cache);
        pass.SetLazyCopyBacks(&lazy);
        auto words=Descriptor(20);
        auto converted=pass.Texture(words);
        Require(converted->Texture()!=first->Texture() && lazy.Empty(),"an incompatible view of a deferred surface does not copy it back first");
        auto output=backend.Buffer(64*4);
        auto commands=backend.CommandBuffer();
        Encode(backend,library,commands,@"readR32Uint",converted->SampledView(),output,MTLSizeMake(8,8,1));
        backend.Wait(commands);
        for(unsigned i=0;i<64;++i) Require(static_cast<const std::uint32_t*>(output.contents)[i]==0x7f491f11u,"an incompatible view of a deferred surface does not see what the earlier pass rendered");
        Require(holds({17,31,73,127}),"the copy-back before an incompatible view did not reach the guest bytes");
    }
    std::fill(guest.begin(),guest.end(),std::byte{0x40});
    cache.Clear();
    write({5,6,7,8});
    Require(lazy.Size()==1 && untouched(),"a rewritten surface's copy-back is not deferred");
    {
        // A buffer binding of the bytes (a compute or DMA consumer): copied back first, GPU-side.
        Metal::MetalShaderResources pass(backend,std::span(&borrow,1),{},&aliases,&cache);
        pass.SetLazyCopyBacks(&lazy);
        // Row 1's texels.
        auto bytes=pass.Buffer(d.baseAddress+geometry.mips[0].pitchBytes,32);
        Require(lazy.Empty(),"a buffer binding over a deferred surface does not copy it back first");
        auto output=backend.Buffer(64);
        auto commands=backend.CommandBuffer();
        auto encoder=[commands computeCommandEncoder];
        [encoder setComputePipelineState:Pipeline(backend,library,@"copyWords")];
        [encoder setTexture:first->SampledView() atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:bytes.buffer offset:bytes.offset atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(8,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [encoder endEncoding];
        backend.Wait(commands);
        Pixels(output.contents,{5,6,7,8},8,"a buffer reader of a deferred surface does not see what the earlier pass rendered");
        Require(holds({5,6,7,8}),"the copy-back before a buffer reader did not reach the guest bytes");
    }
    // Recorded copy-backs over another texture's bytes stay disjoint: the older one is encoded first.
    std::fill(guest.begin(),guest.end(),std::byte{0x40});
    cache.Clear();
    write({1,2,3,4});
    auto commands=backend.CommandBuffer();
    std::vector<Metal::MetalLazyCopyBacks::Materialized> encodedFirst;
    auto other=std::make_shared<Metal::MetalTexture>(backend,Descriptor(20));
    auto alias=backend.Buffer(guest.size());
    Require(!lazy.Record(commands,other,alias,2,{},guest,d.baseAddress,encodedFirst),"a copy-back that is not in place is deferred");
    Require(lazy.Size()==1,"a refused record changed the recorded copy-backs");
    backend.Wait(commands);
    commands=backend.CommandBuffer();
    auto done=lazy.EncodeAll(commands,Metal::MetalLazyCopyBacks::Cause::Drain);
    backend.Wait(commands);
    Require(done.size()==1 && lazy.Empty() && holds({1,2,3,4}),"encoding every recorded copy-back does not reach the guest bytes");
    // A binding that encodes a copy-back and is then dropped (a working-set retry) leaves it on no
    // timeline entry: the registry still names its command buffer until it completed.
    write({9,9,9,9});
    backend.SetBatching(true);
    {
        Metal::MetalShaderResources pass(backend,std::span(&borrow,1),{},&aliases,&cache);
        pass.SetLazyCopyBacks(&lazy);
        static_cast<void>(pass.Buffer(d.baseAddress,32));
    }
    auto unfinished=lazy.Unfinished(guest.data(),guest.size());
    Require(lazy.Empty() && unfinished.size()==1 && backend.OpenBatch(unfinished[0]),"a dropped reader's copy-back is not tracked until it completes");
    Require(lazy.Unfinished(guest.data()+guest.size(),16).empty(),"an unrelated range waits for a dropped reader's copy-back");
    backend.Wait(unfinished[0]);
    backend.SetBatching(false);
    Require(lazy.Unfinished().empty() && holds({9,9,9,9}),"a completed copy-back is still tracked, or did not reach the guest bytes");
    // The mapping stays: the aliases and cached textures above still name it until they go.
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
    DccMetadataViews(backend,library);
    PhysicalAllowed(backend,library);
    PhysicalGuards(backend);
    PhysicalBdaGuards(backend,library);
    StoragePermissions(backend,library);
    ArraySliceDescriptors(backend,library);
    ArrayPrefixViews(backend,library);
    AccessGuards(backend);
    LazyCopyBacks(backend,library);
}
