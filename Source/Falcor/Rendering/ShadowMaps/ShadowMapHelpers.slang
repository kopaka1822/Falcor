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
    
