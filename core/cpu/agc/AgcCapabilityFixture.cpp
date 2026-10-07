#include <cpu/SceAgcImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

// Contract: even an explicitly selected ABI must not resolve an event gate
// without the CPU event owner. Regression: resolution succeeds and fails only
// after guest invocation. Existing native fixture excludes the event contracts;
// this is the owning resolution boundary, with no fake event-success callback.
int main() {
    try {
        Cpu::Machine machine;
        constexpr std::array contracts{Cpu::AgcAbiContract::AddEqEvent, Cpu::AgcAbiContract::DeleteEqEvent};
        Cpu::SceAgcImports imports(machine, {}, contracts);
        for (const auto* nid : {"w2rJhmD+dsE", "DL2RXaXOy88"}) {
            Cpu::SceImport item;
            item.Nid = nid; item.LibraryName = item.ModuleName = "libSceAgcDriver";
            item.LibraryVersion = item.ModuleMajor = item.ModuleMinor = 1;
            bool rejected = false;
            try { (void)imports.Resolve(item, 2, 0); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("native backend contract is unavailable") != std::string::npos;
                if (!rejected) throw;
            }
            if (!rejected) throw std::runtime_error("unavailable event backend resolved a callable gate");
        }
        std::cout << "Add/Delete ABI selected, genuine event owner absent: resolution rejected both before gate allocation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
