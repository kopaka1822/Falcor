the scene contains 4 checkerboard quads (or impostors) viewed through differently shaped glasses.
lense_reference - real quads (non-rotating)
lense_impostor  - impostor quads (rotating)

Change the camera.target to view through different glasses:

float3(1.0, 0.0, 0.0)  - thick planar glass
float3(-1.0, 0.0, 0.0) - thin planar glass
float3(0.0, 0.0, 1.0)  - minifying glass (concave)
float3(0.0, 0.0, -1.0) - magnifying glass (convex)

You can also change the ior in the first line of each scene file.