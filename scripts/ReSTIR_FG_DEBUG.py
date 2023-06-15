from falcor import *

def render_graph_DefaultRenderGraph():
    g = RenderGraph('DefaultRenderGraph')
    g.create_pass('ReSTIR_FG', 'ReSTIR_FG', {})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': IOSize.Default, 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack, 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': False, 'outputSize': IOSize.Default, 'autoReset': True, 'precisionMode': AccumulatePrecision.Single, 'maxFrameCount': 0, 'overflowMode': AccumulateOverflowMode.Stop})
    g.add_edge('VBufferRT.vbuffer', 'ReSTIR_FG.vbuffer')
    g.add_edge('VBufferRT.mvec', 'ReSTIR_FG.mvec')
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('ReSTIR_FG.color', 'AccumulatePass.input')
    g.mark_output('ToneMapper.dst')
    return g

DefaultRenderGraph = render_graph_DefaultRenderGraph()
try: m.addGraph(DefaultRenderGraph)
except NameError: None
