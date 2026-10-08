#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Settings.h"

struct EmbeddedOnvifSettings
{
    uint16_t rtspPort = 554;
    std::string rtspPath = "pc-screen";
};

std::string GetLocalIp();
void UpdateEmbeddedOnvifLiveSettings(const AppSettings& settings);

EmbeddedOnvifSettings StartEmbeddedOnvifServer(
    const AppSettings& settings,
    const std::function<void(const std::string&)>& logger = {},
    uint16_t discoveryPort = 3702);
