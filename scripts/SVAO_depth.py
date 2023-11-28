from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_SVAO():
    g = RenderGraph('SVAO')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back'})
    g.create_pass('ForwardLighting', 'ForwardLighting', {'envMapIntensity': 1.0, 'ambientIntensity': 0.5, 'lightIntensity': 0.5, 'envMapMirror': False})
    g.create_pass('LinearDepth1', 'LinearizeDepth', {'depthFormat': 'R32Float'})
    g.create_pass('EnvMapPass', 'EnvMapPass', {})
    g.create_pass('RayShadow', 'RayShadow', {})
    g.create_pass('SVAO', 'SVAO', {'radius': 0.20000000298023224, 'primaryDepthMode': 'DualDepth', 'secondaryDepthMode': 'SingleDepth', 'exponent': 2.0})
    g.create_pass('DepthPeeling', 'DepthPeeling', {'cullMode': 'Back', 'depthFormat': 'D32Float', 'minSeparationDistance': 0.01})
    g.create_pass('LinearDepthPeel', 'LinearizeDepth', {'depthFormat': 'R32Float'})
    g.create_pass('GuardBand', 'GuardBand', {'guardBand': 64})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Linear', 'clamp': False, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('Ambient', 'ImageEquation', {'formula': 'I0[xy].rrra', 'format': 'RGBA32Float'})
    g.create_pass('Diffuse', 'ImageEquation', {'formula': 'I0[xy].r * I1[xy]', 'format': 'RGBA32Float'})
    g.create_pass('TemporalDepthPeel', 'TemporalDepthPeel', {'minSeparationDistance': 0.5})
    g.create_pass('CompressNormals', 'CompressNormals', {'viewSpace': True, 'use16Bit': True})
    g.create_pass('DepthSelect', 'Switch', {'count': 2, 'selected': 0, 'i0': 'Temporal', 'i1': 'Peel'})
    g.create_pass('CrossBilateralBlur', 'CrossBilateralBlur', {})
    g.add_edge('GBufferRaster.depth', 'ForwardLighting.depth')
    g.add_edge('EnvMapPass.color', 'ForwardLighting.color')
    g.add_edge('RayShadow.visibility', 'ForwardLighting.visibilityBuffer')
    g.add_edge('GBufferRaster.posW', 'RayShadow.posW')
    g.add_edge('GBufferRaster.faceNormalW', 'RayShadow.normalW')
    g.add_edge('GBufferRaster.depth', 'EnvMapPass.depth')
    g.add_edge('GBufferRaster.depth', 'LinearDepth1.depth')
    g.add_edge('DepthPeeling.depth2', 'LinearDepthPeel.depth')
    g.add_edge('GBufferRaster.depth', 'SVAO.gbufferDepth')
    g.add_edge('ToneMapper.dst', 'SVAO.color')
    g.add_edge('LinearDepth1.linearDepth', 'DepthPeeling.linearZ')
    g.add_edge('GuardBand', 'GBufferRaster')
    g.add_edge('ForwardLighting.color', 'ToneMapper.src')
    g.add_edge('ToneMapper.dst', 'Diffuse.I1')
    g.add_edge('GBufferRaster.mvec', 'TemporalDepthPeel.mvec')
    g.add_edge('LinearDepth1.linearDepth', 'TemporalDepthPeel.linearZ')
    g.add_edge('GBufferRaster.faceNormalW', 'CompressNormals.normalW')
    g.add_edge('CompressNormals.normalOut', 'SVAO.normals')
    g.add_edge('LinearDepthPeel.linearDepth', 'DepthSelect.i1')
    g.add_edge('TemporalDepthPeel.depth2', 'DepthSelect.i0')
    g.add_edge('DepthSelect.out', 'SVAO.depth2')
    g.add_edge('LinearDepth1.linearDepth', 'SVAO.depth')
    g.add_edge('SVAO.ao', 'CrossBilateralBlur.color')
    g.add_edge('LinearDepth1.linearDepth', 'CrossBilateralBlur.linear depth')
    g.add_edge('CrossBilateralBlur.colorOut', 'Ambient.I0')
    g.add_edge('CrossBilateralBlur.colorOut', 'Diffuse.I0')
    g.mark_output('Ambient.out')
    g.mark_output('Diffuse.out')
    g.mark_output('LinearDepth1.linearDepth')
    g.mark_output('TemporalDepthPeel.depth2')
    g.mark_output('LinearDepthPeel.linearDepth')
    return g

SVAO = render_graph_SVAO()
try: m.addGraph(SVAO)
except NameError: None
