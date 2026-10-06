#pragma once

#include <cstddef>
#include <cstdint>

namespace AgcDriver {

namespace DriverDetail {
struct Submission;
}

void CaptureSubmissionCommands(DriverDetail::Submission& submission, std::uint64_t guestAddress, std::size_t words);

}
