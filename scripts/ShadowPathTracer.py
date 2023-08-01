from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_ShadowPathTracer():
    g = RenderGraph('ShadowPathTracer')
    g.create_pass('ShadowPathTracer', 'ShadowPathTracer', {})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'useTraceRayInline': False, 'useDOF': True})
    g.add_edge('ShadowPathTracer.color', 'ToneMapper.src')
    g.add_edge('VBufferRT.vbuffer', 'ShadowPathTracer.vbuffer')
    g.mark_output('ToneMapper.dst')
    return g

ShadowPathTracer = render_graph_ShadowPathTracer()
try: m.addGraph(ShadowPathTracer)
except NameError: None
