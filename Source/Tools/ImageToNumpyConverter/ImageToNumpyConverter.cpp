#include "gli/gli/gli.hpp"
#include "npy.h"
#include <iostream>

// skip samples where all values are <= 1.0 (no rays need to be traced for this)
static const bool SkipTrivialSamples = true;

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cout << "Usage: ImageToNumpyConverter.exe <raster image> <ray image>" << std::endl;
        return 1;
    }

    gli::texture2d_array texRaster(gli::load(argv[1]));
    gli::texture2d_array texRay(gli::load(argv[2]));

    assert(texRaster.format() == gli::FORMAT_R32_SFLOAT_PACK32);
    assert(texRaster.layers() == 8);
    assert(texRaster.layers() == texRay.layers());
    assert(texRaster.extent() == texRay.extent());

    auto width = texRaster.extent().x;
    auto height = texRaster.extent().y;
    auto nSamples = texRaster.layers();

    // prepare data in float vector
    std::vector<float> rasterSamples;
    std::vector<float> raySamples;
    std::vector<int> sameAsRay; // 1 if raster is same as ray, 0 otherwise
    rasterSamples.reserve(width * height * nSamples);
    raySamples.reserve(width * height * nSamples);
    sameAsRay.reserve(width * height);

    std::vector<float> raster;
    raster.resize(nSamples);
    std::vector<float> ray;
    ray.resize(nSamples);

    // fetch function for tex raster (and tex ray)
    auto fetch = gli::detail::convert<gli::texture2d_array, float, gli::defaultp>::call(texRaster.format()).Fetch;
    int nTrivial = 0;
    int nEmpty = 0;
    int nSame = 0;
    int nDifferent = 0;

    for(int y = 0; y < height; ++y) for(int x = 0; x < width; ++x)
    {
        // obtain sample data
        for(size_t i = 0; i < nSamples; ++i)
        {
            raster[i] = fetch(texRaster, gli::extent2d(x, y), i, 0, 0).r;
            ray[i] = fetch(texRay, gli::extent2d(x, y), i, 0, 0).r;
        }

        // skip pure zero entries (background)
        if (std::all_of(raster.begin(), raster.end(), [](float f) { return f == 0.0f; }))
        {
            nEmpty++;
            continue;
        }

        // check if trivial sample
        if (std::all_of(raster.begin(), raster.end(), [](float f) { return f <= 1.0f; }))
        {
            nTrivial++;
            if (SkipTrivialSamples) continue;
        }

        // write to raster data and ray data
        rasterSamples.insert(rasterSamples.end(), raster.begin(), raster.end());
        raySamples.insert(raySamples.end(), ray.begin(), ray.end());

        // determine if ray is equal to raster
        //auto same = std::equal(raster.begin(), raster.end(), ray.begin());
        // set same to true if all values are almost equal (within 0.01f)
        auto same = std::equal(raster.begin(), raster.end(), ray.begin(), [](float f1, float f2) { return std::abs(f1 - f2) < 0.01f; });
        sameAsRay.push_back(same ? 1 : 0);
        nSame += same ? 1 : 0;
        nDifferent += same ? 0 : 1;
    }

    // print out number of all samples, empty samples and skipped samples
    std::cout << "Total samples: " << width * height << std::endl;
    std::cout << "Empty samples: " << nEmpty << std::endl;
    std::cout << "Trivial samples: " << nTrivial << std::endl;
    std::cout << "Ray==Raster samples: " << nSame << std::endl;
    std::cout << "Ray!=Raster samples: " << nDifferent << std::endl;
    std::cout << "Remaining samples: " << sameAsRay.size() << std::endl;

    // write to numpy files
    const unsigned long shape[] = {  (unsigned long)sameAsRay.size(), (unsigned long)nSamples }; // shape = rows, columns
    npy::SaveArrayAsNumpy("raster.npy", false, 2, shape, rasterSamples);
    npy::SaveArrayAsNumpy("ray.npy", false, 2, shape, raySamples);

    npy::SaveArrayAsNumpy("same.npy", false, 1, shape, sameAsRay);

    return 0;
}
