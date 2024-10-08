from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_LeakTracingPrototype():
    g = RenderGraph('LeakTracingPrototype')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 8, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back'})
    g.create_pass('ShadowPass', 'ShadowPass', {})
    g.create_pass('VideoRecorder', 'VideoRecorder', {})
    g.create_pass('PathBenchmark', 'PathBenchmark', {})
    g.add_edge('GBufferRaster.posW', 'ShadowPass.posW')
    g.add_edge('GBufferRaster.faceNormalW', 'ShadowPass.faceNormalW')
    g.add_edge('GBufferRaster.diffuseOpacity', 'ShadowPass.diffuse')
    g.add_edge('GBufferRaster.specRough', 'ShadowPass.specularRoughness')
    g.add_edge('GBufferRaster.emissive', 'ShadowPass.emissive')
    g.add_edge('GBufferRaster.guideNormalW', 'ShadowPass.guideNormalW')
    g.add_edge('GBufferRaster.mvec', 'ShadowPass.motionVector')
    g.add_edge('VideoRecorder', 'GBufferRaster')
    g.add_edge('PathBenchmark', 'VideoRecorder')
    g.mark_output('ShadowPass.color')
    return g

LeakTracingPrototype = render_graph_LeakTracingPrototype()
try: m.addGraph(LeakTracingPrototype)
except NameError: None
