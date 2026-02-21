#include "net-builder.h"

#include "presets.h"
#include "../common/utils.h"

#include "ns3/assert.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"
#include "ns3/yans-wifi-helper.h"

#include <cmath>
#include <sstream>

namespace flsim::wifi
{
using namespace ns3;

static WifiStandard
ParseWifiStandard(const std::string& standard)
{
    if (standard == "80211n")
    {
        return WIFI_STANDARD_80211n;
    }
    if (standard == "80211ac")
    {
        return WIFI_STANDARD_80211ac;
    }
    if (standard == "80211ax")
    {
        return WIFI_STANDARD_80211ax;
    }

    NS_FATAL_ERROR("Unsupported wifi standard '" << standard
                                                  << "'. Expected one of: 80211n, 80211ac, 80211ax");
}

static void
SetPointToPointDeviceRate(Ptr<NetDevice> dev, double mbps)
{
    Ptr<PointToPointNetDevice> p2p = dev->GetObject<PointToPointNetDevice>();
    if (!p2p)
    {
        NS_FATAL_ERROR("NetDevice is not a PointToPointNetDevice");
    }

    std::ostringstream oss;
    oss << mbps << "Mbps";
    p2p->SetAttribute("DataRate", DataRateValue(DataRate(oss.str())));
}

static void
AttachReceiveLossModelIfNeeded(Ptr<NetDevice> rxDev, double lossRate)
{
    if (lossRate <= 0.0)
    {
        return;
    }

    Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
    em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
    em->SetAttribute("ErrorRate", DoubleValue(lossRate));
    rxDev->SetAttribute("ReceiveErrorModel", PointerValue(em));
}

static Ptr<YansWifiChannel>
CreateWifiChannel(const ScenarioConfig& cfg)
{
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                               "Exponent",
                               DoubleValue(cfg.topology.channel.logDistanceExponent),
                               "ReferenceDistance",
                               DoubleValue(cfg.topology.channel.logDistanceReferenceDistanceM),
                               "ReferenceLoss",
                               DoubleValue(cfg.topology.channel.logDistanceReferenceLossDb));
    return channel.Create();
}

static Vector
ComputeClientPosition(const ApConfig& ap, double radiusM, uint32_t clientIndex)
{
    const double angle = (2.0 * M_PI * (clientIndex + 1)) / 16.0;
    return Vector(ap.position.x + radiusM * std::cos(angle),
                  ap.position.y + radiusM * std::sin(angle),
                  1.0);
}

