#include "Utils/Math/MathConstants.slangh"

import Scene.Lights.LightData;
import ShadowMapData;

 /** Converts Falcors light type to our Shadow Map type
    \param[in] light info.
    \return Shadow Map type.
    */
    uint getShadowMapLightType(const in LightData lightData)
    {
        uint lightType = 0;
        //Determine the light type
        if (lightData.type == uint(LightType::Directional))
            lightType = (uint) LightTypeSM::Directional;
        else if (lightData.type == uint(LightType::Point) && lightData.openingAngle > M_PI_4)
            lightType = (uint) LightTypeSM::Point;
        else
            lightType = (uint) LightTypeSM::Spot;

        return lightType;
    }
    
    /* Converts Falcors Light Data struct to SMLightData (is much more compact)
    \param[in] light data struct from falcor
    \return Shadow Map light data struct
    */
    static SMLightData getShadowMapLightData(const in LightData lightData, const uint index)
    {
        SMLightData smLightData = {};
        smLightData.posW = lightData.posW;
        smLightData.dirW = lightData.dirW;
        smLightData.type = getShadowMapLightType(lightData);
        smLightData.index = index;
        return smLightData;
    }
