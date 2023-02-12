#pragma once
#include "Falcor.h"
#include <sstream>
#include "../Tools/ImageToNumpyConverter/npy.h"

// helper class that loads a neural net from a file
struct NeuralNet
{
    struct Matrix
    {
        int rows;
        int columns;
        std::vector<float> data;
    };

    void load(const std::string& baseFilename, int layers, int index)
    {
        kernels.resize(layers);
        biases.resize(layers);

        std::vector<unsigned long> shape;
        for(int l = 0; l < layers; ++l)
        {
            std::stringstream ss;
            // load weights
            ss << baseFilename << "_weights" << index << "_kernel" << l << ".npy";
            npy::LoadArrayFromNumpy<float>(ss.str(), shape, kernels[l].data);
            kernels[l].rows = shape[0];
            kernels[l].columns = shape[1];
            // load biases
            ss = std::stringstream(); // clear 
            ss << baseFilename << "_weights" << index << "_bias" << l << ".npy";
            npy::LoadArrayFromNumpy<float>(ss.str(), shape, biases[l].data);
            biases[l].rows = 1;
            biases[l].columns = shape[0];
        }
    }
    
    std::vector<Matrix> kernels;
    std::vector<Matrix> biases;
};

class NeuralNetCollection
{
public:
    NeuralNetCollection(int numNets, int numLayers)
        :
    mNumNets(numNets),
    mLayers(numLayers)
    {}

    void load(const std::string& baseFilename)
    {
        mNets.resize(mNumNets);
        for (int i = 0; i < mNumNets; ++i)
        {
            mNets[i].load(baseFilename, mLayers, i);
        }
    }

    int layers() const { return mLayers; }
    int nets() const { return mNumNets; }

    

private:
    std::vector<NeuralNet> mNets;
    int mNumNets = 0;
    int mLayers = 0;
};
