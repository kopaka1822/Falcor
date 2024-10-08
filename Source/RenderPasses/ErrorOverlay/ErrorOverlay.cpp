/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "ErrorOverlay.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ErrorOverlay>();
}

namespace
{
    //Shader
    const char kShaderFile[] = "RenderPasses/ErrorOverlay/ErrorOverlay.cs.slang";

    //Inputs
    const ChannelList kInputChannels = {
        {"Image", "gImage", "Image Input"},
        {"Reference", "gRef", "Reference input"},
        {"MSE", "gMSE", "MSE error"},
        {"FLIP", "gFLIP", "FLIP error"},
    };

    //Outputs
    const ChannelList kOutputChannels = {
        {"output", "gOut", "Output", false, ResourceFormat::RGBA32Float}
    };

    //Properties
    const char kImageMode0[] = "ImageMode0";
    const char kImageMode1[] = "ImageMode1";
    const char kImageMode2[] = "ImageMode2";
    const char kImageMode3[] = "ImageMode3";
    const char kLineThickness[] = "LineThickness";
    const char kLineColor[] = "LineColor";
    }

ErrorOverlay::ErrorOverlay(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    parseProperties(props);
}

void ErrorOverlay::parseProperties(const Properties& props) {
    for (const auto& [key, value] : props)
    {
        if (key == kImageMode0)
            mImageModes[0] = value;
        else if (key == kImageMode1)
            mImageModes[1] = value;
        else if (key == kImageMode2)
            mImageModes[2] = value;
        else if (key == kImageMode3)
            mImageModes[3] = value;
        else if (key == kLineThickness)
            mLineThickness = value;
        else if (key == kLineColor)
            mLineColor = value;
        else
            logWarning("Unknown property '{}' in ErrorOverlay properties.", key);
    }
}

Properties ErrorOverlay::getProperties() const
{
    Properties props;
    props[kImageMode0] = mImageModes[0];
    props[kImageMode1] = mImageModes[1];
    props[kImageMode2] = mImageModes[2];
    props[kImageMode3] = mImageModes[3];
    props[kLineThickness] = mLineThickness;
    props[kLineColor] = mLineColor;
    return props;
}

RenderPassReflection ErrorOverlay::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void ErrorOverlay::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    //Create Pass
    if (!mpErrorOverlayPass)
    {
        Program::Desc desc;
        desc.addShaderLibrary(kShaderFile).csEntry("main").setShaderModel("6_5");

        DefineList defines;

        mpErrorOverlayPass = ComputePass::create(mpDevice, desc, defines, true);
    }

     FALCOR_ASSERT(mpErrorOverlayPass);

     //Get number of valid images and sort
     uint validImages = 0;
     uint4 imageTypes = uint4(0);
     bool enableSetValidImages = true;
     if (mOnlyShowID >= 0 && mOnlyShowID < 4)
     {
         if (mImageModes[mOnlyShowID] != ErrorOverlayMode::None)
         {
             validImages = 1;
             imageTypes[0] = (uint)mImageModes[mOnlyShowID];
             enableSetValidImages = false;
         }
     }

     if (enableSetValidImages)
     {
        for (uint i = 0; i < mImageModes.size(); i++)
        {
            if (mImageModes[i] != ErrorOverlayMode::None)
            {
                imageTypes[validImages] = (uint)mImageModes[i];
                validImages++;
            }
        }
     }

     if (mCaptureCurrentFrame)
     {
        captureCurrentFrame(renderData, validImages, imageTypes);
        mCaptureCurrentFrame = false;
     }
        
        
     // Set variables
     auto var = mpErrorOverlayPass->getRootVar();

     //Set settings
     var["CB"]["gLinePosX"] = float2(mMousePos.x - mLineThickness, mMousePos.x + mLineThickness);
     var["CB"]["gLinePosY"] = float2(mMousePos.y - mLineThickness, mMousePos.y + mLineThickness);
     var["CB"]["gLineColor"] = mLineColor;
     var["CB"]["gValidImages"] = validImages;
     var["CB"]["gImageModes"] = imageTypes;

     //Bind Textures

     //Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
     auto bind = [&](const ChannelDesc& desc)
     {
         if (!desc.texname.empty())
         {
             var[desc.texname] = renderData.getTexture(desc.name);
         }
     };
     for (auto channel : kInputChannels)
        bind(channel);
     for (auto channel : kOutputChannels)
        bind(channel);

     // Execute
     const uint2 targetDim = renderData.getDefaultTextureDims();
     FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
     mpErrorOverlayPass->execute(pRenderContext, uint3(targetDim, 1));
}

