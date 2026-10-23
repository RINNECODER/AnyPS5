#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int NP_PARTY_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80552504u);
constexpr std::uint16_t NP_PARTY_STATE_NOT_IN_PARTY = 2;

}

extern "C" {

int APS5_VABI sceNpPartyGetState(std::uint16_t* state) {
    if (!state) return NP_PARTY_ERROR_INVALID_ARGUMENT;
    *state = NP_PARTY_STATE_NOT_IN_PARTY;
    return 0;
}
int APS5_VABI sceNpPartyCheckCallback() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyCreate() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyCreateA() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetId() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetMemberInfo() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetMemberInfoA() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetMemberSessionInfo() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetMemberVoiceInfo() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetMembers() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetMembersA() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetStateAsUser() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetStateAsUserA() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyGetVoiceChatPriority() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyInitialize() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyJoin() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyLeave() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyRegisterHandler() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyRegisterHandlerA() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyRegisterPrivateHandler() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartySendBinaryMessage() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartySetVoiceChatPriority() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyShowInvitationList() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyShowInvitationListA() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyTerminate() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceNpPartyUnregisterPrivateHandler() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
