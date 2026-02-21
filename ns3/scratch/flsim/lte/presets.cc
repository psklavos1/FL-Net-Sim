#include "presets.h"

#include "ns3/core-module.h"

namespace flsim::lte
{

const DeviceTier&
GetDeviceTier(const ScenarioConfig& cfg, const std::string& tierName)
{
    auto it = cfg.deviceTiers.find(tierName);
    if (it == cfg.deviceTiers.end())
    {
        NS_FATAL_ERROR("Device tier '" << tierName << "' not found");
    }
    return it->second;
}

std::vector<std::string>
ExpandDeviceTierAssignment(const ScenarioConfig& cfg)
{
    std::vector<std::string> tiers;
    tiers.reserve(cfg.clients.numClients);
    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        const std::string tierName = cfg.clients.deviceTierByClient[i];
        GetDeviceTier(cfg, tierName);
        tiers.push_back(tierName);
    }
    return tiers;
}

} // namespace flsim::lte
