# Billboard Ray Tracing (Falcor 4.3)

## Setup

1. Follow the setup intructions for [Falcor](falcor_README.md).
2. In Visual Studio: Set Mogwai as startup project and use "-sF:\git\Falcor\startup.py" as command arguments (right click mogwai in the solution explorer->properties->configuration properties/debugging/command arguments).
3. Start Mogwai with DebugD3D12 or ReleaseD3D12
4. Load test scenes with File->Load Scene. Choose any .pyscene from the BillboardScenes/ directory.

## The Billboard Ray Tracer

The billboard ray tracer is a simple 1 sample per pixel (non-gbuffer) ray tracer that reflects on specular surface (if roughness == 0) and refracts on transmittive surfaces. If both criterias are met, the refraction is preferred and an environment lookup is used instead of a ray traced reflection.

### Falcor UI
* Ray footpint mode: Choose between ray differentials and Mip0 filtering
* Billboard type: Selects the active billboard type (Impostor, Particle, or Spherical). This will be selected automatically when loading a new scene.
* Reflection correction: ray origin correction for reflections as discussed in the article
* Refraction correction: ray origin correction for refractions as discussed in the article
* Deep shadow samples: number of shadow samples that will be used for the self shadowing of particles. For a real-time application, this should be replaced with filterable deep shadow maps like fourier opacity maps. However, for simplicity we used (expensive) ray traced shadows.
* Shadow: enables shadows
* Random Colors: gives each billboard a random color. This can be used to see how well weighted-blended OIT behaves even with different colored particles.
* Soft Particles (Particle only): Uses the soft particle technique to smoothly fade out particles near objects
* Use weighted OIT (Particle only): Uses weighted OIT with any-hit ray tracing (approximate result)

### Current constraints
* Only a single billboard type is supported at once. The active type is defined in BILLBOARD_TYPE and can be: BILLBOARD_TYPE_IMPOSTOR, BILLBOARD_TYPE_PARTICLE or BILLBOARD_TYPE_SPHERICAL.
* Only a single material can be used for all billboards. This material is the last defined material of the scene. Depending on the name of the material, the appropriate billboard type will be chosen when the scene is loaded.
* All billboards are static.

### Source Code

The full source for the billboard ray tracer is located at Source\RenderPasses\BillboardRayTracer\

**BillboardRayTracer.rt.slang**:
Contains all shader kernels:
* miss, triangleAnyHit, triangleClosestHit: shader kernels for triangular geometry
* boxIntersect: intersection shader for billboards
* boxAnyHit: any-hit shader for billboard (only used for any-hit ray tracing with spherical billboards)
* boxClosestHit: closest-hit shader for impostors and billboard particles (not spherical billboards)
* traceBillboards: helper function to handle all billboards in the given ray interval [Ray.TMin, Ray.TMax]. 
* rayGen: ray generation shader for particles as discussed in the article: First, a non-billboard ray is shot to determine the unoccupied ray-interval for soft particles and spherical billboards. Then, traceBillboards is called for this ray-interval which also updates the color and transmittance. Then the color for the triangle hit point from the non-billboard ray is calculated. Then, the ray gets reflected, refracted or aborted based on the material properites and the remaining transmittance.

**Helper.slang**:
A collection of helper functions to fetch billboard data:
* GetBillboardTangentSpace: computes the billboard tangent space
* getSoftParticleContrast: contrast function from soft particles
* getBillboardVertexData: initializes the vertex data for impostors and normal particles
* getSphericalBillboardVertexData: initializes the vertex data for spherical billboards
* shadeBillboard: forward shades a billboard and uses ray traced shadows for the primary light. If a particle or spherical billboard is shaded, the shadow will be computed for different locations inside the sphere to create soft self shadowing
* shade: forward shades a surface and uses ray traced shadows for the primary light.
* getDepthWeight: weight function from weighted-blended OIT

**RayDifferentials.slang**:
* computeBillboardShadingData: computes the appropriate texture differentials dUVdx, dUVdy for normal and spherical billboards.
* prepareBillboardShadingData: adjusts the fetched particle opacity after calling _computeBillboardShadingData_

**Shadow.slang**:
This file includes the function to compute a hard ray traced shadow with ray queries.

