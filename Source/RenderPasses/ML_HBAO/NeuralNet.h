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
    enum class Binding
    {
        TextureFloat,
        TextureHalf,
        StaticFloat,
    };

    NeuralNetCollection(int numNets, int numLayers)
        :
    mNumNets(numNets),
    mLayers(numLayers),
    mGuiLayers(numLayers)
    {}

    void load(const std::string& baseFilename)
    {
        mFilename = baseFilename;
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
            switch (mBinding)
            {
            case Binding::TextureFloat:
                pVars->setTexture("kernel" + std::to_string(l), mTextures[2 * l]);
                pVars->setTexture("bias" + std::to_string(l), mTextures[2 * l + 1]);
                break;
            case Binding::TextureHalf:
                pVars->setTexture("kernel" + std::to_string(l), mTextureHalfs[2 * l]);
                pVars->setTexture("bias" + std::to_string(l), mTextureHalfs[2 * l + 1]);
                break;
            }

        }
    }

    bool renderUI(Falcor::Gui::Widgets& widget)
    {
        widget.separator();

        bool changed = false;
        const Falcor::Gui::DropdownList kBindingValues =
        {
            { (uint32_t)Binding::TextureFloat, "Texture Float" },
            { (uint32_t)Binding::TextureHalf, "Texture Half" },
            { (uint32_t)Binding::StaticFloat, "Static Float" },
        };

        uint32_t binding = (uint32_t)mBinding;
        changed = widget.dropdown("Neural Net Binding", kBindingValues, binding) || changed;
        mBinding = (Binding)binding;

        widget.var("Layers", mGuiLayers, 1, 8, 1);
        widget.textbox("File: ", mFilename);
        if(widget.button("Load From File"))
        {
            try
            {
                NeuralNetCollection tmp(mNumNets, mGuiLayers);
                tmp.load(mFilename);
                *this = tmp;
            }
            catch(const std::exception& e)
            {
                Falcor::msgBox("Error loading neural net: " + std::string(e.what()));
            }
            changed = true;
        }

        widget.separator();

        return changed;
    }

