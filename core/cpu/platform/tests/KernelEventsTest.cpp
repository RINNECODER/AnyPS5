#include "KernelEventsSession.hpp"
#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <vector>

// Test-audit: actual linked guest park/resume and independently authored byte
// records detect no-wake, reset, width, lifetime and continuation regressions.
// Mutex/priority controls have no queues or asynchronous receipts. This uses
// the production resolver, owner boundary and retained EOP publisher.
using namespace EventFixture;
namespace {
constexpr std::uint64_t User=0x1122334455667788ULL;
void asynchronous(const char* file) {
    Session s(file,0); bool registered=false,published=false,republished=false;
    Cpu::Platform::KernelEvents foreign(s.machine,s.threads,0x7ffdc3000000);
    auto publish=s.queues->EopPublisher();
    s.boundary=[&](bool waiting){
        const auto r=s.state();
        if(r[2] && !registered) {
            require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0,"Owned graphics subscription rejected");
            registered=true;
            rejects([&]{foreign.AddGraphicsEvent(r[2],0x20,User);},"invalid event queue");
            std::atomic<bool> denied{false};
            std::thread thief([&]{try{s.queues->AddGraphicsEvent(r[2],0x20,~User);}
                catch(const std::exception& error){denied=std::string(error.what()).find("owner thread")!=std::string::npos;}});
            thief.join();require(denied,"Non-owner changed a live graphics subscription");
        }
        if(waiting && !published) {
            require(r[8]==2 && r[9] && r[4]!=r[9] && r[5]==0 && r[32] && s.get(r[32])==r[33],
                    "Wait did not really park while another translated guest thread progressed");
            const auto before=s.page;
            // A completion thread has only a retained producer. Owner remains
            // inside its idle boundary until the producer returns, so any guest
            // write from the producer is independently visible here.
            std::thread producer([publish]{publish(0x20);publish(0x20);publish(0x20);});
            producer.join();
            require(s.page==before,"Completion thread wrote guest records/count before owner delivery");
            published=true;
        }
        if(r[3]==3 && !republished){publish(0x20);republished=true;}
    };
    s.finish();const auto r=s.state();
    require(published && s.idleBoundaries && r[10]==0 && r[34]==1 && r[31]==0 && r[35]==0 &&
            r[36]==0x5566778899aabbccULL && r[5]==1,"Asynchronous guest wait/join ABI or payload differs");
    record(r,64,0x20,3,User);
    require(republished && r[50]==0x8002003c && r[51]==0 && r[52]==0 && r[53]==1,
            "Deferred EV_CLEAR delivery retained old count or lost fresh publication");
    record(r,80,0x20,1,User);
    record(r,84,0x20,3,User);
    std::cout<<"PASS translated guest parks, another guest progresses, completion-thread receipts coalesce and owner resumes ABI\n";
}
void deletedWait(const char* file) {
    Session s(file,7);s.finish(true);
    const auto r=s.state();
    require(r[10]==0x80020009 && r[34]==0x80020009 && r[43]==0 && r[5]==1,
            "Guest DeleteEqueue did not resume blocked Wait with source EBADF/count4");
    for(unsigned i=16;i<80;++i)require(s.page[i]==std::byte{0xa5},"Deleted queue wrote stale event records");
    std::cout<<"PASS actual guest DeleteEqueue wakes parked guest with EBADF/count4 and no records\n";
}
void queuedDelete(const char* file) {
    Session s(file,10);bool registered=false,published=false;auto publish=s.queues->EopPublisher();
    const auto attr=s.address+120*8,param=s.address+121*8;s.put(param,256);
    require(s.threads->AttributeInit(attr)==0 && s.threads->AttributeSetPriority(attr,param)==0 &&
            s.threads->AttributeSetInherit(attr,0)==0,"Queued-delete child priority setup rejected");
    s.boundary=[&](bool waiting){
        const auto r=s.state();
        if(r[2] && r[44] && !registered) {
            require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0 &&
                    s.queues->AddGraphicsEvent(r[44],0x21,~User)==0,"Queued-delete subscriptions rejected");registered=true;
        }
        if(waiting && !published){require(registered && r[5]==0 && r[8]==1,"Two guest queue waiters did not park");
            publish(0x20);publish(0x21);published=true;}
    };
    s.finish(true);const auto r=s.state();
    require(published && r[10]==0x80020009 && r[34]==0x80020009 && r[43]==0 && r[45]==0 && r[46]==0 && r[47]==0,
            "Higher priority guest delete did not withdraw queued lower priority receipt before output");
    for(unsigned i=16;i<80;++i)require(s.page[i]==std::byte{0xa5},"Queued deleted receipt wrote stale records");
    std::cout<<"PASS two parked guests, actual priority selection, Delete racing reserved queued wake yields EBADF/count4 without stale delivery\n";
}
// Successful source DeleteEqEvent withdraws a reserved subscription receipt;
// the queue and suspended Wait remain live while unrelated guest code runs.
void reservedSubscriptionDelete(const char* file) {
    Session s(file,15);bool registered=false,published=false,reparked=false;
    auto publish=s.queues->EopPublisher();
    const auto attr=s.address+120*8,param=s.address+121*8;s.put(param,256);
    require(s.threads->AttributeInit(attr)==0 && s.threads->AttributeSetPriority(attr,param)==0 &&
            s.threads->AttributeSetInherit(attr,0)==0,"Subscription-delete child priority setup rejected");
    s.boundary=[&](bool waiting){
        const auto r=s.state();
        if(r[2] && r[44] && !registered) {
            require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0 &&
                    s.queues->AddGraphicsEvent(r[2],0x21,~User)==0 &&
                    s.queues->AddGraphicsEvent(r[44],0x22,User)==0,
                    "Reserved subscription-delete registration rejected");registered=true;
        }
        if(waiting && !published) {
            require(registered && r[5]==0 && r[8]==1,"Subscription-delete guests did not both park");
            publish(0x20);publish(0x22);published=true;
        } else if(waiting && published && !reparked) {
            require(r[43]==0 && r[8]==2 && r[45]==0 && r[46]==0 && r[5]==0 &&
                    r[10]==0 && s.get(r[32])==r[33],
                    "Reserved subscription deletion stopped unrelated progress or completed its live wait");
            for(unsigned i=16;i<80;++i)require(s.page[i]==std::byte{0xa5},"Retracted receipt wrote stale records");
            for(unsigned i=128;i<132;++i)require(s.page[i]==std::byte{0xa5},"Retracted receipt wrote count");
            publish(0x20); // Removed subscription has no new receipt.
            publish(0x21); // The original pending Wait can receive a live event.
            reparked=true;
        }
    };
    s.finish(false,"Successful reserved subscription deletion terminally cancelled live guest threads");
    const auto r=s.state();
    require(published && reparked && r[10]==0 && r[34]==1 && r[5]==1 && r[35]==0 &&
            r[36]==0x5566778899aabbccULL && r[43]==0 && r[8]==2,
            "Retracted live wait did not resume on a fresh surviving subscription");
    record(r,64,0x21,1,~User);
    std::cout<<"PASS reserved subscription source DeleteEqEvent keeps pending wait live, unrelated guest progresses, fresh EOP resumes without stale records\n";
}
void subscriptionRace(const char* file) {
    Session s(file,9);bool registered=false,published=false;auto publish=s.queues->EopPublisher();
    s.boundary=[&](bool waiting){
        const auto r=s.state();
        if(r[2] && !registered){require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0,"Race subscription rejected");registered=true;}
        if(waiting && !published) {
            std::thread racing([publish]{for(unsigned i=0;i<100;++i)publish(0x20);});
            require(s.queues->DeleteGraphicsEvent(r[2],0x20)==0,"Race deletion rejected");racing.join();
            publish(0x20); // Deleted identity cannot retain a receipt.
            require(s.queues->AddGraphicsEvent(r[2],0x20,~User)==0,"Fresh generation subscription rejected");
            publish(0x20);published=true;
        }
    };
    s.finish();require(published && s.state()[34]==1,"Delete/publish race did not return exactly one live event");
    record(s.state(),64,0x20,1,~User);
    std::cout<<"PASS concurrent publisher/subscription deletion discards old receipts before same-id new subscription\n";
}
void memoryAndIdleBound(const char* file) {
    for(unsigned mode:{11U,12U}) {
        Session s(file,mode);bool registered=false,published=false;auto publish=s.queues->EopPublisher();
        s.boundary=[&](bool waiting){
            const auto r=s.state();
            if(r[2] && !registered){require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0,"Memory control subscription rejected");registered=true;}
            if(waiting && mode==11 && !published){s.machine.Protect(PageAddress,4096,Cpu::Permission::Read);publish(0x20);published=true;}
        };
        const auto before=s.page;
        rejects([&]{s.graph->RunMain(1000000,10000);},"Guest access denied");
        require(s.page==before && s.state()[0]==0 && s.state()[5]==0,
                "Unmapped initial/revoked deferred output partially wrote count/records or resumed guest");
    }
    {
        Session s(file,13);const auto before=s.page;unsigned boundaries=0;
        s.threads->SetOwnerBoundary([&](bool){s.threads->CheckIdleOwner();++boundaries;},std::chrono::milliseconds(20));
        const auto begin=std::chrono::steady_clock::now();
        require(s.graph->RunMain(1000000,10000)==Cpu::StopReason::Requested,
                "Diagnostic idle cancellation bound manufactured a guest timeout/success");
        const auto elapsed=std::chrono::steady_clock::now()-begin;
        require(boundaries>2 && elapsed>=std::chrono::milliseconds(20) && elapsed<std::chrono::milliseconds(200) &&
                s.page==before && s.state()[5]==0,"Finite idle owner cancellation failed continued pump or wrote outputs");
    }
    std::cout<<"PASS unmapped initial/revoked deferred guest spans preserve outputs, finite diagnostic idle cap stays cancelable\n";
}
// A game idles briefly on every frame. The idle cap measures one continuous
// idle stretch: two separate 80 ms waits add up to more than the 150 ms cap,
// but neither stretch reaches it, so the guest must run to its normal exit.
void continuousIdleBound(const char* file) {
    using Clock=std::chrono::steady_clock;
    constexpr auto stretch=std::chrono::milliseconds(80),cap=std::chrono::milliseconds(150);
    Session s(file,2);unsigned stage=0;auto publish=s.queues->EopPublisher();
    std::optional<Clock::time_point> since;std::vector<Clock::duration> stretches;
    s.threads->SetOwnerBoundary([&](bool waiting){
        s.threads->CheckIdleOwner();
        const auto now=Clock::now();
        if(!waiting){since.reset();return;}
        if(!since)since=now;
        if(now-*since<stretch)return;
        const auto r=s.state();
        if(r[3]==1 && stage==0) {
            require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0 &&
                    s.queues->AddGraphicsEvent(r[2],0,~User)==0 &&
                    s.queues->AddGraphicsEvent(r[2],0x21,0x8877665544332211ULL)==0,
                    "Idle-stretch subscriptions rejected");
            publish(0x20);publish(0x20);publish(0x20);publish(0);publish(0);publish(0x21);
            require(s.queues->AddGraphicsEvent(r[2],0x20,User+1)==0,"Idle-stretch duplicate Add rejected");
        } else if(r[3]==2 && stage==1) publish(0x20);
        else return;
        ++stage;stretches.push_back(now-*since);since.reset();
    },cap);
    s.finish(false,"Separate short idle stretches were cancelled as one cumulative idle budget");
    const auto r=s.state();
    require(stage==2 && stretches.size()==2 && r[24]==2 && r[26]==1 && r[30]==1,
            "Idle-stretch guest did not consume both delayed event batches");
    require(stretches[0]>=stretch && stretches[1]>=stretch && stretches[0]<cap && stretches[1]<cap &&
            stretches[0]+stretches[1]>=cap,"Idle stretches did not exceed the cap only in total");
    std::cout<<"PASS idle cap resets on guest progress: two 80 ms waits under a 150 ms continuous idle cap\n";
}
// Zero disables idle cancellation. Only the host boundary's own stop ends a
// guest that waits forever, long after any small idle cap would have fired.
void unlimitedIdle(const char* file) {
    using Clock=std::chrono::steady_clock;
    Session s(file,13);const auto before=s.page;bool stopped=false;
    std::optional<Clock::time_point> since;
    s.threads->SetOwnerBoundary([&](bool waiting){
        s.threads->CheckIdleOwner();
        if(!waiting){since.reset();return;}
        if(!since)since=Clock::now();
        if(!stopped && Clock::now()-*since>=std::chrono::milliseconds(200)){stopped=true;s.machine.RequestStop();}
    },std::chrono::milliseconds(0));
    require(s.graph->RunMain(1000000,10000)==Cpu::StopReason::Requested && stopped,
            "Unlimited idle owner stopped before its host boundary requested it");
    require(s.page==before && s.state()[5]==0,"Unlimited idle owner stop wrote outputs or resumed the waiter");
    std::cout<<"PASS zero idle cap is unlimited: only the host boundary stop ends an indefinite wait\n";
}
void timeouts(const char* file) {
    Session s(file,1); const auto begin=std::chrono::steady_clock::now();s.finish();
    const auto elapsed=std::chrono::steady_clock::now()-begin;const auto r=s.state();
    require(r[17]==0x8002003c && r[18]==0 && r[10]==0x8002003c && r[19]==0 &&
            r[20]==0xfeedbeef00001388ULL && r[21]==0x80020016 && r[23]==0x80020016,
            "Poll/finite timeout, immutable timeout4 or invalid capacity status differs");
    require(elapsed>=std::chrono::milliseconds(5) && elapsed<std::chrono::milliseconds(500),
            "Finite microsecond timeout did not genuinely expire within diagnostic cap");
    for(unsigned i=16;i<80;++i)require(s.page[i]==std::byte{0xa5},"Empty timeout invented event records");
    std::cout<<"PASS zero poll, genuine finite monotonic timeout and four-byte immutable input/count\n";
}
void capacity(const char* file) {
    Session s(file,2);unsigned stage=0;auto publish=s.queues->EopPublisher();
    s.boundary=[&](bool){
        const auto r=s.state();
        if(r[3]==1 && stage==0) {
            require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0 &&
                    s.queues->AddGraphicsEvent(r[2],0,~User)==0 &&
                    s.queues->AddGraphicsEvent(r[2],0x21,0x8877665544332211ULL)==0,
                    "Multiple owned subscriptions rejected");
            publish(0x20);publish(0x20);publish(0x20);publish(0);publish(0);publish(0x21);
            // Source duplicate Add preserves pending data and updates udata.
            require(s.queues->AddGraphicsEvent(r[2],0x20,User+1)==0,"Duplicate Add rejected");stage=1;
        } else if(r[3]==2 && stage==1) {publish(0x20);stage=2;}
    };
    s.finish();const auto r=s.state();
    require(stage==2 && r[24]==2 && r[25]==0 && r[26]==1 && r[27]==0x8002003c &&
            r[28]==0 && r[29]==0 && r[30]==1,"Event capacity/unread pending/reset semantics differ");
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>> expected{
        {0x20,{3,User+1}},{0,{2,~User}},{0x21,{1,0x8877665544332211ULL}}};
    for(unsigned slot:{64U,68U,72U}) {
        auto found=expected.find(r[slot]);require(found!=expected.end(),"Capacity returned duplicate/unknown identity");
        record(r,slot,found->first,found->second.first,found->second.second);expected.erase(found);
    }
    require(expected.empty(),"Capacity lost an unread pending event");record(r,76,0x20,1,User+1);
    std::cout<<"PASS capacity2 retains third event, duplicate Add preserves coalesced count, EV_CLEAR resets before next EOP\n";
}
void cancellation(const char* file,unsigned mode) {
    Session s(file,mode);bool fired=false,registered=false;auto publish=s.queues->EopPublisher();
    s.boundary=[&](bool waiting){
        const auto r=s.state();
        if(r[2] && !registered) {
            require(s.queues->AddGraphicsEvent(r[2],0x20,User)==0,"Cancellation subscription rejected");registered=true;
        }
        if(waiting && !fired) {
            require(r[5]==0 && r[32] && s.get(r[32])==r[33],"Cancellation lacked real suspended frame");
            fired=true;
            if(mode==3) {
                publish(0x20);
                s.machine.RequestStop();
            } else if(mode==4) {s.put(r[32],0x1122334455667788ULL);publish(0x20);}
            else if(mode==5) s.machine.RequestStop();
            else s.machine.RequestStop();
        }
    };
    const auto before=s.page;
    if(mode==4) rejects([&]{s.graph->RunMain(1000000,10000);},"return word changed");
    else require(s.graph->RunMain(1000000,10000)==Cpu::StopReason::Requested,"Cancelled wait executed success");
    require(fired && s.state()[0]==0 && s.state()[5]==0 && s.page==before,
            "Delete/cancel/corrupt continuation fabricated event output or guest completion");
    // Root stops and returns from the executor before provider teardown. Domain
    // withdrawal while the public scheduler drive is active remains rejected.
    if(mode==3)s.queues->Shutdown();
    if(mode==6){s.withdraw();s.queues.reset();}
    std::thread retained([publish]{for(unsigned i=0;i<100;++i)publish(0x20);});retained.join();
    require(s.page==before,"Retained publisher delivered after cancellation/withdrawal");
    Cpu::GuestPhaseBudget budget(10000);
    if(mode!=6)require(s.threads->RunEntry(budget)==Cpu::StopReason::Requested && budget.Consumed()==0,
                      "Cancelled event continuation survived terminal stop");
    std::cout<<"PASS event cancellation mode "<<mode<<" preserves outputs and terminal continuation\n";
}
void admission() {
    Cpu::Machine m;auto t=std::make_shared<Cpu::GuestThreads>(m);
    constexpr Cpu::Platform::KernelEventConsumer eboot{"eboot.bin","a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397"};
    {
        Cpu::Platform::TargetKernelEvents target(m,t);
        for(auto nid:{"D0OdFMjp46I","jpFjmgAC5AE","fzyMKs9kim0"}) {
            auto row=scoped(nid);row.LibraryId=44;row.ModuleId=24;
            rejects([&]{target.Resolve(row,2,0,eboot);},"not selected");
        }
    }
    const auto selected=Cpu::Platform::QualifiedKernelEventAdmissionsForImage(eboot.Sha256);
    require(selected.size()==3 && Cpu::Platform::QualifiedKernelEventAdmissionsForImage("unknown").empty(),"Source-bound event selection widened");
    {
    Cpu::Platform::TargetKernelEvents target(m,t,selected);
    for(auto nid:{"D0OdFMjp46I","jpFjmgAC5AE","fzyMKs9kim0"}) {
        auto row=scoped(nid);row.LibraryId=44;row.ModuleId=24;
        require(target.Resolve(row,2,0,eboot).has_value(),"Source-bound exact target event rejected");
        rejects([&]{target.Resolve(row,2,0,{"eboot.bin","unknown"});},"consumer/import row");
        rejects([&]{target.Resolve(row,2,0,{"other.bin",eboot.Sha256});},"consumer/import row");
        for(unsigned field=0;field<8;++field) {
            auto wrong=row;
            if(field==0)wrong.LibraryName="libc";
            if(field==1)wrong.ModuleName="libc";
            if(field==2)wrong.LibraryVersion=2;
            if(field==3)wrong.ModuleMajor=2;
            if(field==4)wrong.ModuleMinor=2;
            if(field==5)wrong.LibraryId=0;
            if(field==6)wrong.ModuleId=1;
            rejects([&]{target.Resolve(wrong,field==7?1:2,0,eboot);},field==5||field==6?"consumer/import row":"scope/version/type/size");
        }
        rejects([&]{target.Resolve(row,2,8,eboot);},"scope/version/type/size");
    }
    target.Provider().Shutdown();t->Withdraw();
    }
    t.reset();
    {
        auto scheduler=std::make_shared<Cpu::GuestThreads>(m);
        auto provider=std::make_unique<Cpu::Platform::KernelEvents>(m,scheduler);
        const auto retained=provider->EopPublisher();scheduler.reset();provider.reset();retained(0x20);
    }
    std::cout<<"PASS default denied, source-bound exact three-route profile, identity/type/size/domain/owner rejection and expired scheduler shutdown\n";
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2 || (argc==3 && (std::string(argv[2])=="--queued-delete" || std::string(argv[2])=="--subscription-delete")),
                "Usage: KernelEventsTest kernel-events.elf [--queued-delete|--subscription-delete]");
        if(argc==3){if(std::string(argv[2])=="--queued-delete")queuedDelete(argv[1]);else reservedSubscriptionDelete(argv[1]);return 0;}
        asynchronous(argv[1]);timeouts(argv[1]);capacity(argv[1]);deletedWait(argv[1]);queuedDelete(argv[1]);reservedSubscriptionDelete(argv[1]);subscriptionRace(argv[1]);
        for(unsigned mode:{3U,4U,5U,6U})cancellation(argv[1],mode);
        memoryAndIdleBound(argv[1]);continuousIdleBound(argv[1]);unlimitedIdle(argv[1]);
        admission();
    } catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
