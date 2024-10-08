
/** Dummy compute program for reflection of the structured buffer types.

     Falcor requires reflection data to be able to create a structured buffer,
     but the programs are not yet ready to be used when we create the buffers.
     Therefore we create this dummy program to make it easier.
 */

import ShadowMap;
#ifdef ORACLE_VALID
import Oracle.ShadowMapOracle;
#endif

StructuredBuffer<uint> gShadowMapIndexMap;
StructuredBuffer<float4x4> gShadowMapVPBuffer;

void main(){}
