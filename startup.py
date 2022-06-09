# Graphs
from falcor import *


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