BuiltNetwork
BuildHomeWifiNetwork(const ScenarioConfig& cfg, const std::vector<std::string>& clientTierByIndex)
{
    // Build complete Wi-Fi network graph and assign client/server IP endpoints.
    if (clientTierByIndex.size() != cfg.clients.numClients)
    {
        NS_FATAL_ERROR("clientTierByIndex size mismatch");
    }

    BuiltNetwork out;

    out.server = CreateObject<Node>();
    Ptr<Node> serverAp = CreateObject<Node>();
    out.serverAp = serverAp;
    out.accessPoints.Create(cfg.topology.aps.size());
    out.clients.Create(cfg.clients.numClients);

    out.all.Add(out.server);
    out.all.Add(serverAp);
    out.all.Add(out.accessPoints);
    out.all.Add(out.clients);

    InternetStackHelper internet;
    internet.Install(out.all);

    // APs <-> server-side AP (access link).
    PointToPointHelper p2p;
    std::ostringstream delay;
    delay << cfg.topology.serverSide.accessLink.oneWayDelayMs << "ms";
    p2p.SetChannelAttribute("Delay", StringValue(delay.str()));

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.0.0.0", "255.255.255.252");

    for (uint32_t apIdx = 0; apIdx < out.accessPoints.GetN(); ++apIdx)
    {
        NodeContainer pair(serverAp, out.accessPoints.Get(apIdx));
        NetDeviceContainer devs = p2p.Install(pair);

        // serverAp TX is server->clients (down), AP TX is clients->server (up).
        SetPointToPointDeviceRate(devs.Get(0), cfg.topology.serverSide.accessLink.downMbps);
        SetPointToPointDeviceRate(devs.Get(1), cfg.topology.serverSide.accessLink.upMbps);
        AttachReceiveLossModelIfNeeded(devs.Get(0), cfg.topology.serverSide.accessLink.lossRate);
        AttachReceiveLossModelIfNeeded(devs.Get(1), cfg.topology.serverSide.accessLink.lossRate);

        Ipv4InterfaceContainer ifc = ipv4.Assign(devs);
        ipv4.NewNetwork();
    }

    // server-side AP <-> server (server link).
    {
        std::ostringstream serverDelay;
        serverDelay << cfg.topology.serverSide.serverLink.oneWayDelayMs << "ms";
        p2p.SetChannelAttribute("Delay", StringValue(serverDelay.str()));

        NodeContainer pair(serverAp, out.server);
        NetDeviceContainer devs = p2p.Install(pair);

        // serverAp TX is clients->server (up), server TX is server->clients (down).
        SetPointToPointDeviceRate(devs.Get(0), cfg.topology.serverSide.serverLink.upMbps);
        SetPointToPointDeviceRate(devs.Get(1), cfg.topology.serverSide.serverLink.downMbps);
        AttachReceiveLossModelIfNeeded(devs.Get(0), cfg.topology.serverSide.serverLink.lossRate);
        AttachReceiveLossModelIfNeeded(devs.Get(1), cfg.topology.serverSide.serverLink.lossRate);

        Ipv4InterfaceContainer ifc = ipv4.Assign(devs);
        out.serverIp = ifc.GetAddress(1);
        ipv4.NewNetwork();
    }

    // Mobility for AP nodes.
    for (uint32_t apIdx = 0; apIdx < cfg.topology.aps.size(); ++apIdx)
    {
        const auto& ap = cfg.topology.aps[apIdx];
        flsim::common::InstallMobilityForNode(out.accessPoints.Get(apIdx),
                                              ap.mobility,
                                              Vector(ap.position.x,
                                                     ap.position.y,
                                                     ap.position.z),
                                              0.0,
                                              true);
    }

    // Place server-side AP and server for NetAnim (no effect on simulation).
    double avgX = 0.0;
    double maxY = 0.0;
    if (!cfg.topology.aps.empty())
    {
        for (const auto& ap : cfg.topology.aps)
        {
            avgX += ap.position.x;
            maxY = std::max(maxY, ap.position.y);
        }
        avgX /= static_cast<double>(cfg.topology.aps.size());
    }
    const double spacing = 20.0;
    Vector serverApPos(avgX, maxY + spacing, 0.0);
    if (cfg.topology.serverSide.hasServerApPosition)
    {
        serverApPos = Vector(cfg.topology.serverSide.serverApPosition.x,
                             cfg.topology.serverSide.serverApPosition.y,
                             cfg.topology.serverSide.serverApPosition.z);
    }
    flsim::common::SetConstantPosition(out.serverAp, serverApPos);

    Vector serverPos(avgX, maxY + 2.0 * spacing, 0.0);
    if (cfg.topology.serverSide.hasServerPosition)
    {
        serverPos = Vector(cfg.topology.serverSide.serverPosition.x,
                           cfg.topology.serverSide.serverPosition.y,
                           cfg.topology.serverSide.serverPosition.z);
    }
    flsim::common::SetConstantPosition(out.server, serverPos);

    out.clientIps.assign(cfg.clients.numClients, Ipv4Address("0.0.0.0"));

    Ipv4AddressHelper wifiIpv4;
    wifiIpv4.SetBase("10.1.0.0", "255.255.255.0");

    for (uint32_t apIdx = 0; apIdx < out.accessPoints.GetN(); ++apIdx)
    {
        const auto& apCfg = cfg.topology.aps[apIdx];
        const auto& apQuality = cfg.topology.apQualityPresets.at(apCfg.quality);

        Ptr<YansWifiChannel> apChannel = CreateWifiChannel(cfg);

        WifiMacHelper apMac;
        apMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(Ssid(apCfg.ssid)));

        YansWifiPhyHelper apPhy;
        apPhy.SetChannel(apChannel);
        apPhy.Set("TxPowerStart", DoubleValue(apQuality.txPowerDbm));
        apPhy.Set("TxPowerEnd", DoubleValue(apQuality.txPowerDbm));

        WifiHelper apWifi;
        apWifi.SetStandard(ParseWifiStandard(apQuality.wifiStandard));
        apWifi.SetRemoteStationManager(cfg.wifiManager);

        NetDeviceContainer apDev = apWifi.Install(apPhy, apMac, out.accessPoints.Get(apIdx));

        std::vector<uint32_t> attached;
        for (uint32_t clientIdx = 0; clientIdx < cfg.topology.clients.size(); ++clientIdx)
        {
            if (cfg.topology.clients[clientIdx].apIndex == apIdx)
            {
                attached.push_back(clientIdx);
            }
        }

        std::vector<std::pair<Ptr<NetDevice>, uint32_t>> localClientWifiDevs;
        for (uint32_t local = 0; local < attached.size(); ++local)
        {
            const uint32_t clientIdx = attached[local];
            const auto& clientCfg = cfg.topology.clients[clientIdx];
            const auto& tier = GetDeviceTier(cfg, clientTierByIndex[clientIdx]);

            Vector pos = clientCfg.hasExplicitPosition
                             ? Vector(clientCfg.x, clientCfg.y, clientCfg.z)
                             : ComputeClientPosition(apCfg, clientCfg.radiusM, local);

            MobilitySpec effectiveMobility;
            if (clientCfg.hasMobilityOverride)
            {
                effectiveMobility = clientCfg.mobility;
            }
            else
            {
                std::string mobilityPresetName = cfg.clients.mobilityPresetByClient[clientIdx];
                if (!clientCfg.mobilityPreset.empty())
                {
                    mobilityPresetName = clientCfg.mobilityPreset;
                }
                const auto& mobilityPreset = cfg.mobilityPresets.at(mobilityPresetName);
                effectiveMobility.model = mobilityPreset.model;
                effectiveMobility.speedMps = mobilityPreset.speedMps;
                const double boundRadius = std::max(10.0, clientCfg.radiusM * mobilityPreset.areaScale);
                effectiveMobility.minX = apCfg.position.x - boundRadius;
                effectiveMobility.maxX = apCfg.position.x + boundRadius;
                effectiveMobility.minY = apCfg.position.y - boundRadius;
                effectiveMobility.maxY = apCfg.position.y + boundRadius;
            }

            flsim::common::InstallMobilityForNode(out.clients.Get(clientIdx),
                                                  effectiveMobility,
                                                  pos,
                                                  effectiveMobility.speedMps,
                                                  true);

            WifiMacHelper staMac;
            staMac.SetType("ns3::StaWifiMac",
                           "Ssid",
                           SsidValue(Ssid(apCfg.ssid)),
                           "ActiveProbing",
                           BooleanValue(false));

            YansWifiPhyHelper staPhy;
            staPhy.SetChannel(apChannel);
            staPhy.Set("TxPowerStart", DoubleValue(tier.txPowerDbm));
            staPhy.Set("TxPowerEnd", DoubleValue(tier.txPowerDbm));

            WifiHelper staWifi;
            staWifi.SetStandard(ParseWifiStandard(apQuality.wifiStandard));
            staWifi.SetRemoteStationManager(cfg.wifiManager);

            NetDeviceContainer staDev = staWifi.Install(staPhy, staMac, out.clients.Get(clientIdx));
            localClientWifiDevs.emplace_back(staDev.Get(0), clientIdx);
        }

        NetDeviceContainer subnetDevs;
        subnetDevs.Add(apDev.Get(0));
        for (const auto& [dev, clientIdx] : localClientWifiDevs)
        {
            (void)clientIdx;
            subnetDevs.Add(dev);
        }

        Ipv4InterfaceContainer ifc = wifiIpv4.Assign(subnetDevs);
        for (uint32_t j = 1; j < subnetDevs.GetN(); ++j)
        {
            Ptr<NetDevice> dev = subnetDevs.Get(j);
            for (const auto& [staDev, clientIdx] : localClientWifiDevs)
            {
                if (staDev == dev)
                {
                    out.clientIps[clientIdx] = ifc.GetAddress(j);
                    break;
                }
            }
        }
        wifiIpv4.NewNetwork();
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    return out;
}

} // namespace flsim::wifi
