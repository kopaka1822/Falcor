# Graphs
from falcor import *

def render_graph_VAODebug():
    g = RenderGraph('VAODebug')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('VAORT.dll')
    loadRenderPassLibrary('VAONonInterleaved.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    LinearizeDepth_ = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    StochasticDepthMap = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
    g.addPass(StochasticDepthMap, 'StochasticDepthMap')
    SSAO = createPass('SSAO', {'enabled': True, 'kernelSize': 8, 'noiseSize': uint2(4,4), 'radius': 0.5, 'distribution': SampleDistribution.Hammersley, 'depthMode': DepthMode.SingleDepth, 'guardBand': 64, 'thickness': 0.5})
    g.addPass(SSAO, 'SSAO')
    ConvertFormat__ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].x + I0[xy].y + I0[xy].z,0,0,0)', 'format': ResourceFormat.R8Unorm})
    g.addPass(ConvertFormat__, 'ConvertFormat__')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('GBufferRaster.depth', 'StochasticDepthMap.depthMap')
    g.addEdge('LinearizeDepth_.linearDepth', 'SSAO.depth2')
    g.addEdge('LinearizeDepth_', 'SSAO')
    g.addEdge('LinearizeDepth.linearDepth', 'SSAO.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'SSAO.normals')
    g.addEdge('StochasticDepthMap.stochasticDepth', 'SSAO.stochasticDepth')
    g.addEdge('SSAO.ambientMap', 'ConvertFormat__.I0')
    g.addEdge('ConvertFormat__.out', 'CrossBilateralBlur.color')
    g.addEdge('SSAO', 'ConvertFormat__')
    g.addEdge('ConvertFormat__', 'CrossBilateralBlur')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    g.markOutput('SSAO.ambientMap')
    return g
m.addGraph(render_graph_VAODebug())
from falcor import *

def render_graph_VAONonInterleaved():
    g = RenderGraph('VAONonInterleaved')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('VAORT.dll')
    loadRenderPassLibrary('VAONonInterleaved.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    LinearizeDepth_ = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    ConvertFormat__ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].x + I0[xy].y + I0[xy].z,0,0,0)', 'format': ResourceFormat.R8Unorm})
    g.addPass(ConvertFormat__, 'ConvertFormat__')
    VAONonInterleaved = createPass('VAONonInterleaved', {'radius': 0.5, 'primaryDepthMode': DepthMode.SingleDepth, 'secondaryDepthMode': DepthMode.StochasticDepth, 'exponent': 1.2999999523162842, 'rayPipeline': False, 'guardBand': 64, 'thickness': 0.5})
    g.addPass(VAONonInterleaved, 'VAONonInterleaved')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    StochasticDepthMap_ = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
    g.addPass(StochasticDepthMap_, 'StochasticDepthMap_')
    VAONonInterleaved2 = createPass('VAONonInterleaved2')
    g.addPass(VAONonInterleaved2, 'VAONonInterleaved2')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('ConvertFormat__.out', 'CrossBilateralBlur.color')
    g.addEdge('ConvertFormat__', 'CrossBilateralBlur')
    g.addEdge('LinearizeDepth_.linearDepth', 'VAONonInterleaved.depth2')
    g.addEdge('LinearizeDepth.linearDepth', 'VAONonInterleaved.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'VAONonInterleaved.normals')
    g.addEdge('LinearizeDepth_', 'VAONonInterleaved')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.depth', 'StochasticDepthMap_.depthMap')
    g.addEdge('VAONonInterleaved.accessStencil', 'StochasticDepthMap_.stencilMask')
    g.addEdge('StochasticDepthMap_.stochasticDepth', 'VAONonInterleaved2.stochasticDepth')
    g.addEdge('VAONonInterleaved.ao', 'VAONonInterleaved2.ao')
    g.addEdge('LinearizeDepth_.linearDepth', 'VAONonInterleaved2.depth2')
    g.addEdge('LinearizeDepth.linearDepth', 'VAONonInterleaved2.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'VAONonInterleaved2.normals')
    g.addEdge('VAONonInterleaved2.ao', 'ConvertFormat__.I0')
    g.addEdge('VAONonInterleaved.stencil', 'VAONonInterleaved2.aoStencil')
    g.addEdge('StochasticDepthMap_', 'VAONonInterleaved2')
    g.addEdge('VAONonInterleaved2', 'ConvertFormat__')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    g.markOutput('StochasticDepthMap_.stochasticDepth')
    return g
m.addGraph(render_graph_VAONonInterleaved())
from falcor import *

def render_graph_HBAOPlusInterleaved():
    g = RenderGraph('HBAOPlusInterleaved')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('VAORT.dll')
    loadRenderPassLibrary('VAONonInterleaved.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    DeinterleaveTexture = createPass('DeinterleaveTexture')
    g.addPass(DeinterleaveTexture, 'DeinterleaveTexture')
    HBAOPlusInterleaved = createPass('HBAOPlusInterleaved', {'radius': 1.0, 'depthMode': DepthMode.DualDepth, 'depthBias': 0.10000000149011612, 'exponent': 2.0})
    g.addPass(HBAOPlusInterleaved, 'HBAOPlusInterleaved')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    StochasticDepthMap = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
    g.addPass(StochasticDepthMap, 'StochasticDepthMap')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    LinearizeDepth_ = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    InterleaveTexture = createPass('InterleaveTexture')
    g.addPass(InterleaveTexture, 'InterleaveTexture')
    DeinterleaveTexture_ = createPass('DeinterleaveTexture')
    g.addPass(DeinterleaveTexture_, 'DeinterleaveTexture_')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].x * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'StochasticDepthMap.depthMap')
    g.addEdge('StochasticDepthMap.stochasticDepth', 'HBAOPlusInterleaved.stochasticDepth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('HBAOPlusInterleaved.ambientMap', 'InterleaveTexture.texIn')
    g.addEdge('LinearizeDepth.linearDepth', 'DeinterleaveTexture.texIn')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('DeinterleaveTexture.texOut', 'HBAOPlusInterleaved.depth')
    g.addEdge('LinearizeDepth_.linearDepth', 'DeinterleaveTexture_.texIn')
    g.addEdge('LinearizeDepth_', 'DeinterleaveTexture')
    g.addEdge('DeinterleaveTexture_.texOut', 'HBAOPlusInterleaved.depth2')
    g.addEdge('DeinterleaveTexture', 'DeinterleaveTexture_')
    g.addEdge('DeinterleaveTexture_', 'HBAOPlusInterleaved')
    g.addEdge('InterleaveTexture.texOut', 'CrossBilateralBlur.color')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('HBAOPlusInterleaved', 'InterleaveTexture')
    g.addEdge('InterleaveTexture', 'CrossBilateralBlur')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'HBAOPlusInterleaved.normals')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    return g
m.addGraph(render_graph_HBAOPlusInterleaved())
from falcor import *

def render_graph_RTAO():
    g = RenderGraph('RTAO')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('VAORT.dll')
    loadRenderPassLibrary('VAONonInterleaved.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    RTAO = createPass('RTAO')
    g.addPass(RTAO, 'RTAO')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    RTAODenoiser = createPass('RTAODenoiser')
    g.addPass(RTAODenoiser, 'RTAODenoiser')
    g.addEdge('RTAODenoiser.denoisedOut', 'Diffuse.I0')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('GBufferRaster.posW', 'RTAO.wPos')
    g.addEdge('GBufferRaster.faceNormalW', 'RTAO.faceNormal')
    g.addEdge('RTAO.ambient', 'RTAODenoiser.aoImage')
    g.addEdge('RTAODenoiser.denoisedOut', 'Ambient.I0')
    g.addEdge('RTAO.rayDistance', 'RTAODenoiser.rayDistance')
    g.addEdge('GBufferRaster.faceNormalW', 'RTAODenoiser.normal')
    g.addEdge('GBufferRaster.linearZ', 'RTAODenoiser.linearDepth')
    g.addEdge('GBufferRaster.mvec', 'RTAODenoiser.mVec')
    g.addEdge('GBufferRaster.depth', 'RTAODenoiser.depth')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    return g
m.addGraph(render_graph_RTAO())
from falcor import *

def render_graph_VAORT():
    g = RenderGraph('VAORT')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('VAORT.dll')
    loadRenderPassLibrary('VAONonInterleaved.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    VAORT = createPass('VAORT', {'radius': 0.5, 'guardBand': 64, 'thickness': 0.5})
    g.addPass(VAORT, 'VAORT')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].x * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.faceNormalW', 'VAORT.normals')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('LinearizeDepth.linearDepth', 'VAORT.depth')
    g.addEdge('VAORT.ambientMap', 'CrossBilateralBlur.color')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    return g
m.addGraph(render_graph_VAORT())

# Scene
m.loadScene('D:/scenes/obj/sponza/sponza.obj')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useGridVolumes=True)
m.scene.camera.position = float3(22.887949,4.794842,2.332704)
m.scene.camera.target = float3(21.943356,4.708484,2.016026)
m.scene.camera.up = float3(0.000738,1.000000,0.000248)
m.scene.cameraSpeed = 1.0

# Window Configuration
m.resizeSwapChain(2048, 1208)
m.ui = True

# Clock Settings
m.clock.time = 0
m.clock.framerate = 0
# If framerate is not zero, you can use the frame property to set the start frame
# m.clock.frame = 0
m.clock.pause()

# Frame Capture
m.frameCapture.outputDir = '.'
m.frameCapture.baseFilename = 'Mogwai'

