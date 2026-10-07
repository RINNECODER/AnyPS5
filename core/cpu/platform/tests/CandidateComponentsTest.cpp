#include "../CandidateComponents.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F&& f, const char* message) {
    try { f(); } catch (const std::exception& e) {
        require(std::string_view(e.what()).find(message) != std::string_view::npos, e.what());
        return;
    }
    throw std::runtime_error("Expected explicit candidate registration failure");
}
// Contract: default assembly admits no unqualified ABI or guest mapping; explicit
// selection binds only that component and keeps observed symbol type mandatory.
// Regression: auto-enabling all candidates or dispatching with an assumed FUNC
// type bypasses qualification. Individual providers cannot catch assembly mistakes.
// This uses the intended registration API, without a test-only production seam.
void registrationBoundary() {
    Cpu::Machine machine;
    Cpu::Platform::CandidateComponents defaults(machine);
    Cpu::SceImport uri;
    uri.Nid = "YuOW3dDAKYc";
    uri.LibraryName = uri.ModuleName = "libSceHttp";
    uri.LibraryVersion = uri.ModuleMajor = uri.ModuleMinor = 1;
    require(!defaults.Resolve(uri, 2) && machine.Mappings().empty(), "Default assembly admitted a candidate");
    using Family = Cpu::Platform::CandidateFamily;
    for (auto family : {Family::Kernel, Family::Content, Family::Np}) {
        Cpu::Platform::CandidateComponents::Configuration invalid;
        invalid.PublicAbiFixtures.insert(family);
        rejects([&]{ Cpu::Platform::CandidateComponents value(machine, invalid); }, "needs");
        require(machine.Mappings().empty(), "Invalid ownership configuration changed guest mappings");
    }
    Cpu::Platform::CandidateComponents::Configuration selected;
    selected.PublicAbiFixtures.insert(Family::Network);
    Cpu::Platform::CandidateComponents network(machine, selected);
    require(network.Resolve(uri, 2).has_value(), "Explicit network component was not bound");
    rejects([&]{ network.Resolve(uri, 1); }, "symbol type");
    auto other = uri; other.Nid = "WJ3rqFwymew"; other.LibraryName = other.ModuleName = "libSceRtc";
    require(!network.Resolve(other, 2), "Assembly enabled an unselected RTC component");
}
}
int main() {
    try { registrationBoundary(); std::cout << "PASS explicit candidate registration boundary\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
