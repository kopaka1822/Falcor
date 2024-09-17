from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_RTAOPass():
    g = RenderGraph('RTAOPass')
    g.create_pass('RTAO', 'RTAO', {})
    g.create_pass('GBufferRT', 'GBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'texLOD': 'Mip0', 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('NRD_Occlusion', 'NRD_Occlusion', {'enabled': True, 'method': 'ReblurOcclusionDiffuse', 'outputSize': 'Default', 'worldSpaceMotion': True, 'disocclusionThreshold': 2.0, 'maxIntensity': 250.0})
    g.create_pass('DLSSPass', 'DLSSPass', {'enabled': True, 'outputSize': 'Default', 'profile': 'Balanced', 'motionVectorScale': 'Relative', 'isHDR': False, 'useJitteredMV': False, 'sharpness': 0.0, 'exposure': 0.0})
    g.create_pass('ColorMapPass', 'ColorMapPass', {'colorMap': 'Grey', 'channel': 0, 'autoRange': True, 'minValue': 0.0, 'maxValue': 1.0})
    g.create_pass('ModulateIllumination', 'ModulateIllumination', {'useEmission': True, 'useDiffuseReflectance': True, 'useDiffuseRadiance': True, 'useSpecularReflectance': True, 'useSpecularRadiance': True, 'useDeltaReflectionEmission': True, 'useDeltaReflectionReflectance': True, 'useDeltaReflectionRadiance': True, 'useDeltaTransmissionEmission': True, 'useDeltaTransmissionReflectance': True, 'useDeltaTransmissionRadiance': True, 'useResidualRadiance': True, 'outputSize': 'Default', 'useDebug': True, 'inputInYCoCg': False})
    g.add_edge('GBufferRT.posW', 'RTAO.wPos')
    g.add_edge('GBufferRT.depth', 'DLSSPass.depth')
    g.add_edge('GBufferRT.faceNormalW', 'RTAO.faceNormal')
    g.add_edge('RTAO.ambient', 'NRD_Occlusion.diffuseHitDist')
    g.add_edge('GBufferRT.linearZ', 'NRD_Occlusion.viewZ')
    g.add_edge('GBufferRT.normWRoughnessMaterialID', 'NRD_Occlusion.normWRoughnessMaterialID')
    g.add_edge('GBufferRT.mvecW', 'NRD_Occlusion.mvec')
    g.add_edge('GBufferRT.mvec', 'DLSSPass.mvec')
    g.add_edge('NRD_Occlusion.filteredDiffuseOcclusion', 'ColorMapPass.input')
    g.add_edge('ColorMapPass.output', 'DLSSPass.color')
    g.add_edge('NRD_Occlusion.filteredDiffuseOcclusion', 'ModulateIllumination.residualRadiance')
    g.add_edge('NRD_Occlusion.outValidation', 'ModulateIllumination.debugLayer')
    g.mark_output('DLSSPass.output')
    g.mark_output('ModulateIllumination.output')
    return g

RTAOPass = render_graph_RTAOPass()
try: m.addGraph(RTAOPass)
except NameError: None
