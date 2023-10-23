from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_CamPathTest():
    g = RenderGraph('CamPathTest')
    g.create_pass('CameraPath', 'CameraPath', {})
    g.create_pass('MinimalPathTracer', 'MinimalPathTracer', {'maxBounces': 0, 'computeDirect': True, 'useImportanceSampling': True})
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'useTraceRayInline': False, 'useDOF': True})
    g.add_edge('VBufferRT.viewW', 'MinimalPathTracer.viewW')
    g.add_edge('VBufferRT.vbuffer', 'MinimalPathTracer.vbuffer')
    g.add_edge('CameraPath', 'VBufferRT')
    g.mark_output('MinimalPathTracer.color')
    return g

CamPathTest = render_graph_CamPathTest()
try: m.addGraph(CamPathTest)
except NameError: None