private:
    // shader define for NEURAL_NET_DATA
    std::string getShaderDefine() const
    {
        std::stringstream ss;
        ss << "#define NUM_LAYERS " << mLayers << '\n';

        for (size_t l = 0; l < mLayers; ++l)
        {
            auto kernel = mNets[0].kernels[l];
            auto bias = mNets[0].biases[l];
            
            switch (mBinding)
            {
            case Binding::TextureFloat:
                ss << "Texture1D<float> kernel" << l << ";\n";
                ss << "Texture1D<float> bias" << l << ";\n";
                break;
            case Binding::TextureHalf:
                ss << "Texture1D<float> kernel" << l << ";\n";
                ss << "Texture1D<float> bias" << l << ";\n";
                break;
            case Binding::StaticFloat:
                ss << mDataStrings[2 * l];
                ss << mDataStrings[2 * l + 1];
                break;
            }

            if(mNumNets == 1) // combined multi-label classifier
            {
                ss << "#define KERNEL" << l << "(row, col) kernel" << l << "[row * " << kernel.columns << " + col]\n";

                ss << "#define BIAS" << l << "(col) bias" << l << "[col]\n";
            }
            else // 1 classifier for each step
            {
                ss << "#define KERNEL" << l << "(step, row, col) kernel" << l << "[step * " << (kernel.rows * kernel.columns) << " + row * " << kernel.columns << " + col]\n";

                ss << "#define BIAS" << l << "(step, col) bias" << l << "[step * " << (bias.columns) << " + col]\n";
            }
            

            ss << "#define KERNEL" << l << "_ROWS " << kernel.rows << "\n";
            ss << "#define KERNEL" << l << "_COLUMNS " << kernel.columns << "\n";
        }


        if(mNumNets == 1)
        {
            const auto& net = mNets[0];
            // generate function
            ss << "\nuint evalNeuralNet" << "(float inputs[" << net.kernels[0].rows << "]){\n";
            std::string prevInput = "inputs";
            for (size_t l = 0; l < mLayers; ++l)
            {
                auto kernel = net.kernels[l];
                auto bias = net.biases[l];

                // load bias
                ss << "\tfloat layer" << l << "Output[" << bias.columns << "];\n";
                ss << "\t[unroll] for(uint outIdx = 0; outIdx < " << bias.columns << "; ++outIdx)\n";
                ss << "\t\tlayer" << l << "Output[outIdx] = BIAS" << l << "(outIdx);\n\n";

                // multiply with kernel
                ss << "\t[unroll] for(uint inIdx = 0; inIdx < " << kernel.rows << "; ++inIdx)\n";
                ss << "\t\t[unroll] for(uint outIdx = 0; outIdx < " << kernel.columns << "; ++outIdx)\n";
                ss << "\t\t\tlayer" << l << "Output[outIdx] += KERNEL" << l << "(inIdx, outIdx) * " << prevInput << "[inIdx];\n\n";

                // apply activation function
                if(l == mLayers - 1ull)
                {
                    // last layer => sigmoid mask
                    ss << "\tuint bitmask = 0;\n";
                    ss << "\t[unroll] for(uint outIdx = 0; outIdx < " << bias.columns << "; ++outIdx)\n";
                    ss << "\t\tif(layer" << l << "Output[outIdx] > 0.0)\n";
                    ss << "\t\t\tbitmask = bitmask | (1u << outIdx);\n\n";
                }
                else
                {
                    // relu activation
                    ss << "\t[unroll] for(uint outIdx = 0; outIdx < " << bias.columns << "; ++outIdx)\n";
                    ss << "\t\tlayer" << l << "Output[outIdx] = max(layer" << l << "Output[outIdx], 0); // RELU\n\n";

                    prevInput = "layer" + std::to_string(l) + "Output";
                }

            }

            ss << "\treturn bitmask;\n}\n";
        }
        

        return ss.str();
    }
    
    void createTextures()
    {
        mTextures.resize(mLayers * 2); // for each layer: kernel and bias
        mDataStrings.resize(mLayers * 2);
        mTextureHalfs.resize(mLayers * 2);

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
            mTextures[2 * l] = Texture::create1D(kernel.rows * kernel.columns * mNumNets, Falcor::ResourceFormat::R32Float,
                1, 1, kernelData.data(), Falcor::Resource::BindFlags::ShaderResource);
            auto kernelHalf = convertToHalfs(kernelData);
            mTextureHalfs[2 * l] = Texture::create1D(kernel.rows * kernel.columns * mNumNets, Falcor::ResourceFormat::R16Float,
                1, 1, kernelHalf.data(), Falcor::Resource::BindFlags::ShaderResource);
            mDataStrings[2 * l] = getDataString("kernel" + std::to_string(l), kernelData);

            auto bias = mNets[0].biases[l];
            std::vector<float> biasData;
            biasData.resize(bias.rows * bias.columns * mNumNets);
            auto biasBegin = biasData.begin();
            for (auto& net : mNets)
            {
                biasBegin = std::copy(net.biases[l].data.begin(), net.biases[l].data.end(), biasBegin);
            }

            // bias
            mTextures[2 * l + 1] = Texture::create1D(bias.rows * bias.columns * mNumNets, Falcor::ResourceFormat::R32Float,
                1, 1, biasData.data(), Falcor::Resource::BindFlags::ShaderResource);
            auto biasHalf = convertToHalfs(biasData);
            mTextureHalfs[2 * l + 1] = Texture::create1D(bias.rows * bias.columns * mNumNets, Falcor::ResourceFormat::R16Float,
                1, 1, biasHalf.data(), Falcor::Resource::BindFlags::ShaderResource);
            mDataStrings[2 * l + 1] = getDataString("bias" + std::to_string(l), biasData);
        }
    }

    std::string getDataString(std::string name, const std::vector<float>& data) const
    {
        std::stringstream ss;
        ss << "static const float " << name << "[" << data.size() << "] = {";
        for (size_t i = 0; i < data.size(); ++i)
        {
            ss << data[i];
            if (i < data.size() - 1)
            {
                ss << ", ";
            }
        }
        ss << "};\n";
        return ss.str();
    }

    std::vector<int16_t> convertToHalfs(const std::vector<float>& data) const
    {
        std::vector<int16_t> halfs;
        halfs.resize(data.size());
        for (size_t i = 0; i < data.size(); ++i)
        {
            halfs[i] = glm::detail::toFloat16(data[i]);
        }
        return halfs;
    }
private:
    Binding mBinding = Binding::StaticFloat;
    std::vector<NeuralNet> mNets;
    std::vector<Texture::SharedPtr> mTextures;
    std::vector<Texture::SharedPtr> mTextureHalfs;
    std::vector<std::string> mDataStrings;
    int mNumNets = 0;
    int mLayers = 0;
    std::string mFilename;
    int mGuiLayers = 0;
};
