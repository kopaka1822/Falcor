#pragma once
#include "gli/gli/gli.hpp"
#include "npy.h"
#include <iostream>
#include <numeric>

void convert_to_numpyv2(std::string raster_image, std::string ray_image, std::string force_ray, std::string sphere_start, std::string raster_ao, std::string ray_ao, int index = 0)
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
    std::vector<float> rasterSamples;
    std::vector<float> raySamples;
    //std::vector<float> sphereStartSamples;
    //std::vector<int> sameInstance; // 1 if raster sample instance is same as pixel center
    //std::vector<int> rasterInstanceDiffs; // 0 if both neighbors are same instance, 1 if only one is same instance, 2 if both are different instances
    std::vector<int> pixelXYi; // x,y coordinates of pixel
    std::vector<uint8_t> required; // 1 if ray tracing is required, 0 if not (x4)
    std::vector<uint8_t> requiredForced;
    std::vector<uint8_t> asked; // 1 if we want to ask the neural net for a prediction

    //std::vector<int> forcedPixels; // XYi coordinates of the pixel that was forced to be ray traced (or is invalid, in which case the saved ao is 1.0)
    //std::vector<int> numInvalid; // number of invalid samples

    rasterSamples.reserve(width * height * nSamples);
    raySamples.reserve(width * height * nSamples);
    //sameInstance.reserve(width * height * nSamples);
    //rasterInstanceDiffs.reserve(width * height * nSamples);
    pixelXYi.reserve(width * height * 3);
    required.reserve(width * height * NUM_STEPS);
    asked.reserve(width * height * NUM_STEPS);
    
    //forcedPixels.reserve(width * height * 3 * NUM_SAMPLES);

    // local arrays
    std::array<float, NUM_SAMPLES> raster;
    std::array<float, NUM_SAMPLES> ray;
    std::array<float, NUM_SAMPLES> sphereStart;
    std::array<float, NUM_SAMPLES> aoDiff; // difference in AO values
    std::array<uint8_t, NUM_SAMPLES> forceRay;
    //std::vector<uint32_t> instances;
    //instances.resize(nSamples);

    // fetch function for tex raster (and tex ray)
    auto fetch = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRaster.format()).Fetch;
    auto fetchAO = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRasterAO.format()).Fetch;
    //auto fetchInt = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texInstances.format()).Fetch;
    auto fetchBool = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texForceRay.format()).Fetch;
    int dubiousSamples = 0;
    float equalityThreshold = 0.01; // assume ray and raster are equal when the values are within this threshold (reduce noise in training data)

    size_t numInvalid = 0; // invalid sample (below the hemisphere)
    size_t numOutOfScreen = 0; // ray tracing was forced
    size_t numDoubleSided = 0;
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
            auto forceRayId = int8_t(fetchBool(texForceRay, gli::extent2d(x, y), i, 0, 0).r);
            forceRay[i] = forceRayId != 0;
            if (forceRayId == 3) numInvalid++;
            else if (forceRayId == 2) numDoubleSided++;
            else if (forceRayId == 1) numOutOfScreen++;
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

        for (size_t direction = 0; direction < NUM_DIRECTIONS; ++direction)
        {
            if(forceRay[0] && forceRay[1] && forceRay[2] && forceRay[3] && IsTraining)
            {
                goto prepare_next_iteration; // skip this for the training
            }

            // fill require mask
            std::array<uint8_t, NUM_STEPS>  requireMask;
            std::array<uint8_t, NUM_STEPS> askMask;
            for (size_t step = 0; step < NUM_STEPS; ++step)
            {
                uint8_t askRay = 1 - forceRay[step]; // ask for ray if we don't force it

                // check if trivial
                if (useSphereStart && raster[step] <= sphereStart[step]) askRay = 0; // raster is in sphere
                if (!useSphereStart && raster[step] <= 1.0f) askRay = 0; // raster is in trusted area

                askMask[step] = askRay;
                
                requireMask[step] = (ray[step] < raster[step]) ? 1 : 0;
                requireMask[step] = requireMask[step] & askRay; // only require if we ask for it
            }

            if (askMask[0] == 0 && askMask[1] == 0 && askMask[2] == 0 && askMask[3] == 0 && IsTraining)
            {
                goto prepare_next_iteration; // nothing needs to be evaluated by the neural net
            }

            // use this sample
            rasterSamples.insert(rasterSamples.end(), raster.begin(), raster.end());
            raySamples.insert(raySamples.end(), ray.begin(), ray.end());
            
            pixelXYi.push_back(x);
            pixelXYi.push_back(y);
            pixelXYi.push_back(int(direction * 4));
            asked.insert(asked.end(), askMask.begin(), askMask.end());
            required.insert(required.end(), requireMask.begin(), requireMask.end());
            requiredForced.insert(requiredForced.end(), forceRay.begin(), forceRay.begin() + NUM_STEPS);

            prepare_next_iteration:
            // rotate samples for the next direction
            std::rotate(raster.begin(), raster.begin() + NUM_STEPS, raster.end());
            std::rotate(ray.begin(), ray.begin() + NUM_STEPS, ray.end());
            std::rotate(sphereStart.begin(), sphereStart.begin() + NUM_STEPS, sphereStart.end());
            std::rotate(aoDiff.begin(), aoDiff.begin() + NUM_STEPS, aoDiff.end());
            std::rotate(forceRay.begin(), forceRay.begin() + NUM_STEPS, forceRay.end());
        }
    }

    const auto strIndex = std::to_string(index);

    // print out number of all samples, empty samples and skipped samples
    std::cout << "Dubious samples (ray > raster): " << dubiousSamples << std::endl;
    unsigned long remainingSamples = (unsigned long)(required.size() / NUM_STEPS);

    std::cout << "Remaining samples: " << remainingSamples << std::endl;
    
    auto numAsked = std::accumulate(asked.begin(), asked.end(), size_t(0));
    auto numRequired = std::accumulate(required.begin(), required.end(), size_t(0));
    std::cout << "Num Asked: " << numAsked << std::endl;
    std::cout << "Num Required: " << numRequired << std::endl;
    std::cout << "Num Double Sided (forced): " << numDoubleSided << std::endl;
    std::cout << "Num Invalid (below hemisphere): " << numInvalid << std::endl;
    std::cout << "Num Excluded because of screen border: " << numOutOfScreen << std::endl;
    

    // write to numpy files
    unsigned long shapeSamples[] = { remainingSamples, (unsigned long)nSamples }; // shape = rows, columns
    unsigned long shapeRequired[] = { remainingSamples, (unsigned long)NUM_STEPS };
    unsigned long shapeXY[] = { remainingSamples, 3ul };

    if (IsTraining)
    {
        npy::SaveArrayAsNumpy("raster_train_" + strIndex + ".npy", false, 2, shapeSamples, rasterSamples);
        npy::SaveArrayAsNumpy("ray_train_" + strIndex + ".npy", false, 2, shapeSamples, raySamples);
        npy::SaveArrayAsNumpy("required_train_" + strIndex + ".npy", false, 2, shapeRequired, required);
        npy::SaveArrayAsNumpy("asked_train_" + strIndex + ".npy", false, 2, shapeRequired, asked);
        npy::SaveArrayAsNumpy("required_forced_train_" + strIndex + ".npy", false, 2, shapeRequired, requiredForced);
    }
    else
    {
        npy::SaveArrayAsNumpy("raster_eval_" + strIndex + ".npy", false, 2, shapeSamples, rasterSamples);
        npy::SaveArrayAsNumpy("ray_eval_" + strIndex + ".npy", false, 2, shapeSamples, raySamples);
        npy::SaveArrayAsNumpy("required_eval_" + strIndex + ".npy", false, 2, shapeRequired, required);
        npy::SaveArrayAsNumpy("asked_eval_" + strIndex + ".npy", false, 2, shapeRequired, asked);
        npy::SaveArrayAsNumpy("required_forced_eval_" + strIndex + ".npy", false, 2, shapeRequired, requiredForced);
        npy::SaveArrayAsNumpy("pixelXY_" + strIndex + ".npy", false, 2, shapeXY, pixelXYi);
    }
}
