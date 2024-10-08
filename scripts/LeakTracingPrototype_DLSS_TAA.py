from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_LeakTracingPrototype():
    g = RenderGraph('LeakTracingPrototype')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'DirectX', 'sampleCount': 8, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back'})
    g.create_pass('ShadowPass', 'ShadowPass', {})
    g.create_pass('VideoRecorder', 'VideoRecorder', {})
    g.create_pass('DLSSPass', 'DLSSPass', {'enabled': True, 'outputSize': 'Default', 'profile': 'Balanced', 'motionVectorScale': 'Relative', 'isHDR': False, 'useJitteredMV': False, 'sharpness': 0.0, 'exposure': 0.0})
    g.create_pass('PathBenchmark', 'PathBenchmark', {})
    g.create_pass('TAA', 'TAA', {'alpha': 0.10000000149011612, 'colorBoxSigma': 1.0, 'antiFlicker': True})
    g.add_edge('GBufferRaster.posW', 'ShadowPass.posW')
    g.add_edge('GBufferRaster.faceNormalW', 'ShadowPass.faceNormalW')
    g.add_edge('GBufferRaster.diffuseOpacity', 'ShadowPass.diffuse')
    g.add_edge('GBufferRaster.specRough', 'ShadowPass.specularRoughness')
    g.add_edge('GBufferRaster.emissive', 'ShadowPass.emissive')
    g.add_edge('GBufferRaster.guideNormalW', 'ShadowPass.guideNormalW')
    g.add_edge('ShadowPass.color', 'DLSSPass.color')
    g.add_edge('GBufferRaster.depth', 'DLSSPass.depth')
    g.add_edge('GBufferRaster.mvec', 'DLSSPass.mvec')
    g.add_edge('GBufferRaster.mvec', 'ShadowPass.motionVector')
    g.add_edge('VideoRecorder', 'GBufferRaster')
    g.add_edge('PathBenchmark', 'VideoRecorder')
    g.add_edge('ShadowPass.color', 'TAA.colorIn')
    g.add_edge('GBufferRaster.mvec', 'TAA.motionVecs')
    g.mark_output('DLSSPass.output')
    g.mark_output('ShadowPass.color')
    g.mark_output('TAA.colorOut')
    return g

LeakTracingPrototype = render_graph_LeakTracingPrototype()
try: m.addGraph(LeakTracingPrototype)
except NameError: None
