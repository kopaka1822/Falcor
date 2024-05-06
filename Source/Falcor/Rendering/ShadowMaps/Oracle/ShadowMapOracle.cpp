#include "ShadowMapOracle.h"
#include "Scene/Camera/Camera.h"
#include "Utils/Math/FalcorMath.h"

namespace Falcor
{

void ShadowMapOracle::createParameterBlock(ref<ComputePass> reflectProgram) {
    auto reflector = reflectProgram->getProgram()->getReflector()->getParameterBlock("gShadowMapOracle");
    mpShadowMapOracleParameterBlock = ParameterBlock::create(mpDevice, reflector);
    FALCOR_ASSERT(mpShadowMapOracleParameterBlock);
}

DefineList ShadowMapOracle::getDefines() const
{
    DefineList defines;

    defines.add("NPS_OFFSET_SPOT", std::to_string(mNPSOffsets.x));
    defines.add("NPS_OFFSET_CASCADED", std::to_string(mNPSOffsets.y));
    defines.add("ORACLE_DIST_FUNCTION_MODE", std::to_string((uint)mOracleDistanceFunctionMode));

    defines.add("USE_ORACLE_FUNCTION", mUseSMOracle ? "1" : "0");
    defines.add("ORACLE_COMP_VALUE", std::to_string(mOracleCompaireValue));
    defines.add("ORACLE_ADD_RAYS", mOracleAddRays ? "1" : "0");
    defines.add("ORACLE_UPPER_BOUND", std::to_string(mOracleCompaireUpperBound));
    defines.add("USE_ORACLE_DISTANCE_FUNCTION", mOracleDistanceFunctionMode == OracleDistFunction::None ? "0" : "1");
    defines.add("USE_ORACLE_FOR_DIRECT", mOracleIgnoreDirect ? "1" : "0");
    defines.add("USE_ORACLE_FOR_DIRECT_ROUGHNESS", std::to_string(mOracleIgnoreDirectRoughness));

    return defines;
}

void ShadowMapOracle::setShaderData(const uint2 frameDim, const CameraData& cameraData)
{
    FALCOR_ASSERT(mpShadowMapOracleParameterBlock);

    auto var = mpShadowMapOracleParameterBlock->getRootVar();
    var["gCameraNPS"] =
        getNormalizedPixelSize(frameDim, focalLengthToFovY(cameraData.focalLength, cameraData.frameHeight), cameraData.aspectRatio);

    var["gShadowMapNPSBuffer"] = mpNormalizedPixelSize; // Can be Nullptr on init
}

// Gets the pixel size at distance 1. Assumes every pixel has the same size.
float ShadowMapOracle::getNormalizedPixelSize(uint2 frameDim, float fovY, float aspect)
{
    float h = tan(fovY / 2.f) * 2.f;
    float w = h * aspect;
    float wPix = w / frameDim.x;
    float hPix = h / frameDim.y;
    return wPix * hPix;
}

float ShadowMapOracle::getNormalizedPixelSizeOrtho(uint2 frameDim, float width, float height)
{
    float wPix = width / frameDim.x;
    float hPix = height / frameDim.y;
    return wPix * hPix;
}

void ShadowMapOracle::handleNormalizedPixelSizeBuffer(ref<Scene> pScene, uint shadowMapSize, uint shadowCubeSize, uint shadowCascSize, uint cascadedLevelCount, std::vector<float2>& cascadedWidthHeight)
{
    // If buffer is build, no need to rebuild it
    if (mpNormalizedPixelSize)
        return;

    // Get the NPS(Normalized Pixel Size) for each light type
    std::vector<float> pointNPS;
    std::vector<float> spotNPS;
    std::vector<float> dirNPS;

    uint cascadedCount = 0;

    //TODO better solution for this
    auto getLightType = [&](const ref<Light> light) {
        const LightType& type = light->getType();
        if (type == LightType::Directional)
            return LightTypeSM::Directional;
        else if (type == LightType::Point)
        {
            if (light->getData().openingAngle > M_PI_4)
                return LightTypeSM::Point;
            else
                return LightTypeSM::Spot;
        }

        return LightTypeSM::NotSupported;
    };

    for (ref<Light> light : pScene->getLights())
    {
        LightTypeSM type = getLightType(light);
        switch (type)
        {
        case LightTypeSM::Point:
            pointNPS.push_back(getNormalizedPixelSize(uint2(shadowCubeSize), float(M_PI_2), 1.f));
            break;
        case LightTypeSM::Spot:
            spotNPS.push_back(getNormalizedPixelSize(uint2(shadowMapSize), light->getData().openingAngle * 2, 1.f));
            break;
        case LightTypeSM::Directional:
            for (uint i = 0; i < cascadedLevelCount; i++)
            {
                float2 wh = cascadedWidthHeight[cascadedCount * cascadedLevelCount + i];
                dirNPS.push_back(getNormalizedPixelSizeOrtho(uint2(shadowCascSize), wh.x, wh.y));
            }
            cascadedCount++;
            break;
        }
    }

    mNPSOffsets = uint2(pointNPS.size(), pointNPS.size() + spotNPS.size());
    size_t totalSize = mNPSOffsets.y + dirNPS.size();

    std::vector<float> npsData;
    npsData.reserve(totalSize);
    for (auto nps : pointNPS)
        npsData.push_back(nps);
    for (auto nps : spotNPS)
        npsData.push_back(nps);
    for (auto nps : dirNPS)
        npsData.push_back(nps);

    // Create the buffer
    mpNormalizedPixelSize = Buffer::createStructured(
        mpDevice, sizeof(float), npsData.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, npsData.data(), false
    );
    mpNormalizedPixelSize->setName("ShadowMap::NPSBuffer");
}

bool ShadowMapOracle::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;
    dirty |= widget.checkbox("Use Oracle Function", mUseSMOracle);
    widget.tooltip("Enables the oracle function for Shadow Mapping", true);
    if (mUseSMOracle)
    {
        dirty |= widget.var("Oracle Compaire Value", mOracleCompaireValue, 0.f, 64.f, 0.1f);
        widget.tooltip("Compaire Value for the Oracle function. Is basically compaired against ShadowMapPixelArea/CameraPixelArea.");
        dirty |= widget.var("Oracle Upper Bound", mOracleCompaireUpperBound, mOracleCompaireValue, 2048.f, 0.1f);
        widget.tooltip("Upper Bound for the oracle value. If oracle is above this value the shadow test is skipped and an ray is shot.");

        dirty |= widget.checkbox("Oracle Add Rays", mOracleAddRays);
        widget.tooltip("Adds rays when enabled");

        dirty |= widget.dropdown("Oracle Distance Mode", mOracleDistanceFunctionMode);
        widget.tooltip("Mode for the distance factor applied on bounces.");

        dirty |= widget.checkbox("Ignore Oracle on direct hit", mOracleIgnoreDirect);
        widget.tooltip("Ignores the oracle on direct and very specular hits");
        if (mOracleIgnoreDirect)
        {
            dirty |= widget.var("Ignore Oracle Roughness", mOracleIgnoreDirectRoughness, 0.f, 1.f, 0.0001f);
            widget.tooltip("The roughness that defines the very specular hits for the ignore oracle function");
        }
    }

    return dirty;
}


}//Falcor Namespace
