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
#include "CameraPath.h"
#include "Utils/Math/VectorMath.h"

#include <fstream>

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, CameraPath>();
}

CameraPath::CameraPath(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mClock = Clock();
}

Properties CameraPath::getProperties() const
{
    return {};
}

RenderPassReflection CameraPath::reflect(const CompileData& compileData)
{
    //Nothing is happening here
    RenderPassReflection reflector;
    return reflector;
}

void CameraPath::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mpCamera = mpScene->getCamera();
    //TODO look for camera path data file in same dir as the scene


    mClock.setTime(0); // Reset Time on scene load in
}

void CameraPath::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene)
        return;
    mClock.tick();  //Update Clock

    if (mRecordCameraPath)
        recordFrame();

    //Follow the camera path
    if (mCameraPath.empty() || !mUseCameraPath)
        return;

    pathFrame();
}

void CameraPath::renderUI(Gui::Widgets& widget)
{
    //Info Block
    widget.text("Loaded Path Nodes: " + std::to_string(mCameraPath.size()));


    if (mRecordCameraPath)
    {
        recordUI(widget);
        return;
    }

    if (mUseCameraPath)
    {
        pathUI(widget);
        return;
    }

    //Settings UI
    if(widget.button("Start Camera Path"))
        startPath();

    widget.tooltip("Starts the camera path. Number nodes need to be > 1");
    if (widget.button("Record Path", true))
        startRecording();

    if (widget.button("Store Camera Path"))
    {
        std::filesystem::path storePath;
        if (saveFileDialog(kCamPathFileFilters, storePath))
        {
            storeCameraPath(storePath);
        }
    }

    if (widget.button("Load Camera Path", true))
    {
        std::filesystem::path loadPath;
        if (openFileDialog(kCamPathFileFilters, loadPath))
        {
            loadCameraPathFromFile(loadPath);
        }
    }
}

void CameraPath::recordUI(Gui::Widgets& widget)
{
    widget.text("Recording ...");
    if (widget.button("Stop Recording"))
        mRecordCameraPath = false;
}

void CameraPath::pathUI(Gui::Widgets& widget)
{
    widget.text("Following the Path ...");
    widget.text("Current Node: " + std::to_string(mCurrentPathFrame));

    if (auto group = widget.group("Time Settings", true))
    {
        if (widget.var("Speed Scale", mClockTimeScale, 0.001, 100.0, 0.001f))
        {
            mClock.setTimeScale(mClockTimeScale);
        }

        if (mClock.isPaused())
        {
            if (group.button("Resume Clock"))
            {
                mClock.play();
                mCurrentPathFrame = mCurrentPauseFrame;
            }
                

            group.var("Set Node", mCurrentPathFrame, size_t(0), mCameraPath.size()-1, 1u);
        }
        else
        {
            if (group.button("Pause Clock"))
            {
                mClock.pause();
                mCurrentPauseFrame = mCurrentPathFrame;
            }
                
        }
    }

    widget.separator(4);

    if (widget.button("Stop"))
        mUseCameraPath = false;
}

void CameraPath::startRecording() {
    mCameraPath.clear();
    mCameraPath.resize(0);
    mRecordedFrames = 0;
    // Restart Clock
    if (mClock.isPaused())
        mClock.play();
    mClock.setTime(0);      

    mRecordCameraPath = true;
}

void CameraPath::recordFrame() {
    double currentTime = mClock.getTime();

    //Create the node
    CamPathData node = {};
    node.position = mpCamera->getPosition();
    node.target = mpCamera->getTarget();
    node.deltaT = mRecordedFrames > 0 ? currentTime - mLastFrameTime : 0.0;

    mCameraPath.push_back(node);

    //Update time
    mRecordedFrames++;
    mLastFrameTime = currentTime;
}

void CameraPath::startPath() {
    if (mCameraPath.size() <= 1)
    {
        reportError("Camera Path empty, could not start!");
        return;
    }
        

    mCurrentPathFrame = 0;
    mLastFrameTime = 0;
    mNextFrameTime = mCameraPath[0].deltaT;
    mUseCameraPath = true;
    mClock.setTime(0); // Restart Clock
}

