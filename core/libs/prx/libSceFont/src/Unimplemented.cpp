// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "prx/libc/include/General.hpp"
#include "prx/libSceFont/include/FontTypes.hpp"

struct FontGraphicsDevice;
struct FontGraphicsDeviceParam;
struct FontGraphicsService;
struct FontGraphicsServiceParam;
struct FontWords;
struct FontOutlineProfile;
struct FontMetrics;
struct FontStyleInformation;
struct FontGlyphExpandBufferState;
struct FontGraphicsResource;
struct FontGraphicsFillMethod;
struct FontGraphicsFillPlot;
struct FontGraphicsFillRates;
struct FontGraphicsFill;

struct FontGraphicsRegion {
    float x;
    float y;
    float width;
    float height;
    std::uint32_t flags;
    std::uint32_t tag;
    float outerRadius;
    float innerRadius;
};

static_assert(sizeof(FontGraphicsRegion) == 32);
static_assert(offsetof(FontGraphicsRegion, x) == 0);
static_assert(offsetof(FontGraphicsRegion, y) == 4);
static_assert(offsetof(FontGraphicsRegion, width) == 8);
static_assert(offsetof(FontGraphicsRegion, height) == 12);
static_assert(offsetof(FontGraphicsRegion, flags) == 16);
static_assert(offsetof(FontGraphicsRegion, tag) == 20);
static_assert(offsetof(FontGraphicsRegion, outerRadius) == 24);
static_assert(offsetof(FontGraphicsRegion, innerRadius) == 28);


#pragma GCC visibility push(default)

extern "C" {

int APS5_VABI sceFontControl() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontCreateGraphicsDevice(const FontMemory* memory, const FontGraphicsDeviceParam* param, FontGraphicsDevice** device) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontCreateGraphicsService(const FontMemory* memory, const FontGraphicsServiceParam* param, FontGraphicsService** service) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontCreateGraphicsServiceWithEdition() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontCreateWords() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontDestroyGraphicsDevice(FontGraphicsDevice** device) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontDestroyGraphicsService(FontGraphicsService** service) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontDestroyWords(FontWords** words) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}


int APS5_VABI sceFontGetFontGlyphsOutlineProfile(FontHandle font, FontOutlineProfile* profile) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGetFontMetrics(FontHandle font, std::uint32_t mode, FontMetrics* metrics) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGetFontStyleInformation(FontHandle font, FontStyleInformation* information) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGetGlyphExpandBufferState(FontHandle font, FontGlyphExpandBufferState* state) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGlyphRenderImage() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGlyphRenderImageHorizontal() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGlyphRenderImageVertical() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsBeginFrame() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsDrawingCancel() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsDrawingFinish() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsEndFrame() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsExchangeResource() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillMethodInit(FontGraphicsFillMethod* method, std::int32_t mode, std::int32_t type) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillPlotInit(FontGraphicsFillPlot* plot, const FontGraphicsRegion* region) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillPlotSetLayout(FontGraphicsFillPlot* plot, std::int32_t first, std::int32_t second) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillPlotSetMapping(FontGraphicsFillPlot* plot, float first, float second, float third, float fourth) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillRatesInit(FontGraphicsFillRates* rates, const FontGraphicsFill* fill, std::uint32_t effect) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillRatesSetFillEffect(FontGraphicsFillRates* rates, const FontGraphicsFill* fill, std::uint32_t effect) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillRatesSetLayout(FontGraphicsFillRates* rates, std::int32_t first, std::int32_t second) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsFillRatesSetMapping(FontGraphicsFillRates* rates, float first, float second, float third, float fourth) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsGetDeviceUsage() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsRegionInit(FontGraphicsRegion* region, float x, float y, float width, float height) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsRegionInitCircular(FontGraphicsRegion* region, float centerX, float centerY, float outerRadius, float innerRadius) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsRegionInitRoundish(FontGraphicsRegion* region, float x, float y, float width, float height, float outerRadius, float innerRadius, std::uint32_t flags) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsRelease() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsRenderResource() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetFramePolicy() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupClipping() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupColorRates(FontGraphicsResource* resource, float red, float green, float blue, float alpha) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupFillMethod() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupFillRates() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupGlyphFill() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupGlyphFillPlot() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupHandleDefault() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupLocation(FontGraphicsResource* resource, float x, float y) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupPositioning() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupRotation(FontGraphicsResource* resource, float angle, float pivotX, float pivotY) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupScaling(FontGraphicsResource* resource, float scaleX, float scaleY, float pivotX, float pivotY) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupShapeFill() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsSetupShapeFillPlot() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsStructureCanvas() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsStructureCanvasSequence() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsStructureDesign() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsStructureDesignResource() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsStructureSurfaceTexture() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateClipping() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateColorRates(FontGraphicsResource* resource, float red, float green, float blue, float alpha) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateFillMethod() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateFillRates() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateGlyphFill() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateGlyphFillPlot() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateLocation(FontGraphicsResource* resource, float x, float y) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdatePositioning() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateRotation(FontGraphicsResource* resource, float angle, float pivotX, float pivotY) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateScaling(FontGraphicsResource* resource, float scaleX, float scaleY, float pivotX, float pivotY) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateShapeFill() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsUpdateShapeFillPlot() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontSupportGlyphs() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontWordsFindWordCharacters() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsDrawupFillTextureImageObject(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceFontGraphicsDrawupFillTexturePatternObject(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}

#pragma GCC visibility pop
