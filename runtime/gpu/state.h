#pragma once
// Decoded Xenos draw state (roadmap phase 4, "Xenos state tracking"): everything the Vulkan
// backend needs per draw, read from the register file. Field meanings follow Xenia's
// registers.h / xenos.h (BSD; see runtime/gpu/generated/registers.h).
#include <cstdint>
#include <string>

namespace gpu
{
    struct BlendState
    {
        uint32_t colorSrc, colorDst, colorOp, alphaSrc, alphaDst, alphaOp; // xenos::BlendFactor / BlendOp
        bool enabled() const { return !(colorSrc == 1 && colorDst == 0 && colorOp == 0 && alphaSrc == 1 && alphaDst == 0 && alphaOp == 0); }
    };

    struct ColorTarget
    {
        uint32_t format;     // xenos::ColorRenderTargetFormat
        uint32_t edramBase;  // in tiles
        int32_t expBias;
        uint32_t writeMask;  // RGBA bits from RB_COLOR_MASK
        BlendState blend;
    };

    struct StencilFace { uint32_t func, fail, zpass, zfail, ref, mask, writeMask; };

    struct DrawState
    {
        // Draw
        uint32_t primitive = 0, indexCount = 0, indexSource = 0, indexSize = 0; // from VGT_DRAW_INITIATOR
        int32_t indexOffset = 0;
        bool primitiveRestart = false;
        uint32_t restartIndex = 0;

        // EDRAM / render targets
        uint32_t edramMode = 0; // xenos::EdramMode: 4 color+depth, 5 depth only, 6 copy (resolve)
        uint32_t surfacePitch = 0, msaaSamples = 0;
        ColorTarget color[4]{};
        uint32_t depthFormat = 0, depthBase = 0;

        // Depth / stencil
        bool depthTest = false, depthWrite = false, stencil = false, stencilBackface = false;
        uint32_t depthFunc = 0;
        StencilFace front{}, back{};

        // Alpha test
        bool alphaTest = false, alphaToMask = false;
        uint32_t alphaFunc = 0;
        float alphaRef = 0;

        // Rasterizer
        bool cullFront = false, cullBack = false, frontFaceCW = false, msaa = false, provokingLast = false;
        uint32_t polyMode = 0, polyFront = 0, polyBack = 0;
        bool polyOffsetFront = false, polyOffsetBack = false;
        float polyOffsetScale = 0, polyOffset = 0;
        bool clipDisable = false, dxClipSpace = false;

        // Viewport (identity components when the VTE enable bit is clear)
        float vpScale[3] = { 1, 1, 1 }, vpOffset[3] = { 0, 0, 0 };
        // PA_CL_VTE_CNTL vtx_xy_fmt / vtx_z_fmt / vtx_w0_fmt: the shader's position is already
        // divided by W (XY, Z), and W is W rather than 1/W
        bool xyDividedByW = false, zDividedByW = false, wNotReciprocal = false;
        bool pixelCenterHalf = false;     // PA_SU_VTX_CNTL pix_center: 0 = Direct3D (centers at .0), 1 = OpenGL (.5)
        bool vertexWindowOffset = false;  // PA_SU_SC_MODE_CNTL vtx_window_offset_enable

        // Scissor
        int32_t windowOffsetX = 0, windowOffsetY = 0;
        bool windowOffsetDisabled = false;
        uint32_t scissorTL[2] = {}, scissorBR[2] = {}, screenScissorTL[2] = {}, screenScissorBR[2] = {};

        // Shader setup
        uint32_t vsRegisters = 0, psRegisters = 0, vsExportMode = 0, vsExportCount = 0, psExportMode = 0;
        bool paramGen = false;
        uint32_t vsConstBase = 0, vsConstSize = 0, psConstBase = 0, psConstSize = 0;
    };

    // Read the current register file. `initiator` is the draw packet's VGT_DRAW_INITIATOR.
    DrawState captureDrawState(uint32_t initiator);

    // Multi-line human-readable description (used by the frame capture).
    std::string describe(const DrawState& s);

    // The parts of state a Vulkan graphics pipeline is built from, as a stable string.
    std::string pipelineKey(const DrawState& s);
}
