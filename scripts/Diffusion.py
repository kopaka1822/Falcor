from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_Diffusion():
    g = RenderGraph('Diffusion')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back'})
    g.create_pass('VideoRecorder', 'VideoRecorder', {})
    g.create_pass('Lineart', 'Lineart', {})
    g.create_pass('GaussianBlur', 'GaussianBlur', {'kernelWidth': 3, 'sigma': 1.5})
    g.create_pass('DepthGuide', 'DepthGuide', {})
    g.create_pass('UVMaps', 'UVMaps', {})
    g.add_edge('GBufferRaster', 'VideoRecorder')
    g.add_edge('GBufferRaster.diffuseOpacity', 'Lineart.diffuseIn')
    g.add_edge('GBufferRaster.posW', 'Lineart.wPos')
    g.add_edge('GBufferRaster.faceNormalW', 'Lineart.wNormal')
    g.add_edge('Lineart.out', 'GaussianBlur.src')
    g.add_edge('GBufferRaster.linearZ', 'DepthGuide.linearZ')
    g.add_edge('GBufferRaster.texC', 'UVMaps.texC')
    g.add_edge('GBufferRaster.mtlData', 'UVMaps.material')
    g.mark_output('GBufferRaster.diffuseOpacity')
    g.mark_output('Lineart.out')
    g.mark_output('GaussianBlur.dst')
    g.mark_output('DepthGuide.out')
    g.mark_output('UVMaps.out0')
    return g

Diffusion = render_graph_Diffusion()
try: m.addGraph(Diffusion)
except NameError: None
