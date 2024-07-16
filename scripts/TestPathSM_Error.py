from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_DefaultRenderGraph():
    g = RenderGraph('DefaultRenderGraph')
    g.create_pass('FLIPPass', 'FLIPPass', {'enabled': True, 'useMagma': True, 'clampInput': False, 'isHDR': False, 'toneMapper': 'ACES', 'useCustomExposureParameters': False, 'startExposure': 0.0, 'stopExposure': 0.0, 'numExposures': 2, 'monitorWidthPixels': 3840, 'monitorWidthMeters': 0.699999988079071, 'monitorDistanceMeters': 0.699999988079071, 'computePooledFLIPValues': False, 'useRealMonitorInfo': False})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Linear', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('TestPathSM', 'TestPathSM', {'maxBounces': 8, 'computeDirect': True, 'useImportanceSampling': True, 'shadowTracingMode': 'LeakTracing'})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': True, 'outputSize': 'Default', 'autoReset': True, 'precisionMode': 'Single', 'maxFrameCount': 0, 'overflowMode': 'Stop'})
    g.create_pass('ErrorMeasurePass', 'ErrorMeasurePass', {'ReferenceImagePath': '', 'MeasurementsFilePath': '', 'IgnoreBackground': True, 'ComputeSquaredDifference': True, 'ComputeAverage': False, 'UseLoadedReference': False, 'ReportRunningError': True, 'RunningErrorSigma': 0.9950000047683716, 'SelectedOutputId': 'Difference'})
    g.create_pass('TestPathSM0', 'TestPathSM', {'maxBounces': 8, 'computeDirect': True, 'useImportanceSampling': True, 'shadowTracingMode': 'RayShadows'})
    g.create_pass('AccumulatePass0', 'AccumulatePass', {'enabled': True, 'outputSize': 'Default', 'autoReset': True, 'precisionMode': 'Single', 'maxFrameCount': 0, 'overflowMode': 'Stop'})
    g.create_pass('ToneMapper0', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Linear', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('ErrorOverlay', 'ErrorOverlay', {'ImageMode0': 'Input1', 'ImageMode1': 'Input2', 'ImageMode2': 'MSE', 'ImageMode3': 'FLIP', 'LineThickness': 1, 'LineColor': [1.0, 1.0, 1.0]})
    g.add_edge('VBufferRT.vbuffer', 'TestPathSM.vbuffer')
    g.add_edge('VBufferRT.viewW', 'TestPathSM.viewW')
    g.add_edge('TestPathSM.color', 'AccumulatePass.input')
    g.add_edge('VBufferRT.vbuffer', 'TestPathSM0.vbuffer')
    g.add_edge('VBufferRT.viewW', 'TestPathSM0.viewW')
    g.add_edge('TestPathSM0.color', 'AccumulatePass0.input')
    g.add_edge('AccumulatePass.output', 'ErrorMeasurePass.Source')
    g.add_edge('AccumulatePass0.output', 'ErrorMeasurePass.Reference')
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('AccumulatePass0.output', 'ToneMapper0.src')
    g.add_edge('ToneMapper0.dst', 'FLIPPass.testImage')
    g.add_edge('ToneMapper.dst', 'FLIPPass.referenceImage')
    g.add_edge('FLIPPass.errorMapDisplay', 'ErrorOverlay.FLIP')
    g.add_edge('ErrorMeasurePass.Output', 'ErrorOverlay.MSE')
    g.add_edge('ToneMapper.dst', 'ErrorOverlay.Input2')
    g.add_edge('ToneMapper0.dst', 'ErrorOverlay.Input1')
    g.mark_output('ErrorOverlay.output')
    return g

DefaultRenderGraph = render_graph_DefaultRenderGraph()
try: m.addGraph(DefaultRenderGraph)
except NameError: None
