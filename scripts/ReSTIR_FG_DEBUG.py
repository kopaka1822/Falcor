from falcor import *

def render_graph_DefaultRenderGraph():
    g = RenderGraph('DefaultRenderGraph')
    g.create_pass('ReSTIR_FG', 'ReSTIR_FG', {})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': IOSize.Default, 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack, 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': False, 'outputSize': IOSize.Default, 'autoReset': True, 'precisionMode': AccumulatePrecision.Single, 'maxFrameCount': 0, 'overflowMode': AccumulateOverflowMode.Stop})
    g.create_pass('Composite', 'Composite', {'mode': CompositeMode.Add, 'scaleA': 1.0, 'scaleB': 1.0, 'outputFormat': ResourceFormat.RGBA32Float})
    g.create_pass('RTXDIPass', 'RTXDIPass', {'options': RTXDIOptions(mode=RTXDIMode.SpatiotemporalResampling, presampledTileCount=128, presampledTileSize=1024, storeCompactLightInfo=True, localLightCandidateCount=24, infiniteLightCandidateCount=8, envLightCandidateCount=8, brdfCandidateCount=1, brdfCutoff=0.0, testCandidateVisibility=True, biasCorrection=RTXDIBiasCorrection.Basic, depthThreshold=0.10000000149011612, normalThreshold=0.5, samplingRadius=30.0, spatialSampleCount=1, spatialIterations=5, maxHistoryLength=20, boilingFilterStrength=0.0, rayEpsilon=0.0010000000474974513, useEmissiveTextures=False, enableVisibilityShortcut=False, enablePermutationSampling=False)})
    g.create_pass('Composite0', 'Composite', {'mode': CompositeMode.Add, 'scaleA': 1.0, 'scaleB': 1.0, 'outputFormat': ResourceFormat.RGBA32Float})
    g.add_edge('VBufferRT.vbuffer', 'RTXDIPass.vbuffer')
    g.add_edge('VBufferRT.vbuffer', 'ReSTIR_FG.vbuffer')
    g.add_edge('VBufferRT.mvec', 'ReSTIR_FG.mvec')
    g.add_edge('VBufferRT.viewW', 'ReSTIR_FG.viewW')
    g.add_edge('VBufferRT.depth', 'ReSTIR_FG.rayDist')
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('ReSTIR_FG.color', 'Composite.B')
    g.add_edge('VBufferRT.mvec', 'RTXDIPass.mvec')
    g.add_edge('RTXDIPass.color', 'Composite.A')
    g.add_edge('RTXDIPass.emission', 'Composite0.B')
    g.add_edge('Composite.out', 'Composite0.A')
    g.add_edge('Composite0.out', 'AccumulatePass.input')
    g.mark_output('ToneMapper.dst')
    return g

DefaultRenderGraph = render_graph_DefaultRenderGraph()
try: m.addGraph(DefaultRenderGraph)
except NameError: None