void CameraPath::pathFrame() {
    double currentTime = mClock.getTime();

    //If the clock is paused, just return the current node
    if (mClock.isPaused())
    {
        const CamPathData& n = mCameraPath[mCurrentPathFrame];

        // Update Camera
        mpCamera->setPosition(n.position);
        mpCamera->setTarget(n.target);
        return;
    }

    //Find the current node
    for (uint i = 0; i < kMaxSearchedFrames; i++)
    {
        //Reset Path
        if (mCurrentPathFrame >= mCameraPath.size() - 1)
        {
            mCurrentPathFrame = 0;
            mClock.setTime(0);
            mLastFrameTime = 0;
            mNextFrameTime = mCameraPath[1].deltaT;
            break;
        }

        //Stop if time is within the node timeframe
        if (currentTime < mNextFrameTime)
            break;

        //Else update to next node pair
        mLastFrameTime = mNextFrameTime;
        mNextFrameTime += mCameraPath[mCurrentPathFrame].deltaT;
        mCurrentPathFrame++;
    }

    double dT = currentTime - mLastFrameTime;

    float lerpVal = dT / std::max(1e-15, mNextFrameTime - mLastFrameTime);

    //Set the Camera values to the lerp of the next two nodes
    const CamPathData& n1 = mCameraPath[mCurrentPathFrame];
    const CamPathData& n2 = mCameraPath[mCurrentPathFrame + 1];
    float3 position = math::lerp(n1.position, n2.position, lerpVal);
    float3 target = math::lerp(n1.target, n2.target, lerpVal);

    //Update Camera
    mpCamera->setPosition(position);
    mpCamera->setTarget(target);
}

std::string float3VecToString(float3 vec) {
    std::string out = std::to_string(vec.x) + "," + std::to_string(vec.y) + "," + std::to_string(vec.z);
    return out;
}

bool CameraPath::storeCameraPath(std::filesystem::path& path) {
    if (mCameraPath.size() <= 1)
    {
        reportError("Camera Path empty, could not store!");
        return false;
    }
           
    std::ofstream file(path.string(), std::ios::trunc);

    if (!file)
    {
        reportError("Could not store file at " + path.string());
        return false;
    }

    // Write into file
    file << std::fixed << std::setprecision(32);
    //Loop over all nodes
    for (size_t i = 0; i < mCameraPath.size(); i++)
    {
        auto& n = mCameraPath[i];
        std::string line = float3VecToString(n.position) + "," + float3VecToString(n.target) + "," + std::to_string(n.deltaT);
        file << line;
        file << std::endl;
    }
    file.close();

    return true;
}

bool CameraPath::loadCameraPathFromFile(std::filesystem::path& path) {

    std::ifstream file(path.string());

    if (!file.is_open())
    {
        reportError("Could not open file");
        return false;
    }

    std::vector<CamPathData> readData;

    std::string line;
    while (std::getline(file, line))
    {
        std::vector<std::string> valuesStr;
        std::stringstream ss(line);
        std::string lineValue;
        while (std::getline(ss, lineValue, ','))
        {
            valuesStr.push_back(lineValue);
        }

        if (valuesStr.size() != 7)
        {
            reportError("CameraPath file format error!");
            return false;
        }

        //Create and fill node
        CamPathData n;
        n.position.x = std::stof(valuesStr[0]);
        n.position.y = std::stof(valuesStr[1]);
        n.position.z = std::stof(valuesStr[2]);
        n.target.x = std::stof(valuesStr[3]);
        n.target.y = std::stof(valuesStr[4]);
        n.target.z = std::stof(valuesStr[5]);
        n.deltaT = std::stod(valuesStr[6]);

        readData.push_back(n);
    }

    if (readData.size() <= 1)
    {
        reportError("Read Camera Path too small, ignored");
        return false;
    }

    mCameraPath = readData;
    mRecordedFrames = mCameraPath.size() - 1;

    return true;
}
