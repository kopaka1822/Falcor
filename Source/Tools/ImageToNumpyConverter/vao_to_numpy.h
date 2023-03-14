#pragma once
#include "gli/gli/gli.hpp"
#include "npy.h"
#include <iostream>
#include <numeric>
#include "force_ray.h"

void vao_to_numpy(const std::vector<float> sphereStart, std::string raster_image, std::string ray_image, std::string force_ray, std::string raster_ao, std::string ray_ao, std::string sphere_end, int index, bool IsTraining)
{
    static constexpr size_t NUM_SAMPLES = 8;
    
    const bool useDubiousSamples = !IsTraining; // false for training, true for evaluation
    const bool forceDoubleSided = true; // can probably improve the classify accuracy

    gli::texture2d_array texRaster(gli::load(raster_image));
    gli::texture2d_array texRay(gli::load(ray_image));
    //gli::texture2d_array texInstances(gli::load(argv[3]));
    //gli::texture2d_array texCurInstance(gli::load(argv[4]));
    gli::texture2d_array texForceRay(gli::load(force_ray));

    gli::texture2d_array texRasterAO(gli::load(raster_ao));
    gli::texture2d_array texRayAO(gli::load(ray_ao));
    gli::texture2d_array texSphereEnd(gli::load(sphere_end));

    auto width = texRaster.extent().x;
    auto height = texRaster.extent().y;
    assert(texRaster.layers() == NUM_SAMPLES);

    // prepare data in float vector
    std::vector<float> rasterSamples;
    std::vector<float> raySamples;
    std::vector<int> pixelXY; // x,y coordinates of pixel
    std::vector<uint8_t> required; // 1 if ray tracing is required, 0 if not (x8)
    std::vector<uint8_t> requiredForced;
    std::vector<uint8_t> asked; // 1 if we want to ask the neural net for a prediction
    std::vector<float> sphereEndSamples;
    std::vector<float> sphereStartSamples;

    //std::vector<int> forcedPixels; // XYi coordinates of the pixel that was forced to be ray traced (or is invalid, in which case the saved ao is 1.0)
    //std::vector<int> numInvalid; // number of invalid samples

    rasterSamples.reserve(width * height * NUM_SAMPLES);
    raySamples.reserve(width * height * NUM_SAMPLES);
    //sameInstance.reserve(width * height * nSamples);
    //rasterInstanceDiffs.reserve(width * height * nSamples);
    pixelXY.reserve(width * height * 2);
    required.reserve(width * height * NUM_SAMPLES);
    asked.reserve(width * height * NUM_SAMPLES);
    sphereEndSamples.reserve(width * height * NUM_SAMPLES);
    sphereStartSamples.reserve(width * height * NUM_SAMPLES);

    //forcedPixels.reserve(width * height * 3 * NUM_SAMPLES);

    // local arrays
    std::array<float, NUM_SAMPLES> raster;
    std::array<float, NUM_SAMPLES> ray;
    std::array<float, NUM_SAMPLES> aoDiff; // difference in AO values
    std::array<uint8_t, NUM_SAMPLES> forceRay;
    std::array<float, NUM_SAMPLES> sphereEnd;
    //std::vector<uint32_t> instances;
    //instances.resize(nSamples);

    // fetch function for tex raster (and tex ray)
    auto fetch = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRaster.format()).Fetch;
    auto fetchAO = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRasterAO.format()).Fetch;
    //auto fetchInt = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texInstances.format()).Fetch;
    auto fetchBool = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texForceRay.format()).Fetch;
    int dubiousSamples = 0;
    float equalityThreshold = 0.01f; // assume ray and raster are equal when the values are within this threshold (reduce noise in training data)

    size_t numInvalid = 0; // invalid sample (below the hemisphere)
    size_t numOutOfScreen = 0; // ray tracing was forced
    size_t numDoubleSided = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
    {
        bool outOfScreen = false;
        for (size_t i = 0; i < NUM_SAMPLES; ++i)
        {
            raster[i] = fetch(texRaster, gli::extent2d(x, y), i, 0, 0).r;
            ray[i] = fetch(texRay, gli::extent2d(x, y), i, 0, 0).r;
            sphereEnd[i] = fetch(texSphereEnd, gli::extent2d(x, y), i, 0, 0).r;
            //instances[i] = (uint32_t)fetchInt(texInstances, gli::extent2d(x, y), i, 0, 0).r;
            aoDiff[i] = abs(fetchAO(texRasterAO, gli::extent2d(x, y), i, 0, 0).r - fetchAO(texRayAO, gli::extent2d(x, y), i, 0, 0).r);
            assert(aoDiff[i] <= 1.0f);
            auto forceRayId = int8_t(fetchBool(texForceRay, gli::extent2d(x, y), i, 0, 0).r);
            forceRay[i] = 0;
            if (forceRayId == FORCE_RAY_INVALID)
            {
                numInvalid++;
                forceRay[i] = 1;
            }
            else if (forceRayId == FORCE_RAY_DOUBLE_SIDED && forceDoubleSided)
            {
                numDoubleSided++;
                forceRay[i] = 1;
            }
            else if (forceRayId == FORCE_RAY_OUT_OF_SCREEN)
            {
                numOutOfScreen++;
                outOfScreen = true;
                forceRay[i] = 1;
            }
        }

        if (IsTraining && outOfScreen) continue; // skip out of screen pixels in training data

        // reduce noise
        bool isDubious = false;
        for (size_t i = 0; i < NUM_SAMPLES; ++i)
        {
            // set ray = raster if they are close enough
            if (std::abs(raster[i] - ray[i]) <= equalityThreshold)
            {
                ray[i] = raster[i];
            }
            // clamping for farplane:
            if(raster[i] < -100.0f)
            {
                ray[i] = raster[i]; // clamp ray to raster for very small values (hitting background)
            }

            if (raster[i] < ray[i])
            {
                dubiousSamples++;
                //std::cout << "doubious sample at: " << x << ", " << y << " id: " << i << " diff: " << ray[i] - raster[i] << '\n';
                isDubious = true;
                ray[i] = raster[i]; // set ray to raster
            }
        }

        if (isDubious && !useDubiousSamples) continue; // less noise in training data

        // fill require mask
        std::array<uint8_t, NUM_SAMPLES>  requireMask;
        std::array<uint8_t, NUM_SAMPLES> askMask;
        for (size_t step = 0; step < NUM_SAMPLES; ++step)
        {
            uint8_t askRay = 1 - forceRay[step]; // ask for ray if we don't force it

            // check if trivial
            //if (raster[step] <= sphereStart[step]) askRay = 0; // raster is in sphere
            if (raster[step] <= 1.0) askRay = 0; // raster is in const area or sphere

            askMask[step] = askRay;

            requireMask[step] = (ray[step] < raster[step]) ? 1 : 0;
            requireMask[step] = requireMask[step] & askRay; // only require if we ask for it
        }

        if (std::all_of(askMask.begin(), askMask.end(), [](uint8_t ask) {return ask == 0; }) && IsTraining)
            continue; // nothing needs to be evaluated by the neural net

        // use this sample
        rasterSamples.insert(rasterSamples.end(), raster.begin(), raster.end());
        raySamples.insert(raySamples.end(), ray.begin(), ray.end());

        pixelXY.push_back(x);
        pixelXY.push_back(y);
        asked.insert(asked.end(), askMask.begin(), askMask.end());
        required.insert(required.end(), requireMask.begin(), requireMask.end());
        requiredForced.insert(requiredForced.end(), forceRay.begin(), forceRay.end());
        sphereEndSamples.insert(sphereEndSamples.end(), sphereEnd.begin(), sphereEnd.end());
        sphereStartSamples.insert(sphereStartSamples.end(), sphereStart.begin(), sphereStart.end());
    }

    const auto strIndex = std::to_string(index);

    // print out number of all samples, empty samples and skipped samples
    std::cout << "Dubious samples (ray > raster): " << dubiousSamples << std::endl;
    unsigned long remainingSamples = (unsigned long)(required.size() / NUM_SAMPLES);

    std::cout << "Remaining samples: " << remainingSamples << std::endl;

    auto numAsked = std::accumulate(asked.begin(), asked.end(), size_t(0));
    auto numRequired = std::accumulate(required.begin(), required.end(), size_t(0));
    std::cout << "Num Asked: " << numAsked << std::endl;
    std::cout << "Num Required: " << numRequired << std::endl;
    std::cout << "Num Double Sided (forced): " << numDoubleSided << std::endl;
    std::cout << "Num Invalid (below hemisphere): " << numInvalid << std::endl;
    std::cout << "Num Excluded because of screen border: " << numOutOfScreen / NUM_SAMPLES << std::endl;


    // write to numpy files
    unsigned long shapeSamples[] = { remainingSamples, (unsigned long)NUM_SAMPLES }; // shape = rows, columns
    unsigned long shapeRequired[] = { remainingSamples, (unsigned long)NUM_SAMPLES };
    unsigned long shapeXY[] = { remainingSamples, 2ul };

    if (IsTraining)
    {
        npy::SaveArrayAsNumpy("raster_train_" + strIndex + ".npy", false, 2, shapeSamples, rasterSamples);
        npy::SaveArrayAsNumpy("ray_train_" + strIndex + ".npy", false, 2, shapeSamples, raySamples);
        npy::SaveArrayAsNumpy("sphere_start_train_" + strIndex + ".npy", false, 2, shapeSamples, sphereStartSamples);
        npy::SaveArrayAsNumpy("sphere_end_train_" + strIndex + ".npy", false, 2, shapeSamples, sphereEndSamples);
        
        npy::SaveArrayAsNumpy("required_train_" + strIndex + ".npy", false, 2, shapeRequired, required);
        npy::SaveArrayAsNumpy("asked_train_" + strIndex + ".npy", false, 2, shapeRequired, asked);
        //npy::SaveArrayAsNumpy("required_forced_train_" + strIndex + ".npy", false, 2, shapeRequired, requiredForced);
    }
    else
    {
        npy::SaveArrayAsNumpy("raster_eval_" + strIndex + ".npy", false, 2, shapeSamples, rasterSamples);
        npy::SaveArrayAsNumpy("ray_eval_" + strIndex + ".npy", false, 2, shapeSamples, raySamples);
        npy::SaveArrayAsNumpy("sphere_start_eval_" + strIndex + ".npy", false, 2, shapeSamples, sphereStartSamples);
        npy::SaveArrayAsNumpy("sphere_end_eval_" + strIndex + ".npy", false, 2, shapeSamples, sphereEndSamples);

        npy::SaveArrayAsNumpy("required_eval_" + strIndex + ".npy", false, 2, shapeRequired, required);
        npy::SaveArrayAsNumpy("asked_eval_" + strIndex + ".npy", false, 2, shapeRequired, asked);
        npy::SaveArrayAsNumpy("required_forced_eval_" + strIndex + ".npy", false, 2, shapeRequired, requiredForced);
        npy::SaveArrayAsNumpy("pixelXY_" + strIndex + ".npy", false, 2, shapeXY, pixelXY);
    }
}
