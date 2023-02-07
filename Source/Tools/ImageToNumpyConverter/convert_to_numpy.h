#pragma once
#include "gli/gli/gli.hpp"
#include "npy.h"
#include <iostream>

void convert_to_numpy(std::string raster_image, std::string ray_image, std::string force_ray, std::string sphere_start, std::string raster_ao, std::string ray_ao, int index = 0)
{
    static constexpr size_t NUM_STEPS = 4;
    static constexpr size_t NUM_DIRECTIONS = 8;
    static constexpr size_t NUM_SAMPLES = NUM_STEPS * NUM_DIRECTIONS;

    // indicates if we export training data (without pixelXY information and dubious samples) or evaluation data (pixel information with all samples)
    static constexpr bool IsTraining = false;

    static constexpr bool useSphereStart = true;
    static constexpr bool useDubiousSamples = !IsTraining; // false for training, true for evaluation

    gli::texture2d_array texRaster(gli::load(raster_image));
    gli::texture2d_array texRay(gli::load(ray_image));
    gli::texture2d_array texSphereStart(gli::load(sphere_start));
    //gli::texture2d_array texInstances(gli::load(argv[3]));
    //gli::texture2d_array texCurInstance(gli::load(argv[4]));
    gli::texture2d_array texForceRay(gli::load(force_ray));

    gli::texture2d_array texRasterAO(gli::load(raster_ao));
    gli::texture2d_array texRayAO(gli::load(ray_ao));


    assert(texRaster.format() == gli::FORMAT_R32_SFLOAT_PACK32);
    //assert(texRaster.layers() == 8);
    assert(texRaster.layers() == texRay.layers());
    assert(texRaster.extent() == texRay.extent());

    auto width = texRaster.extent().x;
    auto height = texRaster.extent().y;
    auto nSamples = texRaster.layers();
    assert(nSamples == NUM_SAMPLES);

    // prepare data in float vector
    std::array<std::vector<float>, NUM_STEPS> rasterSamples;
    std::array<std::vector<float>, NUM_STEPS> raySamples;
    //std::vector<float> sphereStartSamples;
    //std::vector<int> sameInstance; // 1 if raster sample instance is same as pixel center
    //std::vector<int> rasterInstanceDiffs; // 0 if both neighbors are same instance, 1 if only one is same instance, 2 if both are different instances
    std::array<std::vector<int>, NUM_STEPS> pixelXYi; // x,y coordinates of pixel
    std::array<std::vector<uint8_t>, NUM_STEPS> required; // 1 if ray tracing is required, 0 if not
    std::array<std::vector<float>, NUM_STEPS> weight; // sample weights
    std::vector<int> forcedPixels; // XYi coordinates of the pixel that was forced to be ray traced (or is invalid, in which case the saved ao is 1.0)
    //std::vector<int> numInvalid; // number of invalid samples
    for(size_t i = 0; i < NUM_STEPS; ++i)
    {
        rasterSamples[i].reserve(width * height * nSamples);
        raySamples[i].reserve(width * height * nSamples);
        //sameInstance.reserve(width * height * nSamples);
        //rasterInstanceDiffs.reserve(width * height * nSamples);
        pixelXYi[i].reserve(width * height * 3);
        required[i].reserve(width * height);
        weight[i].reserve(width * height);
    }
    forcedPixels.reserve(width * height * 3 * NUM_SAMPLES);

    // local arrays
    std::array<float, NUM_SAMPLES> raster;
    std::array<float, NUM_SAMPLES> ray;
    std::array<float, NUM_SAMPLES> sphereStart;
    std::array<float, NUM_SAMPLES> aoDiff; // difference in AO values
    std::array<bool, NUM_SAMPLES> forceRay;
    //std::vector<uint32_t> instances;
    //instances.resize(nSamples);

    // fetch function for tex raster (and tex ray)
    auto fetch = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRaster.format()).Fetch;
    auto fetchAO = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRasterAO.format()).Fetch;
    //auto fetchInt = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texInstances.format()).Fetch;
    auto fetchBool = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texForceRay.format()).Fetch;
    int dubiousSamples = 0;
    float equalityThreshold = 0.01; // assume ray and raster are equal when the values are within this threshold (reduce noise in training data)

    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
    {
        for (size_t i = 0; i < nSamples; ++i)
        {
            raster[i] = fetch(texRaster, gli::extent2d(x, y), i, 0, 0).r;
            ray[i] = fetch(texRay, gli::extent2d(x, y), i, 0, 0).r;
            sphereStart[i] = fetch(texSphereStart, gli::extent2d(x, y), i, 0, 0).r;
            //instances[i] = (uint32_t)fetchInt(texInstances, gli::extent2d(x, y), i, 0, 0).r;
            aoDiff[i] = abs(fetchAO(texRasterAO, gli::extent2d(x, y), i, 0, 0).r - fetchAO(texRayAO, gli::extent2d(x, y), i, 0, 0).r);
            assert(aoDiff[i] <= 1.0f);
            forceRay[i] = fetchBool(texForceRay, gli::extent2d(x, y), i, 0, 0).r > 0.5f;
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
            }
        }

        if (isDubious && !useDubiousSamples) continue; // less noise in training data

        for(size_t direction = 0; direction < NUM_DIRECTIONS; ++direction)
        {
            for (size_t step = 0; step < NUM_STEPS; ++step)
            {
                // check if forced
                if (forceRay[step])
                {
                    if (!IsTraining)
                    {
                        // save forced pixel
                        forcedPixels.push_back(x);
                        forcedPixels.push_back(y);
                        forcedPixels.push_back(int(direction * NUM_STEPS + step));
                    }
                    continue;
                }

                // check if trivial
                if (useSphereStart && raster[step] <= sphereStart[step]) continue; // raster is in sphere
                if (!useSphereStart && raster[step] <= 1.0f) continue; // raster is in trusted area

                // use this sample
                rasterSamples[step].insert(rasterSamples[step].end(), raster.begin(), raster.end());
                raySamples[step].insert(raySamples[step].end(), ray.begin(), ray.end());
                pixelXYi[step].push_back(x);
                pixelXYi[step].push_back(y);
                pixelXYi[step].push_back(int(direction * NUM_STEPS + step));
                if(ray[step] < raster[step]) // require ray tracing
                {
                    required[step].push_back(1);
                    weight[step].push_back(aoDiff[step]);
                }
                else // ray tracing not required 
                {
                    required[step].push_back(0);
                    weight[step].push_back(1.0f);
                }
            }

            // rotate samples for the next direction
            std::rotate(raster.begin(), raster.begin() + NUM_STEPS, raster.end());
            std::rotate(ray.begin(), ray.begin() + NUM_STEPS, ray.end());
            std::rotate(sphereStart.begin(), sphereStart.begin() + NUM_STEPS, sphereStart.end());
            std::rotate(aoDiff.begin(), aoDiff.begin() + NUM_STEPS, aoDiff.end());
            std::rotate(forceRay.begin(), forceRay.begin() + NUM_STEPS, forceRay.end());
        }

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

    const auto strIndex = std::to_string(index);

    // print out number of all samples, empty samples and skipped samples
    std::cout << "Dubious samples (ray > raster): " << dubiousSamples << std::endl;
    for(size_t i = 0; i < NUM_STEPS; ++i)
    {
        unsigned long remainingSamples = (unsigned long)required[i].size();

        std::cout << "Remaining samples [" << i << "] " << remainingSamples << std::endl;

        // write to numpy files
        unsigned long shapeSamples[] = { remainingSamples, (unsigned long)nSamples }; // shape = rows, columns
        unsigned long shapeRequired[] = { remainingSamples, 1ul };
        unsigned long shapeXY[] = { remainingSamples, 3ul };

        const auto strStep = std::to_string(i);
        if(IsTraining)
        {
            npy::SaveArrayAsNumpy("raster_train" + strStep + "_" + strIndex + ".npy", false, 2, shapeSamples, rasterSamples[i]);
            npy::SaveArrayAsNumpy("ray_train" + strStep + "_" + strIndex + ".npy", false, 2, shapeSamples, raySamples[i]);
            npy::SaveArrayAsNumpy("required_train" + strStep + "_" + strIndex + ".npy", false, 2, shapeRequired, required[i]);
            npy::SaveArrayAsNumpy("weight_train" + strStep + "_" + strIndex + ".npy", false, 2, shapeRequired, weight[i]);
        }
        else
        {
            npy::SaveArrayAsNumpy("raster_eval" + strStep + "_" + strIndex + ".npy", false, 2, shapeSamples, rasterSamples[i]);
            npy::SaveArrayAsNumpy("ray_eval" + strStep + "_" + strIndex + ".npy", false, 2, shapeSamples, raySamples[i]);
            npy::SaveArrayAsNumpy("required_eval" + strStep + "_" + strIndex + ".npy", false, 2, shapeRequired, required[i]);
            npy::SaveArrayAsNumpy("pixelXY" + strStep + "_" + strIndex + ".npy", false, 2, shapeXY, pixelXYi[i]);
            npy::SaveArrayAsNumpy("weight_eval" + strStep + "_" + strIndex + ".npy", false, 2, shapeRequired, weight[i]);
        }
    }

    if(!IsTraining)
    {
        unsigned long shapeXYForced[] = { (unsigned long)(forcedPixels.size() / 3), 3ul };
        npy::SaveArrayAsNumpy("forcedXY_eval_" + strIndex + ".npy", false, 2, shapeXYForced, forcedPixels);
    }
}
