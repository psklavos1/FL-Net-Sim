#pragma once

#include "config.h"

#include "ns3/internet-module.h"
#include "ns3/network-module.h"

#include <vector>

namespace flsim::wifi
{

// Runtime network handles returned by the Wi-Fi topology builder.
struct BuiltNetwork
{
    ns3::Ptr<ns3::Node> server;
    ns3::Ptr<ns3::Node> serverAp;
    ns3::NodeContainer accessPoints;
    ns3::NodeContainer clients;

    ns3::NodeContainer all;

    std::vector<ns3::Ipv4Address> clientIps;
    ns3::Ipv4Address serverIp;
};

// Build Wi-Fi topology and install base networking stack.
BuiltNetwork BuildHomeWifiNetwork(const ScenarioConfig& cfg,
                                  const std::vector<std::string>& clientTierByIndex);

} // namespace flsim::wifi
