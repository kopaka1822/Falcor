#pragma once
#include "gli/gli/gli.hpp"
#include "npy.h"
#include <iostream>

void convert_to_numpy(std::string raster_image, std::string ray_image, std::string inScreen, std::string sphere_start, int index = 0)
{
    static constexpr bool useSphereStart = true;

    gli::texture2d_array texRaster(gli::load(raster_image));
    gli::texture2d_array texRay(gli::load(ray_image));
    gli::texture2d_array texSphereStart(gli::load(sphere_start));
    //gli::texture2d_array texInstances(gli::load(argv[3]));
    //gli::texture2d_array texCurInstance(gli::load(argv[4]));
    gli::texture2d_array texInScreen(gli::load(inScreen));

    assert(texRaster.format() == gli::FORMAT_R32_SFLOAT_PACK32);
    //assert(texRaster.layers() == 8);
    assert(texRaster.layers() == texRay.layers());
    assert(texRaster.extent() == texRay.extent());

    auto width = texRaster.extent().x;
    auto height = texRaster.extent().y;
    auto nSamples = texRaster.layers();

    // prepare data in float vector
    std::vector<float> rasterSamples;
    std::vector<float> raySamples;
    std::vector<float> sphereStartSamples;
    //std::vector<int> sameInstance; // 1 if raster sample instance is same as pixel center
    //std::vector<int> rasterInstanceDiffs; // 0 if both neighbors are same instance, 1 if only one is same instance, 2 if both are different instances
    std::vector<int> pixelXY; // x,y coordinates of pixel
    //std::vector<int> numInvalid; // number of invalid samples
    rasterSamples.reserve(width * height * nSamples);
    raySamples.reserve(width * height * nSamples);
    sphereStartSamples.reserve(width * height * nSamples);
    //sameInstance.reserve(width * height * nSamples);
    //rasterInstanceDiffs.reserve(width * height * nSamples);
    pixelXY.reserve(width * height * 2);

    std::vector<float> raster;
    raster.resize(nSamples);
    std::vector<float> ray;
    ray.resize(nSamples);
    std::vector<float> sphereStart;
    sphereStart.resize(nSamples);
    //std::vector<uint32_t> instances;
    //instances.resize(nSamples);

    // fetch function for tex raster (and tex ray)
    auto fetch = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRaster.format()).Fetch;
    //auto fetchInt = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texInstances.format()).Fetch;
    auto fetchBool = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texInScreen.format()).Fetch;
    int nTrivial = 0;
    int nOutsideScreen = 0;
    int dubiousSamples = 0;
    float equalityThreshold = 0.01; // assume ray and raster are equal when the values are within this threshold (reduce noise in training data)

    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
    {
        //const auto curInstance = (uint32_t)fetchInt(texCurInstance, gli::extent2d(x, y), 0, 0, 0).r;;
        bool inScreen = fetchBool(texInScreen, gli::extent2d(x, y), 0, 0, 0).r > 0.5f;

        if (!inScreen) // skip samples that are partly outside the screen
        {
            nOutsideScreen++;
            continue;
        }

        bool trivial = true; // assume trivial (all raster samples are within range)
        // obtain sample data
        for (size_t i = 0; i < nSamples; ++i)
        {
            raster[i] = fetch(texRaster, gli::extent2d(x, y), i, 0, 0).r;
            ray[i] = fetch(texRay, gli::extent2d(x, y), i, 0, 0).r;
            sphereStart[i] = fetch(texSphereStart, gli::extent2d(x, y), i, 0, 0).r;
            //instances[i] = (uint32_t)fetchInt(texInstances, gli::extent2d(x, y), i, 0, 0).r;
            if (useSphereStart && raster[i] > sphereStart[i]) trivial = false; // raster is outside of range (not trivial)
            if (!useSphereStart && raster[i] > 1.0f) trivial = false;
        }

        if(trivial)
        {
            nTrivial++;
            continue;
        }

        // reduce noise
        bool isDubious = false;
        for (size_t i = 0; i < nSamples; ++i)
        {
            // set ray = raster if they are close enough
            if (std::abs(raster[i] - ray[i]) <= equalityThreshold)
            {
                ray[i] = raster[i];
            }

            if (raster[i] < ray[i])
            {
                dubiousSamples++;
                //std::cout << "doubious sample at: " << x << ", " << y << " id: " << i << " diff: " << ray[i] - raster[i] << '\n';
                isDubious = true;
                break;
            }
        }

        if (isDubious) continue; // less noise in training data

        // write to raster data and ray data
        rasterSamples.insert(rasterSamples.end(), raster.begin(), raster.end());
        raySamples.insert(raySamples.end(), ray.begin(), ray.end());
        sphereStartSamples.insert(sphereStartSamples.end(), sphereStart.begin(), sphereStart.end());
        pixelXY.push_back(x);
        pixelXY.push_back(y);



        //for (size_t i = 0; i < nSamples; ++i)
        //{
        //    sameInstance.push_back(instances[i] == curInstance ? 1 : 0);
        //    uint32_t instDiff = 0;
        //    instDiff += instances[i] != instances[(i + nSamples - 1) % nSamples] ? 1 : 0;
        //    instDiff += instances[i] != instances[(i + 1) % nSamples] ? 1 : 0;
        //    rasterInstanceDiffs.push_back(instDiff);
        //}

        // determine if ray is equal to raster
        //auto same = std::equal(raster.begin(), raster.end(), ray.begin());
        // set same to true if all values are almost equal (within 0.01f)
    }

    // print out number of all samples, empty samples and skipped samples
    auto nRemainingSamples = pixelXY.size() / 2;
    std::cout << "Total samples: " << width * height << std::endl;
    std::cout << "Trivial samples: " << nTrivial << std::endl;
    std::cout << "Outside screen samples: " << nOutsideScreen << std::endl;
    std::cout << "Dubious samples (ray > raster): " << dubiousSamples << std::endl;
    std::cout << "Remaining samples: " << nRemainingSamples << std::endl;

    auto strIndex = std::to_string(index);

    // write to numpy files
    unsigned long shape[] = { (unsigned long)nRemainingSamples, (unsigned long)nSamples }; // shape = rows, columns
    npy::SaveArrayAsNumpy("raster" + strIndex + ".npy", false, 2, shape, rasterSamples);
    npy::SaveArrayAsNumpy("ray" + strIndex + ".npy", false, 2, shape, raySamples);
    npy::SaveArrayAsNumpy("sphereStart" + strIndex + ".npy", false, 2, shape, sphereStartSamples);

    //npy::SaveArrayAsNumpy("sameInstance.npy", false, 2, shape, sameInstance);
    //npy::SaveArrayAsNumpy("rasterInstanceDiffs.npy", false, 2, shape, rasterInstanceDiffs);

    //npy::SaveArrayAsNumpy("same.npy", false, 1, shape, sameAsRay);

    // adjust shape for pixelXY
    //unsigned long shapeXY[] = { (unsigned long)nRemainingSamples, 2ul };
    //npy::SaveArrayAsNumpy("pixelXY" + strIndex + ".npy", false, 2, shapeXY, pixelXY);
}
