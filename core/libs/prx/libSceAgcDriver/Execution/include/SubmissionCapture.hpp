#pragma once

#include <cstddef>
#include <cstdint>

namespace AgcDriver {

namespace DriverDetail {
struct Submission;
}

void CaptureSubmissionCommands(DriverDetail::Submission& submission, std::uint64_t guestAddress, std::size_t words);
// Reads the (offset, value) list of every SET_*_REG_INDIRECT packet of the captured commands into
// submission.registerLists, keyed by the packet's DWORD cursor: the lists are taken when the command
// buffer is submitted, as the commands are, not when the queue worker reaches the packet.
void CaptureIndirectRegisterLists(DriverDetail::Submission& submission);

}
