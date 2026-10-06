#pragma once

#include <cstdint>

namespace AgcDriver {

namespace DriverDetail {
struct Submission;
}

void ValidateSubmission(const DriverDetail::Submission& submission, std::uint64_t guestAddress = 0);

}
