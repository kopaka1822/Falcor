from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_RTAO():
    g = RenderGraph('RTAO')
    g.create_pass('RTAO', 'RTAO', {})
    g.create_pass('NRD', 'NRD', {'enabled': True, 'method': 'ReblurOcclusionDiffuse', 'outputSize': 'Default', 'worldSpaceMotion': True, 'disocclusionThreshold': 2.0, 'maxIntensity': 250.0})
    g.create_pass('GBufferRT', 'GBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'texLOD': 'RayDiffs', 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('DiffuseAmbient', 'ImageEquation', {'formula': 'I0[xy].xxxw * I1[xy]', 'format': 'RGBA16Float'})
    g.create_pass('TAA0', 'TAA', {'alpha': 0.20000000298023224, 'colorBoxSigma': 1.0, 'antiFlicker': True})
    g.create_pass('EnvMapPass', 'EnvMapPass', {})
    g.create_pass('ForwardLighting', 'ForwardLighting', {'envMapIntensity': 0.25, 'ambientIntensity': 0.25, 'lightIntensity': 0.5, 'envMapMirror': False})
    g.create_pass('RayShadow', 'RayShadow', {})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Linear', 'clamp': False, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('DepthPass', 'DepthPass', {'depthFormat': 'D32Float', 'useAlphaTest': True, 'cullMode': 'Back'})
    g.create_pass('Ambient', 'ImageEquation', {'formula': 'I0[xy].xxxw', 'format': 'RGBA8UnormSrgb'})
    g.create_pass('PathBenchmark', 'PathBenchmark', {})
    g.add_edge('RTAO.ambient', 'NRD.diffuseHitDist')
    g.add_edge('GBufferRT.normWRoughnessMaterialID', 'NRD.normWRoughnessMaterialID')
    g.add_edge('GBufferRT.posW', 'RTAO.wPos')
    g.add_edge('GBufferRT.faceNormalW', 'RTAO.faceNormal')
    g.add_edge('GBufferRT.linearZ', 'NRD.viewZ')
    g.add_edge('DiffuseAmbient.out', 'TAA0.colorIn')
    g.add_edge('GBufferRT.mvec', 'TAA0.motionVecs')
    g.add_edge('EnvMapPass.color', 'ForwardLighting.color')
    g.add_edge('GBufferRT.posW', 'RayShadow.posW')
    g.add_edge('GBufferRT.faceNormalW', 'RayShadow.normalW')
    g.add_edge('RayShadow.visibility', 'ForwardLighting.visibilityBuffer')
    g.add_edge('ForwardLighting.color', 'ToneMapper.src')
    g.add_edge('ToneMapper.dst', 'DiffuseAmbient.I1')
    g.add_edge('DepthPass.depth', 'EnvMapPass.depth')
    g.add_edge('DepthPass.depth', 'ForwardLighting.depth')
    g.add_edge('GBufferRT.mvecW', 'NRD.mvec')
    g.add_edge('NRD.filteredDiffuseOcclusion', 'DiffuseAmbient.I0')
    g.add_edge('NRD.filteredDiffuseOcclusion', 'Ambient.I0')
    g.add_edge('TAA0', 'PathBenchmark')
    g.mark_output('TAA0.colorOut')
    g.mark_output('Ambient.out')
    return g

RTAO = render_graph_RTAO()
try: m.addGraph(RTAO)
except NameError: None
