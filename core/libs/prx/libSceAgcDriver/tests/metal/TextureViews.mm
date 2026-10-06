#include "MetalTestSupport.hpp"
#include "MetalShaderResources.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>
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
kernel void readR32Float(texture2d<float,access::read> t [[texture(0)]],device uint* out [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { out[p.y*8+p.x]=as_type<uint>(t.read(p).x); }
struct BdaHeader { uint version,count,entryBytes,reserved; };
struct BdaRange { ulong begin,end; device uchar* address; uint permissions,reserved; };
kernel void physicalStore(device const BdaHeader* header [[buffer(0)]],device atomic_uint* fault [[buffer(1)]],constant ulong& address [[buffer(2)]]) {
    device const BdaRange* ranges=reinterpret_cast<device const BdaRange*>(header+1);
    for(uint i=0;i<header->count;++i) {
        if(address<ranges[i].begin||address>=ranges[i].end||4>ranges[i].end-address||(ranges[i].permissions&2)==0) continue;
        *reinterpret_cast<device uint*>(ranges[i].address+address-ranges[i].begin)=0x9172a3b4;
        uint page=uint(address>>12)+1u;
        atomic_store_explicit(fault+9+(((page*0x9e3779b1u)>>20)&4095),page,memory_order_relaxed);
        return;
    }
}
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
    PhysicalAllowed(backend,library);
    PhysicalGuards(backend);
    PhysicalBdaGuards(backend,library);
    AccessGuards(backend);
}
