from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_ReSTIR_GI_Shadow():
    g = RenderGraph('ReSTIR_GI_Shadow')
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': True, 'outputSize': 'Default', 'autoReset': True, 'precisionMode': 'Single', 'maxFrameCount': 0, 'overflowMode': 'Stop'})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('ReSTIR_GI_Shadow', 'ReSTIR_GI_Shadow', {})
    g.create_pass('DLSSPass', 'DLSSPass', {'enabled': True, 'outputSize': 'Default', 'profile': 'Balanced', 'motionVectorScale': 'Absolute', 'isHDR': True, 'sharpness': 0.0, 'exposure': 0.0})
    g.create_pass('ToneMapper0', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('ReSTIR_GI_Shadow.color', 'AccumulatePass.input')
    g.add_edge('VBufferRT.vbuffer', 'ReSTIR_GI_Shadow.vbuffer')
    g.add_edge('VBufferRT.mvec', 'ReSTIR_GI_Shadow.mvec')
    g.add_edge('ReSTIR_GI_Shadow.color', 'DLSSPass.color')
    g.add_edge('VBufferRT.depth', 'DLSSPass.depth')
    g.add_edge('VBufferRT.mvec', 'DLSSPass.mvec')
    g.add_edge('DLSSPass.output', 'ToneMapper0.src')
    g.mark_output('ToneMapper.dst')
    g.mark_output('ToneMapper0.dst')
    return g

ReSTIR_GI_Shadow = render_graph_ReSTIR_GI_Shadow()
try: m.addGraph(ReSTIR_GI_Shadow)
except NameError: None