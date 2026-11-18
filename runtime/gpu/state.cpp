// Draw state decoding from the GPU register file. Register layouts: generated/registers.h
// (from Xenia's registers.h, BSD - see docs/licenses/xenia-LICENSE.txt).
#include "state.h"
#include "gpu.h"
#include "generated/registers.h"

#include <cstdio>
#include <cstring>

namespace gpu
{
    using namespace regs;

    namespace
    {
        template <typename R> R readReg() { return R{ readRegister(R::kIndex) }; }
        float regFloat(uint32_t index)
        {
            uint32_t v = readRegister(index);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }

        // Indexed registers that share one layout
        constexpr uint32_t kColorInfo[4] = { 0x2001, 0x2003, 0x2004, 0x2005 };     // RB_COLOR_INFO, RB_COLOR1..3_INFO
        constexpr uint32_t kBlendControl[4] = { 0x2201, 0x2209, 0x220A, 0x220B }; // RB_BLENDCONTROL0..3
        constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C, RB_ALPHA_REF = 0x210E;
        constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F; // XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET
        constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380, PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381;

        std::string blendText(const BlendState& b)
        {
            if (!b.enabled()) return "off";
            char buf[160];
            snprintf(buf, sizeof(buf), "rgb %s(%s, %s) a %s(%s, %s)", BlendOpName(b.colorOp), BlendFactorName(b.colorSrc),
                     BlendFactorName(b.colorDst), BlendOpName(b.alphaOp), BlendFactorName(b.alphaSrc), BlendFactorName(b.alphaDst));
            return buf;
        }

        std::string maskText(uint32_t m)
        {
            std::string s;
            const char* c = "rgba";
            for (int i = 0; i < 4; i++) s += (m >> i) & 1 ? c[i] : '-';
            return s;
        }
    }

