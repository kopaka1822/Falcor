#include "FXAA.h"

namespace
{
    const std::string kColorIn = "colorIn";
    const std::string kColorOut = "colorOut";

    const std::string kQualityLevel = "qualityLevel"; // For FXAA quality settings

    const std::string kShaderFilename = "RenderPasses/FXAA/FXAA.ps.slang"; // Path to the FXAA shader
}

static void regFXAA(pybind11::module& m)
{
    pybind11::class_<FXAA, RenderPass, ref<FXAA>> pass(m, "FXAA");
    pass.def_property("qualityLevel", &FXAA::getQualityLevel, &FXAA::setQualityLevel);
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, FXAA>();
    ScriptBindings::registerBinding(regFXAA);
}

FXAA::FXAA(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpPass = FullScreenPass::create(mpDevice, kShaderFilename);
    mpFbo = Fbo::create(mpDevice);
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear);
    mpLinearSampler = Sampler::create(mpDevice, samplerDesc);

    for (const auto& [key, value] : props)
    {
        if (key == kQualityLevel) mControls.qualityLevel = value;
        else logWarning("Unknown property '{}' in FXAA properties.", key);
    }
}

Properties FXAA::getProperties() const
{
    Properties props;
    props[kQualityLevel] = mControls.qualityLevel;
    return props;
}

RenderPassReflection FXAA::reflect(const CompileData& compileData)
{
    RenderPassReflection reflection;
    reflection.addInput(kColorIn, "Color-buffer of the current frame");
    reflection.addOutput(kColorOut, "Anti-aliased color buffer");
    return reflection;
}

void FXAA::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    const auto& pColorIn = renderData.getTexture(kColorIn);
    const auto& pColorOut = renderData.getTexture(kColorOut);
    if (!mEnabled)
    {
        pRenderContext->blit(pColorIn->getSRV(), pColorOut->getRTV());
        return;
    }

    mpFbo->attachColorTarget(pColorOut, 0);

    auto var = mpPass->getRootVar();
    mpPass->addDefine("FXAA_PRESET", std::to_string(getQualityLevel()));
    var["gTexColor"] = pColorIn;
    var["gSampler"] = mpLinearSampler;

    mpPass->execute(pRenderContext, mpFbo);
}

void FXAA::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;
    widget.var("Quality Level", mControls.qualityLevel, 0, 5, 1); // Assuming quality levels from 0 to 3
}
