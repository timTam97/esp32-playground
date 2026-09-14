#pragma once
#include "sdkconfig.h"

// tools/build_camera.py supplies this alongside the matching lwIP archive.
// An ordinary Arduino build continues to report the SDK's stock buffer size.
#ifndef CAMERA_TCP_SEND_BUFFER_BYTES
#define CAMERA_TCP_SEND_BUFFER_BYTES CONFIG_LWIP_TCP_SND_BUF_DEFAULT
#endif

bool startCameraServer();
