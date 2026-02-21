#pragma once

#include "config.h"

#include "ns3/internet-module.h"
#include "ns3/network-module.h"

#include <vector>

namespace flsim::lte
{

// Runtime network handles returned by the LTE topology builder.
struct BuiltNetwork
{
    ns3::Ptr<ns3::Node> serverAp;
    ns3::Ptr<ns3::Node> server;
    ns3::NodeContainer enbs;
    ns3::NodeContainer clients;

    ns3::NodeContainer all;

    std::vector<ns3::Ipv4Address> clientIps;
    ns3::Ipv4Address serverIp;
};

// Build LTE topology (EPC + eNB/UE + server entry + server link) and base IP routing.
BuiltNetwork BuildCellularNetwork(const ScenarioConfig& cfg,
                                  const std::vector<std::string>& clientTierByIndex);

} // namespace flsim::lte