    DrawState captureDrawState(uint32_t initiator)
    {
        DrawState s;
        VGT_DRAW_INITIATOR di{ initiator };
        s.primitive = di.prim_type();
        s.indexCount = di.num_indices();
        s.indexSource = di.source_select();
        s.indexSize = di.index_size();
        s.indexOffset = int32_t(readReg<VGT_INDX_OFFSET>().indx_offset());

        auto mode = readReg<PA_SU_SC_MODE_CNTL>();
        s.primitiveRestart = mode.multi_prim_ib_ena();
        s.restartIndex = readReg<VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx();

        // EDRAM and render targets
        s.edramMode = readReg<RB_MODECONTROL>().edram_mode();
        auto surface = readReg<RB_SURFACE_INFO>();
        s.surfacePitch = surface.surface_pitch();
        s.msaaSamples = 1u << surface.msaa_samples();
        auto colorMask = readReg<RB_COLOR_MASK>().value;
        for (int i = 0; i < 4; i++)
        {
            RB_COLOR_INFO ci{ readRegister(kColorInfo[i]) };
            RB_BLENDCONTROL bc{ readRegister(kBlendControl[i]) };
            ColorTarget& t = s.color[i];
            t.format = ci.color_format();
            t.edramBase = ci.color_base() | (uint32_t(ci.color_base_bit_11()) << 11);
            t.expBias = ci.color_exp_bias();
            t.writeMask = (colorMask >> (4 * i)) & 0xF;
            t.blend = { bc.color_srcblend(), bc.color_destblend(), bc.color_comb_fcn(),
                        bc.alpha_srcblend(), bc.alpha_destblend(), bc.alpha_comb_fcn() };
        }
        auto depthInfo = readReg<RB_DEPTH_INFO>();
        s.depthFormat = depthInfo.depth_format();
        s.depthBase = depthInfo.depth_base() | (uint32_t(depthInfo.depth_base_bit_11()) << 11);

        // Depth / stencil
        auto dc = readReg<RB_DEPTHCONTROL>();
        s.depthTest = dc.z_enable();
        s.depthWrite = dc.z_write_enable();
        s.depthFunc = dc.zfunc();
        s.stencil = dc.stencil_enable();
        s.stencilBackface = dc.backface_enable();
        auto ref = readReg<RB_STENCILREFMASK>();
        RB_STENCILREFMASK refBack{ readRegister(RB_STENCILREFMASK_BF) };
        s.front = { dc.stencilfunc(), dc.stencilfail(), dc.stencilzpass(), dc.stencilzfail(), ref.stencilref(), ref.stencilmask(), ref.stencilwritemask() };
        s.back = { dc.stencilfunc_bf(), dc.stencilfail_bf(), dc.stencilzpass_bf(), dc.stencilzfail_bf(), refBack.stencilref(),
                   refBack.stencilmask(), refBack.stencilwritemask() };

        // Alpha test
        auto cc = readReg<RB_COLORCONTROL>();
        s.alphaTest = cc.alpha_test_enable();
        s.alphaFunc = cc.alpha_func();
        s.alphaToMask = cc.alpha_to_mask_enable();
        s.alphaRef = regFloat(RB_ALPHA_REF);

        // Rasterizer
        s.cullFront = mode.cull_front();
        s.cullBack = mode.cull_back();
        s.frontFaceCW = mode.face();
        s.msaa = mode.msaa_enable();
        s.provokingLast = mode.provoking_vtx_last();
        s.polyMode = mode.poly_mode();
        s.polyFront = mode.polymode_front_ptype();
        s.polyBack = mode.polymode_back_ptype();
        s.polyOffsetFront = mode.poly_offset_front_enable();
        s.polyOffsetBack = mode.poly_offset_back_enable();
        s.polyOffsetScale = regFloat(PA_SU_POLY_OFFSET_FRONT_SCALE);
        s.polyOffset = regFloat(PA_SU_POLY_OFFSET_FRONT_OFFSET);
        auto clip = readReg<PA_CL_CLIP_CNTL>();
        s.clipDisable = clip.clip_disable();
        s.dxClipSpace = clip.dx_clip_space_def();

        // Viewport
        auto vte = readReg<PA_CL_VTE_CNTL>();
        bool scaleEna[3] = { vte.vport_x_scale_ena(), vte.vport_y_scale_ena(), vte.vport_z_scale_ena() };
        bool offsetEna[3] = { vte.vport_x_offset_ena(), vte.vport_y_offset_ena(), vte.vport_z_offset_ena() };
        for (int i = 0; i < 3; i++)
        {
            if (scaleEna[i]) s.vpScale[i] = regFloat(PA_CL_VPORT_XSCALE + 2 * i);
            if (offsetEna[i]) s.vpOffset[i] = regFloat(PA_CL_VPORT_XSCALE + 2 * i + 1);
        }
        s.xyDividedByW = vte.vtx_xy_fmt();
        s.zDividedByW = vte.vtx_z_fmt();
        s.wNotReciprocal = vte.vtx_w0_fmt();
        s.pixelCenterHalf = readReg<PA_SU_VTX_CNTL>().pix_center();
        s.vertexWindowOffset = mode.vtx_window_offset_enable();

        // Scissor
        auto wo = readReg<PA_SC_WINDOW_OFFSET>();
        s.windowOffsetX = wo.window_x_offset();
        s.windowOffsetY = wo.window_y_offset();
        auto tl = readReg<PA_SC_WINDOW_SCISSOR_TL>();
        auto br = readReg<PA_SC_WINDOW_SCISSOR_BR>();
        s.windowOffsetDisabled = tl.window_offset_disable();
        s.scissorTL[0] = tl.tl_x(); s.scissorTL[1] = tl.tl_y();
        s.scissorBR[0] = br.br_x(); s.scissorBR[1] = br.br_y();
        auto stl = readReg<PA_SC_SCREEN_SCISSOR_TL>();
        auto sbr = readReg<PA_SC_SCREEN_SCISSOR_BR>();
        s.screenScissorTL[0] = stl.tl_x(); s.screenScissorTL[1] = stl.tl_y();
        s.screenScissorBR[0] = sbr.br_x(); s.screenScissorBR[1] = sbr.br_y();

        // Shader setup
        auto pc = readReg<SQ_PROGRAM_CNTL>();
        s.vsRegisters = pc.vs_num_reg() + 1;
        s.psRegisters = pc.ps_num_reg() + 1;
        s.vsExportMode = pc.vs_export_mode();
        s.vsExportCount = pc.vs_export_count();
        s.psExportMode = pc.ps_export_mode();
        s.paramGen = pc.param_gen();
        s.sampleControl = readReg<SQ_CONTEXT_MISC>().sc_sample_cntl();
        s.samplingPattern = readReg<SQ_INTERPOLATOR_CNTL>().sampling_pattern();
        auto vsc = readReg<SQ_VS_CONST>();
        auto psc = readReg<SQ_PS_CONST>();
        s.vsConstBase = vsc.base(); s.vsConstSize = vsc.size();
        s.psConstBase = psc.base(); s.psConstSize = psc.size();
        return s;
    }

