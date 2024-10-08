from falcor import *

def render_graph_PathTracerShadowMap():
    g = RenderGraph("PathTracerShadowMap")
    PathTracerShadowMap = createPass("PathTracerShadowMap", {'samplesPerPixel': 1})
    g.addPass(PathTracerShadowMap, "PathTracerShadowMap")
    VBufferRT = createPass("VBufferRT", {'samplePattern': 'Stratified', 'sampleCount': 16, 'useAlphaTest': True})
    g.addPass(VBufferRT, "VBufferRT")
    AccumulatePass = createPass("AccumulatePass", {'enabled': True, 'precisionMode': 'Single'})
    g.addPass(AccumulatePass, "AccumulatePass")
    ToneMapper = createPass("ToneMapper", {'autoExposure': False, 'exposureCompensation': 0.0})
    g.addPass(ToneMapper, "ToneMapper")
    g.addEdge("VBufferRT.vbuffer", "PathTracerShadowMap.vbuffer")
    g.addEdge("VBufferRT.viewW", "PathTracerShadowMap.viewW")
    g.addEdge("VBufferRT.mvec", "PathTracerShadowMap.mvec")
    g.addEdge("PathTracerShadowMap.color", "AccumulatePass.input")
    g.addEdge("AccumulatePass.output", "ToneMapper.src")
    g.markOutput("ToneMapper.dst")
    return g

PathTracerShadowMap = render_graph_PathTracerShadowMap()
try: m.addGraph(PathTracerShadowMap)
except NameError: None
