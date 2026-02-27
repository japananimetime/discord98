#pragma once

#include "Main.hpp"
#include <vector>
#include <mutex>
#include <string>

#define DM_STREAM_VIEWER_CLASS       TEXT("DMStreamViewerClass")
#define DM_STREAM_VIEWER_CHILD_CLASS TEXT("DMStreamViewerChildClass")

bool RegisterStreamViewerClass();
void CreateStreamViewerWindow(const std::string& streamerName);
void KillStreamViewerWindow();

// Called from StreamViewer when a frame is decoded
void StreamViewerOnFrame(const uint8_t* bgraPixels, int width, int height);
