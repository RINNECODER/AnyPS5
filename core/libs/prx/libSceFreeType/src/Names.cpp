#include "prx/libSceFreeType/include/GuestObjects.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"

#include <cstdint>

using namespace FreeType;

extern "C" {

int APS5_VABI FT_Get_Glyph_Name_nid_postfix(GuestFaceRec* face, std::uint32_t glyphIndex, void* buffer, std::uint32_t bufferSize) {
    const GuestFace* owner = FaceOf(face);
    const FT_Face host = owner ? owner->host : nullptr;
    if (!host || !buffer || bufferSize == 0) return FT_Get_Glyph_Name(host, glyphIndex, buffer, bufferSize);
    GuestArena::HostWrite write(buffer, bufferSize);
    if (static_cast<std::int64_t>(glyphIndex) >= host->num_glyphs) {
        static_cast<char*>(buffer)[0] = '\0';
        return FT_Err_Invalid_Glyph_Index;
    }
    return FT_Get_Glyph_Name(host, glyphIndex, buffer, bufferSize);
}

std::uint32_t APS5_VABI FT_Get_Name_Index_nid_postfix(GuestFaceRec* face, const char* glyphName) {
    const GuestFace* owner = FaceOf(face);
    return FT_Get_Name_Index(owner ? owner->host : nullptr, glyphName);
}

const char* APS5_VABI FT_Get_Postscript_Name_nid_postfix(GuestFaceRec* face) {
    const GuestFace* owner = FaceOf(face);
    return FT_Get_Postscript_Name(owner ? owner->host : nullptr);
}

}