    std::string describe(const DrawState& s)
    {
        char buf[512];
        std::string out;
        snprintf(buf, sizeof(buf), "  draw: %s %u indices (%s%s)%s offset=%d\n", PrimitiveTypeName(s.primitive), s.indexCount,
                 SourceSelectName(s.indexSource), s.indexSource == 0 ? (s.indexSize ? " u32" : " u16") : "",
                 s.primitiveRestart ? (" restart=" + std::to_string(s.restartIndex)).c_str() : "", s.indexOffset);
        out += buf;
        snprintf(buf, sizeof(buf), "  edram: %s pitch=%u msaa=%ux\n", EdramModeName(s.edramMode), s.surfacePitch, s.msaaSamples);
        out += buf;
        for (int i = 0; i < 4; i++)
        {
            const ColorTarget& t = s.color[i];
            if (!t.writeMask) continue;
            snprintf(buf, sizeof(buf), "  rt%d: %s @tile %u mask=%s exp_bias=%d blend %s\n", i, ColorRenderTargetFormatName(t.format),
                     t.edramBase, maskText(t.writeMask).c_str(), t.expBias, blendText(t.blend).c_str());
            out += buf;
        }
        snprintf(buf, sizeof(buf), "  depth: %s @tile %u test=%s write=%d", DepthRenderTargetFormatName(s.depthFormat), s.depthBase,
                 s.depthTest ? CompareFunctionName(s.depthFunc) : "off", s.depthWrite);
        out += buf;
        if (s.stencil)
        {
            snprintf(buf, sizeof(buf), " stencil %s ref=%u mask=%02X write=%02X fail=%s zfail=%s zpass=%s%s", CompareFunctionName(s.front.func),
                     s.front.ref, s.front.mask, s.front.writeMask, StencilOpName(s.front.fail), StencilOpName(s.front.zfail),
                     StencilOpName(s.front.zpass), s.stencilBackface ? " (separate back face)" : "");
            out += buf;
        }
        out += "\n";
        snprintf(buf, sizeof(buf), "  raster: cull=%s front=%s%s%s%s%s\n",
                 s.cullFront && s.cullBack ? "both" : s.cullFront ? "front" : s.cullBack ? "back" : "none", s.frontFaceCW ? "cw" : "ccw",
                 s.polyMode ? (std::string(" poly=") + PolygonTypeName(s.polyFront) + "/" + PolygonTypeName(s.polyBack)).c_str() : "",
                 s.polyOffsetFront || s.polyOffsetBack ? (" offset=" + std::to_string(s.polyOffsetScale) + "," + std::to_string(s.polyOffset)).c_str() : "",
                 s.msaa ? " msaa" : "", s.clipDisable ? " clip_disabled" : "");
        out += buf;
        if (s.alphaTest || s.alphaToMask)
        {
            snprintf(buf, sizeof(buf), "  alpha: %s %s %.3f%s\n", s.alphaTest ? "test" : "", CompareFunctionName(s.alphaFunc), s.alphaRef,
                     s.alphaToMask ? " alpha_to_mask" : "");
            out += buf;
        }
        snprintf(buf, sizeof(buf), "  viewport: scale=(%.1f, %.1f, %.3f) offset=(%.1f, %.1f, %.3f)%s%s window_offset=(%d, %d)%s\n",
                 s.vpScale[0], s.vpScale[1], s.vpScale[2], s.vpOffset[0], s.vpOffset[1], s.vpOffset[2],
                 s.xyDividedByW ? " xy/w" : "", s.wNotReciprocal ? "" : " w_is_1/w", s.windowOffsetX, s.windowOffsetY,
                 s.windowOffsetDisabled ? " (disabled for scissor)" : "");
        out += buf;
        snprintf(buf, sizeof(buf), "  scissor: window (%u,%u)-(%u,%u) screen (%u,%u)-(%u,%u)\n", s.scissorTL[0], s.scissorTL[1],
                 s.scissorBR[0], s.scissorBR[1], s.screenScissorTL[0], s.screenScissorTL[1], s.screenScissorBR[0], s.screenScissorBR[1]);
        out += buf;
        snprintf(buf, sizeof(buf), "  shaders: vs %u regs export_mode=%u exports=%u, ps %u regs export_mode=%u%s, consts vs %u+%u ps %u+%u, sample_cntl=%u pattern=%04X\n",
                 s.vsRegisters, s.vsExportMode, s.vsExportCount, s.psRegisters, s.psExportMode, s.paramGen ? " param_gen" : "",
                 s.vsConstBase, s.vsConstSize, s.psConstBase, s.psConstSize, s.sampleControl, s.samplingPattern);
        out += buf;
        return out;
    }

    std::string pipelineKey(const DrawState& s)
    {
        // Everything that becomes fixed-function pipeline state in Vulkan (shaders are keyed separately)
        char buf[512];
        int n = snprintf(buf, sizeof(buf), "%s|%s|msaa%u", PrimitiveTypeName(s.primitive), EdramModeName(s.edramMode), s.msaaSamples);
        for (int i = 0; i < 4; i++)
        {
            const ColorTarget& t = s.color[i];
            if (!t.writeMask) continue;
            n += snprintf(buf + n, sizeof(buf) - n, "|rt%d:%s:%s:%s", i, ColorRenderTargetFormatName(t.format), maskText(t.writeMask).c_str(),
                          blendText(t.blend).c_str());
        }
        n += snprintf(buf + n, sizeof(buf) - n, "|z:%s:%s:%d", DepthRenderTargetFormatName(s.depthFormat),
                      s.depthTest ? CompareFunctionName(s.depthFunc) : "off", s.depthWrite);
        if (s.stencil)
            n += snprintf(buf + n, sizeof(buf) - n, "|st:%s:%s:%s:%s", CompareFunctionName(s.front.func), StencilOpName(s.front.fail),
                          StencilOpName(s.front.zfail), StencilOpName(s.front.zpass));
        snprintf(buf + n, sizeof(buf) - n, "|cull:%d%d:%s|alpha:%s", s.cullFront, s.cullBack, s.frontFaceCW ? "cw" : "ccw",
                 s.alphaTest ? CompareFunctionName(s.alphaFunc) : "off");
        return buf;
    }
}
