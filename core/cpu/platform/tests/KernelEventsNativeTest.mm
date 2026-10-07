#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "KernelEventsSession.hpp"
#include "NativeAgcBackend.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <atomic>

// Test-audit: NativeAgcFixture already proves real Metal EOP interrupt counts,
// but cannot detect whether a translated equeue waiter ever resumes. This test
// combines its actual backend/PM4 EOP with the queue guest and production owner
// pump. No mock completion, manual pump or new target AGC admission is used.
int main(int argc,char** argv) {
    using namespace EventFixture;
    @autoreleasepool {
        try {
            require(argc==3,"Usage: KernelEventsNativeTest kernel-events.elf utility.metallib");
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,"Native event test requires Metal device");
            NSError* error=nil;
            id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]] error:&error];
            require(library!=nil,"Native event test requires existing utility metallib");
            alignas(65536) std::array<std::uint32_t,16384> commands{},output{};
            output.fill(0xdeadbeef);
            const std::array<std::uint32_t,8> eop{0xc0064900,0,(1u<<29)|(1u<<24),0x510100,0,0xabcdef12,0,0};
            std::copy(eop.begin(),eop.end(),commands.begin());
            const std::array ranges{
                AgcDriver::NativeGuestMemory::BorrowedRange{0x500000,std::as_writable_bytes(std::span(commands)).first(4096),false},
                AgcDriver::NativeGuestMemory::BorrowedRange{0x510000,std::as_writable_bytes(std::span(output)).first(4096),true}};
            AgcDriver::Metal::MetalDriver driver;
            Session s(argv[1],14);
            const auto publish=s.queues->EopPublisher();
            std::atomic<unsigned> callbacks{0};std::atomic<bool> producerOwner{false};
            driver.Configure((__bridge void*)device,(__bridge void*)library,ranges,
                [publish,&callbacks,&producerOwner,owner=s.owner](std::uint32_t id){
                    producerOwner=std::this_thread::get_id()==owner;
                    publish(id);++callbacks;
                });
            struct Drain {
                AgcDriver::Metal::MetalDriver& driver;
                ~Drain(){driver.Shutdown();}
            } drain{driver};
            auto backend=Cpu::MakeNativeAgcBackend(driver);
            backend.AddEvent=[&](std::uint64_t handle,std::int32_t id,std::uint64_t user){
                return s.queues->AddGraphicsEvent(handle,id,user);
            };
            backend.DeleteEvent=[&](std::uint64_t handle,std::int32_t id){return s.queues->DeleteGraphicsEvent(handle,id);};
            bool registered=false,submitted=false;
            s.boundary=[&](bool waiting){
                const auto r=s.state();
                if(r[2] && !registered) {
                    require(backend.AddEvent(r[2],0x20,0x8877665544332211ULL)==0,"Native callback wiring rejected numeric queue");
                    registered=true;
                }
                if(waiting && !submitted) {
                    require(r[8]==2 && r[5]==0 && r[32] && s.get(r[32])==r[33],
                            "Native EOP oracle did not reach real blocked guest");
                    backend.Submit(0x500000,8,0,0x20);submitted=true;
                }
            };
            s.finish();driver.WaitIdle();
            const auto r=s.state();
            require(submitted && s.idleBoundaries && callbacks==1 && !producerOwner && r[34]==1 && r[10]==0,
                    "Actual asynchronous native EOP did not wake translated guest on idle owner");
            require(r[50]==0x8002003c && r[51]==0,"Actual native deferred EOP did not clear its pending count");
            record(r,64,0x20,1,0x8877665544332211ULL);
            record(r,84,0x20,1,0x8877665544332211ULL);
            require(output[64]==0xabcdef12,"Actual Metal EOP failed independent GPU memory write");
            for(unsigned i=0;i<output.size();++i)if(i!=64)require(output[i]==0xdeadbeef,"Native EOP overran GPU output guard");
            driver.Shutdown();s.queues->Shutdown();publish(0x20);
            std::cout<<"PASS actual synthetic AGC backend -> native Metal EOP completion thread -> retained queue receipt -> idle persistent owner -> translated guest wake; game_runtime_ready=false\n";
        }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
    }
}
