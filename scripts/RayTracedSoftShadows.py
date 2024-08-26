from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_RayTracedSoftShadows():
    g = RenderGraph('RayTracedSoftShadows')
    g.create_pass('RayTracedSoftShadows', 'RayTracedSoftShadows', {})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': False, 'outputSize': 'Default', 'autoReset': True, 'precisionMode': 'Single', 'maxFrameCount': 0, 'overflowMode': 'Stop'})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('NRD', 'NRD', {'enabled': True, 'method': 'ReblurDiffuseSpecular', 'outputSize': 'Default', 'worldSpaceMotion': False, 'disocclusionThreshold': 2.0, 'maxIntensity': 1000.0})
    g.create_pass('ModulateIllumination', 'ModulateIllumination', {'useEmission': True, 'useDiffuseReflectance': True, 'useDiffuseRadiance': True, 'useSpecularReflectance': True, 'useSpecularRadiance': True, 'useDeltaReflectionEmission': False, 'useDeltaReflectionReflectance': False, 'useDeltaReflectionRadiance': False, 'useDeltaTransmissionEmission': False, 'useDeltaTransmissionReflectance': False, 'useDeltaTransmissionRadiance': False, 'useResidualRadiance': True, 'outputSize': 'Default', 'useDebug': True})
    g.create_pass('GBufferRT', 'GBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'texLOD': 'Mip0', 'useTraceRayInline': False, 'useDOF': True})
    g.add_edge('RayTracedSoftShadows.diffuseRadiance', 'NRD.diffuseRadianceHitDist')
    g.add_edge('RayTracedSoftShadows.specularRadiance', 'NRD.specularRadianceHitDist')
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('ModulateIllumination.output', 'AccumulatePass.input')
    g.add_edge('RayTracedSoftShadows.color', 'ModulateIllumination.residualRadiance')
    g.add_edge('RayTracedSoftShadows.emission', 'ModulateIllumination.emission')
    g.add_edge('RayTracedSoftShadows.diffuseReflectance', 'ModulateIllumination.diffuseReflectance')
    g.add_edge('RayTracedSoftShadows.specularReflectance', 'ModulateIllumination.specularReflectance')
    g.add_edge('NRD.filteredDiffuseRadianceHitDist', 'ModulateIllumination.diffuseRadiance')
    g.add_edge('NRD.filteredSpecularRadianceHitDist', 'ModulateIllumination.specularRadiance')
    g.add_edge('GBufferRT.vbuffer', 'RayTracedSoftShadows.vBuffer')
    g.add_edge('GBufferRT.mvecW', 'NRD.mvec')
    g.add_edge('GBufferRT.linearZ', 'NRD.viewZ')
    g.add_edge('GBufferRT.normWRoughnessMaterialID', 'NRD.normWRoughnessMaterialID')
    g.add_edge('GBufferRT.viewW', 'RayTracedSoftShadows.viewW')
    g.add_edge('NRD.outValidation', 'ModulateIllumination.debugLayer')
    g.mark_output('ToneMapper.dst')
    g.mark_output('AccumulatePass.output')
    g.mark_output('ModulateIllumination.output')
    return g

RayTracedSoftShadows = render_graph_RayTracedSoftShadows()
try: m.addGraph(RayTracedSoftShadows)
except NameError: None
