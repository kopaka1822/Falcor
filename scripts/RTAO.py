from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_SVAO():
    g = RenderGraph('SVAO')
    g.create_pass('RTAO', 'RTAO', {})
    g.create_pass('CrossBilateralBlur', 'CrossBilateralBlur', {})
    g.create_pass('ImageEquation', 'ImageEquation', {'formula': 'I0[xy].xxxw', 'format': 'RGBA32Float'})
    g.create_pass('TAA', 'TAA', {'alpha': 0.10000000149011612, 'colorBoxSigma': 1.0, 'antiFlicker': True})
    g.create_pass('NRD', 'NRD', {'enabled': True, 'method': 'ReblurOcclusionDiffuse', 'outputSize': 'Default', 'worldSpaceMotion': False, 'disocclusionThreshold': 2.0, 'maxIntensity': 250.0})
    g.create_pass('GBufferRT', 'GBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'texLOD': 'Mip0', 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('ImageEquation0', 'ImageEquation', {'formula': 'I0[xy].xxxw', 'format': 'RGBA32Float'})
    g.add_edge('RTAO.rayDistance', 'NRD.diffuseHitDist')
    g.add_edge('RTAO.ambient', 'CrossBilateralBlur.color')
    g.add_edge('ImageEquation.out', 'TAA.colorIn')
    g.add_edge('GBufferRT.normWRoughnessMaterialID', 'NRD.normWRoughnessMaterialID')
    g.add_edge('GBufferRT.mvec', 'NRD.mvec')
    g.add_edge('GBufferRT.posW', 'RTAO.wPos')
    g.add_edge('GBufferRT.faceNormalW', 'RTAO.faceNormal')
    g.add_edge('GBufferRT.linearZ', 'NRD.viewZ')
    g.add_edge('GBufferRT.linearZ', 'CrossBilateralBlur.linear depth')
    g.add_edge('GBufferRT.mvec', 'TAA.motionVecs')
    g.add_edge('CrossBilateralBlur.colorOut', 'ImageEquation.I0')
    g.add_edge('NRD.filteredDiffuseOcclusion', 'ImageEquation0.I0')
    g.mark_output('RTAO.ambient')
    g.mark_output('CrossBilateralBlur.colorOut')
    g.mark_output('ImageEquation.out')
    g.mark_output('TAA.colorOut')
    g.mark_output('NRD.filteredDiffuseOcclusion')
    g.mark_output('ImageEquation0.out')
    return g

SVAO = render_graph_SVAO()
try: m.addGraph(SVAO)
except NameError: None
