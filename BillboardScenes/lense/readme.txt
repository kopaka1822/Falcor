the scene contains 4 checkerboard quads (or impostors) viewed through differently shaped glasses.

closeup scenes: distance to glass = 2, distance to quad = 2.5:
lense_reference - real quads (non-rotating)
lense_impostor  - impostor quads (rotating)

far scenes: distance to glass = 2, distance to quad = 5
lense_reference_far - real quads (non-rotating)
lense_impostor_far  - impostor quads (rotating)

Change the camera.target to view through different glasses:

float3(1.0, 0.0, 0.0)  - thick planar glass
float3(-1.0, 0.0, 0.0) - thin planar glass
float3(0.0, 0.0, 1.0)  - minifying glass (concave)
float3(0.0, 0.0, -1.0) - magnifying glass (convex)

You can also change the ior in the first line of each scene file.

Important note:
By default, impostors are y-axis aligned. To remove the axis alignment, go to the GetBillboardTangentSpace() function
in Helper.slang and comment out the if(yAlign) ... line
Note that the computed bounding box will be too small in the XZ direction now. However, this is only visible when looking at the bounding box from above.