void ErrorOverlay::renderUI(Gui::Widgets& widget)
{
     if (mOnlyShowID >= 0)
     {
        widget.text("Showing only " + std::to_string(mOnlyShowID));
     }
     widget.dropdown("Image0", mImageModes[0]);
     widget.dropdown("Image1", mImageModes[1]);
     widget.dropdown("Image2", mImageModes[2]);
     widget.dropdown("Image3", mImageModes[3]);

     widget.slider("Line Thickness", mLineThickness, 0u, 50u);
     widget.rgbColor("Line Color", mLineColor);

     if (auto group = widget.group("Output"))
     {
        group.textbox("Base Filename", mBaseFilename);
        group.text("Output Directory\n" + mOutputDir.string());
        group.tooltip("Relative paths are treated as relative to the runtime directory (" + getRuntimeDirectory().string() + ").");
        if (group.button("Change Folder"))
        {
            std::filesystem::path path;
            if (chooseFolderDialog(path))
                setOutputDirectory(path);
        }

        mCaptureCurrentFrame = group.button("Capture Current Frame");
     }

     if (auto group = widget.group("Controls"))
     {
        group.text("Right Click + Drag: Control the seperation line");
        group.separator();
        group.text("Num 7: Switch Image0");
        group.text("Num 9: Switch Image1");
        group.text("Num 1: Switch Image2");
        group.text("Num 3: Switch Image3");
        group.separator();
        group.text("Num 4: Only show Image0");
        group.text("Num 8: Only show Image1");
        group.text("Num 2: Only show Image2");
        group.text("Num 6: Only show Image3");
        group.text("Num 5: Disable show one");
     }
}

bool ErrorOverlay::onMouseEvent(const MouseEvent& mouseEvent) {

    if (mouseEvent.type == MouseEvent::Type::ButtonDown && mouseEvent.button == Input::MouseButton::Right)
    {
        mMouseDown = true;
        return true;
    }
    else if (mouseEvent.type == MouseEvent::Type::ButtonUp && mouseEvent.button == Input::MouseButton::Right)
    {
        mMouseDown = false;
        return true;
    }

    if (mMouseDown)
    {
        mMousePos = mouseEvent.screenPos;
        return false;
    }

    return false;
}

bool ErrorOverlay::onKeyEvent(const KeyboardEvent& keyEvent)
{
    //Switch mode per button press
    auto increaseMode = [&](const uint idx){
        mImageModes[idx] = ErrorOverlayMode(((uint)mImageModes[idx] + 1) % (uint)ErrorOverlayMode::NumElements);
    };

    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad7)
    {
        increaseMode(0);
        return true;
    }
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad9)
    {
        increaseMode(1);
        return true;
    }
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad1)
    {
        increaseMode(2);
        return true;
    }
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad3)
    {
        increaseMode(3);
        return true;
    }

    //Only show one image per button press
    auto switchToID = [&](const uint id){
        if (mOnlyShowID != id) //Set to id
            mOnlyShowID = id;
        else // Disable
            mOnlyShowID = -1;
    };

    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad4)
    {
        switchToID(0);
        return true;
    }
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad8)
    {
        switchToID(1);
        return true;
    }
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad2)
    {
        switchToID(2);
        return true;
    }
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad6)
    {
        switchToID(3);
        return true;
    }
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed && keyEvent.key == Input::Key::Keypad5)
    {
        mOnlyShowID = -1;
        return true;
    }

    return false;
}

void ErrorOverlay::setOutputDirectory(const std::filesystem::path& path_)
{
    std::filesystem::path path = path_;
    if (path.is_absolute())
    {
        // Use relative path to executable directory if possible.
        auto relativePath = path.lexically_relative(getRuntimeDirectory());
        if (!relativePath.empty() && relativePath.string().find("..") == std::string::npos)
            path = relativePath;
    }
    mOutputDir = path;
}

void ErrorOverlay::captureCurrentFrame(const RenderData& renderData, const uint validImages, const uint4 imageTypes) {
    if (validImages == 0)
        return;

    auto imgTex = renderData.getTexture(kInputChannels[0].name);
    auto refTex = renderData.getTexture(kInputChannels[1].name);
    auto mseTex = renderData.getTexture(kInputChannels[2].name);
    auto flipTex = renderData.getTexture(kInputChannels[3].name);

    auto captureTexture = [&](ref<Texture>& pTex, std::string suffix) {
        //Get Path
        auto path = mOutputDir;
        if (!path.is_absolute())
            path = std::filesystem::absolute(getRuntimeDirectory() / path);
        path /= mBaseFilename;

        auto ext = Bitmap::getFileExtFromResourceFormat(pTex->getFormat());
        auto fileformat = Bitmap::getFormatFromFileExtension(ext);
        std::string filename = path.string() + "_" + suffix + "." + ext;
        Bitmap::ExportFlags flags = Bitmap::ExportFlags::None;

        pTex->captureToFile(0, 0, filename, fileformat);
    };


    for (uint i = 0; i < validImages; i++)
    {
        ErrorOverlayMode type = ErrorOverlayMode(imageTypes[i]);
        switch (type)
        {
        case Falcor::ErrorOverlayMode::Image:
            captureTexture(imgTex, "img");
            break;
        case Falcor::ErrorOverlayMode::Reference:
            captureTexture(refTex, "ref");
            break;
        case Falcor::ErrorOverlayMode::MSE:
            captureTexture(mseTex, "mse");
            break;
        case Falcor::ErrorOverlayMode::FLIP:
            captureTexture(flipTex, "flip");
            break;
        default:
            break;
        }
    }
}
