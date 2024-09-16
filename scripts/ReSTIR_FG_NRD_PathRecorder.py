from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_ReSTIR_FG_NRD():
    g = RenderGraph('ReSTIR_FG_NRD')
    g.create_pass('ModulateIllumination', 'ModulateIllumination', {'useEmission': True, 'useDiffuseReflectance': True, 'useDiffuseRadiance': True, 'useSpecularReflectance': True, 'useSpecularRadiance': True, 'useDeltaReflectionEmission': True, 'useDeltaReflectionReflectance': True, 'useDeltaReflectionRadiance': True, 'useDeltaTransmissionEmission': True, 'useDeltaTransmissionReflectance': True, 'useDeltaTransmissionRadiance': True, 'useResidualRadiance': True, 'outputSize': 'Default'})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': False, 'outputSize': 'Default', 'autoReset': True, 'precisionMode': 'Single', 'maxFrameCount': 0, 'overflowMode': 'Stop'})
    g.create_pass('DLSSPass', 'DLSSPass', {'enabled': True, 'outputSize': 'Default', 'profile': 'Balanced', 'motionVectorScale': 'Relative', 'isHDR': True, 'useJitteredMV': False, 'sharpness': 0.0, 'exposure': 0.0})
    g.create_pass('ReSTIR_FG', 'ReSTIR_FG', {})
    g.create_pass('NRD', 'NRD_Normal', {'enabled': True, 'method': 'RelaxDiffuseSpecular', 'outputSize': 'Default', 'worldSpaceMotion': True, 'disocclusionThreshold': 0.5099999904632568, 'maxIntensity': 25.0, 'diffusePrepassBlurRadius': 32.0, 'specularPrepassBlurRadius': 8.0, 'diffuseMaxAccumulatedFrameNum': 31, 'specularMaxAccumulatedFrameNum': 31, 'diffuseMaxFastAccumulatedFrameNum': 2, 'specularMaxFastAccumulatedFrameNum': 2, 'diffusePhiLuminance': 1.0, 'specularPhiLuminance': 0.699999988079071, 'diffuseLobeAngleFraction': 2.0, 'specularLobeAngleFraction': 0.800000011920929, 'roughnessFraction': 0.5, 'diffuseHistoryRejectionNormalThreshold': 0.0, 'specularVarianceBoost': 0.0, 'specularLobeAngleSlack': 10.0, 'disocclusionFixEdgeStoppingNormalPower': 8.0, 'disocclusionFixMaxRadius': 32.0, 'disocclusionFixNumFramesToFix': 2, 'historyClampingColorBoxSigmaScale': 2.5, 'spatialVarianceEstimationHistoryThreshold': 4, 'atrousIterationNum': 6, 'minLuminanceWeight': 0.0, 'depthThreshold': 0.019999999552965164, 'luminanceEdgeStoppingRelaxation': 0.5, 'normalEdgeStoppingRelaxation': 0.30000001192092896, 'roughnessEdgeStoppingRelaxation': 0.30000001192092896, 'enableAntiFirefly': True, 'enableReprojectionTestSkippingWithoutMotion': False, 'enableSpecularVirtualHistoryClamping': True, 'enableRoughnessEdgeStopping': True, 'enableMaterialTestForDiffuse': False, 'enableMaterialTestForSpecular': False})
    g.create_pass('GBufferRT', 'GBufferRT', {'outputSize': 'Default', 'samplePattern': 'Stratified', 'sampleCount': 32, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'texLOD': 'Mip0', 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('VideoRecorder', 'VideoRecorder', {})
    g.add_edge('ReSTIR_FG.residualRadiance', 'ModulateIllumination.residualRadiance')
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('NRD.filteredSpecularRadianceHitDist', 'ModulateIllumination.specularRadiance')
    g.add_edge('ReSTIR_FG.emission', 'ModulateIllumination.emission')
    g.add_edge('ReSTIR_FG.diffuseReflectance', 'ModulateIllumination.diffuseReflectance')
    g.add_edge('ReSTIR_FG.specularReflectance', 'ModulateIllumination.specularReflectance')
    g.add_edge('NRD.filteredDiffuseRadianceHitDist', 'ModulateIllumination.diffuseRadiance')
    g.add_edge('ReSTIR_FG.diffuseRadiance', 'NRD.diffuseRadianceHitDist')
    g.add_edge('ReSTIR_FG.specularRadiance', 'NRD.specularRadianceHitDist')
    g.add_edge('GBufferRT.linearZ', 'NRD.viewZ')
    g.add_edge('GBufferRT.normWRoughnessMaterialID', 'NRD.normWRoughnessMaterialID')
    g.add_edge('ModulateIllumination.output', 'DLSSPass.color')
    g.add_edge('DLSSPass.output', 'AccumulatePass.input')
    g.add_edge('GBufferRT.depth', 'DLSSPass.depth')
    g.add_edge('GBufferRT.mvecW', 'NRD.mvec')
    g.add_edge('GBufferRT.mvec', 'DLSSPass.mvec')
    g.add_edge('GBufferRT.vbuffer', 'ReSTIR_FG.vbuffer')
    g.add_edge('GBufferRT.mvec', 'ReSTIR_FG.mvec')
    g.add_edge('VideoRecorder', 'GBufferRT')
    g.mark_output('ToneMapper.dst')
    g.mark_output('AccumulatePass.output')
    return g

ReSTIR_FG_NRD = render_graph_ReSTIR_FG_NRD()
try: m.addGraph(ReSTIR_FG_NRD)
except NameError: None
