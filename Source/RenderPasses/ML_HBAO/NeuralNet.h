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
    using Texture = Falcor::Texture;
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

        createTextures();
    }

    int layers() const { return mLayers; }
    int nets() const { return mNumNets; }



    void writeDefinesToFile(const char* filename) const
    {
        std::ofstream file(filename);
        file << getShaderDefine();
        file.close();
    }

    void bindData(Falcor::ProgramVars* pVars) const
    {
        assert(pVars);
        for (size_t l = 0; l < mLayers; ++l)
        {
            pVars->setTexture("kernel" + std::to_string(l), mTextures[2 * l]);
            
            pVars->setTexture("bias" + std::to_string(l), mTextures[2 * l + 1]);
        }
    }

private:
    // shader define for NEURAL_NET_DATA
    std::string getShaderDefine() const
    {
        std::stringstream ss;
        for (size_t l = 0; l < mLayers; ++l)
        {
            auto kernel = mNets[0].kernels[l];
            ss << "Texture1DArray<float> kernel" << l << ";\n";
            ss << "#define KERNEL" << l << "(step, row, col) kernel" << l << "[uint2(row * " << kernel.columns << " + col,step)]\n";

            auto bias = mNets[0].biases[l];
            ss << "Texture1DArray<float> bias" << l << ";\n";
            ss << "#define BIAS" << l << "(step, col) bias" << l << "[uint2(col,step)]\n";
        }

        return ss.str();
    }
    
    void createTextures()
    {
        mTextures.resize(mLayers * 2); // for each layer: kernel and bias
        for (size_t l = 0; l < mLayers; ++l)
        {
            auto kernel = mNets[0].kernels[l];

            std::vector<float> kernelData;
            kernelData.resize(kernel.rows * kernel.columns * mNumNets);
            auto kernelBegin = kernelData.begin();
            for(auto& net : mNets)
            {
                kernelBegin = std::copy(net.kernels[l].data.begin(), net.kernels[l].data.end(), kernelBegin);
            }
            
            // kernel
            mTextures[2 * l] = Texture::create1D(kernel.rows * kernel.columns, Falcor::ResourceFormat::R32Float,
                mNumNets, 1, kernelData.data(), Falcor::Resource::BindFlags::ShaderResource);
            
            auto bias = mNets[0].biases[l];
            std::vector<float> biasData;
            biasData.resize(bias.rows * bias.columns * mNumNets);
            auto biasBegin = biasData.begin();
            for (auto& net : mNets)
            {
                biasBegin = std::copy(net.biases[l].data.begin(), net.biases[l].data.end(), biasBegin);
            }

            // bias
            mTextures[2 * l + 1] = Texture::create1D(bias.rows * bias.columns, Falcor::ResourceFormat::R32Float,
                mNumNets, 1, biasData.data(), Falcor::Resource::BindFlags::ShaderResource);
        }
    }

private:
    std::vector<NeuralNet> mNets;
    std::vector<Texture::SharedPtr> mTextures;
    int mNumNets = 0;
    int mLayers = 0;
};
