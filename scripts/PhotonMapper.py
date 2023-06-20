from falcor import *

def render_graph_PhotonMapper():
    g = RenderGraph('PhotonMapper')
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack, 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': IOSize.Default, 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': True, 'outputSize': IOSize.Default, 'autoReset': True, 'precisionMode': AccumulatePrecision.Single, 'maxFrameCount': 0, 'overflowMode': AccumulateOverflowMode.Stop})
    g.create_pass('PhotonMapper', 'PhotonMapper', {})
    g.add_edge('VBufferRT.vbuffer', 'PhotonMapper.vbuffer')
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('PhotonMapper.color', 'AccumulatePass.input')
    g.mark_output('ToneMapper.dst')
    return g

PhotonMapper = render_graph_PhotonMapper()
try: m.addGraph(PhotonMapper)
except NameError: None
