#ifndef CLI_CONSOLEPROGRESS_HPP
#define CLI_CONSOLEPROGRESS_HPP

#include <domain/IProgress.hpp>
#include <ostream>
#include <stdexcept>

namespace Cli {

class ConsoleProgress final : public Domain::IProgress {
public:
    explicit ConsoleProgress(std::ostream& stream) : stream(stream) {}

    void Report(const Domain::ProgressEvent& event) override {
        if (event.Completed > event.Total) throw std::logic_error("Invalid progress counters");
        const char* label = nullptr;
        switch (event.Stage) {
            case Domain::ProgressStage::Executable: label = "Executable"; break;
            case Domain::ProgressStage::Modules: label = "PRX conversion"; break;
            case Domain::ProgressStage::Syscalls: label = "Syscall scan"; break;
            case Domain::ProgressStage::Decode: label = "Instruction scan"; break;
            case Domain::ProgressStage::Imports: label = "Import analysis"; break;
            case Domain::ProgressStage::Image: label = "Output image"; break;
            case Domain::ProgressStage::Write: label = "Write output"; break;
            default: throw std::logic_error("Unknown progress stage");
        }
        const auto percent = event.Total == 0 ? 100u : static_cast<unsigned>(100.0L * event.Completed / event.Total);
        if (lastStage == event.Stage && lastInput == event.Input && lastPercent == percent && event.Completed != 0) return;
        stream << label << " [" << event.Input << "]: " << percent << "% (" << event.Completed << '/' << event.Total << ")\n" << std::flush;
        lastStage = event.Stage;
        lastInput = event.Input;
        lastPercent = percent;
    }

    void Message(std::string_view message) override {
        stream << message << std::flush;
    }

private:
    std::ostream& stream;
    Domain::ProgressStage lastStage = Domain::ProgressStage::Write;
    std::string lastInput;
    unsigned lastPercent = 101;
};

}

#endif
