#include "net-builder.h"

#include "presets.h"
#include "../common/utils.h"

#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"

#include <cmath>
#include <sstream>

namespace flsim::lte
{
using namespace ns3;

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

static Vector
ComputeClientPosition(const EnbConfig& enb, double radiusM, uint32_t clientIndex)
{
    const double angle = (2.0 * M_PI * (clientIndex + 1)) / 16.0;
    return Vector(enb.position.x + radiusM * std::cos(angle),
                  enb.position.y + radiusM * std::sin(angle),
                  1.5);
}


BuiltNetwork
BuildCellularNetwork(const ScenarioConfig& cfg, const std::vector<std::string>& clientTierByIndex)
{
    // Build complete LTE/EPC topology and assign client/server IP endpoints.
    if (clientTierByIndex.size() != cfg.clients.numClients)
    {
        NS_FATAL_ERROR("clientTierByIndex size mismatch");
    }

    BuiltNetwork out;
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    // Favor high-throughput LTE settings while staying within realistic ranges.
    Config::SetDefault("ns3::LteHelper::UseIdealRrc", BooleanValue(true));
    Config::SetDefault("ns3::LteHelper::UsePdschForCqiGeneration", BooleanValue(true));
    Config::SetDefault("ns3::LteEnbRrc::EpsBearerToRlcMapping",
                       EnumValue(LteEnbRrc::RLC_UM_ALWAYS));
    // Avoid tiny default RLC buffers that throttle throughput.
    Config::SetDefault("ns3::LteRlcUm::MaxTxBufferSize", UintegerValue(10 * 1024 * 1024));
    Config::SetDefault("ns3::LteRlcAm::MaxTxBufferSize", UintegerValue(10 * 1024 * 1024));
    Config::SetDefault("ns3::LteRlcUm::EnablePdcpDiscarding", BooleanValue(false));
    // Enable carrier aggregation with a simple CC manager.
    lteHelper->SetAttribute("UseCa", BooleanValue(true));
    lteHelper->SetAttribute("NumberOfComponentCarriers", UintegerValue(2));
    lteHelper->SetEnbComponentCarrierManagerType("ns3::RrComponentCarrierManager");
    lteHelper->SetUeComponentCarrierManagerType("ns3::SimpleUeComponentCarrierManager");
    lteHelper->SetEpcHelper(epcHelper);
    // Throughput-oriented scheduler and CQI filtering.
    lteHelper->SetSchedulerType("ns3::FdMtFfMacScheduler");
    Config::SetDefault("ns3::FfMacScheduler::UlCqiFilter",
                       EnumValue(FfMacScheduler::PUSCH_UL_CQI));
    if (!cfg.topology.channel.pathlossModel.empty())
    {
        lteHelper->SetAttribute("PathlossModel",
                                StringValue(cfg.topology.channel.pathlossModel));
    }
    if (cfg.topology.channel.pathlossModel == "ns3::LogDistancePropagationLossModel")
    {
        Config::SetDefault("ns3::LogDistancePropagationLossModel::Exponent",
                           DoubleValue(cfg.topology.channel.logDistanceExponent));
        Config::SetDefault("ns3::LogDistancePropagationLossModel::ReferenceDistance",
                           DoubleValue(cfg.topology.channel.logDistanceReferenceDistanceM));
        Config::SetDefault("ns3::LogDistancePropagationLossModel::ReferenceLoss",
                           DoubleValue(cfg.topology.channel.logDistanceReferenceLossDb));
    }

    NodeContainer serverContainer;
    serverContainer.Create(1);
    out.server = serverContainer.Get(0);

    NodeContainer serverApContainer;
    serverApContainer.Create(1);
    out.serverAp = serverApContainer.Get(0);

    NodeContainer enbNodes;
    enbNodes.Create(cfg.topology.enbs.size());
    out.enbs = enbNodes;

    NodeContainer ueNodes;
    ueNodes.Create(cfg.clients.numClients);
    out.clients = ueNodes;

    out.all.Add(out.serverAp);
    out.all.Add(out.server);
    out.all.Add(out.enbs);
    out.all.Add(out.clients);

    InternetStackHelper internet;
    // Avoid IPv6 DAD issues on LTE devices; IPv4-only is sufficient here.
    internet.SetIpv6StackInstall(false);
    internet.Install(serverApContainer);
    internet.Install(serverContainer);

    Ptr<Node> pgw = epcHelper->GetPgwNode();
    NS_ABORT_MSG_UNLESS(pgw, "PGW node missing");
    // PGW is created and initialized by the EPC helper (already has Internet stack).

    PointToPointHelper entryP2p;
    std::ostringstream entryDelay;
    entryDelay << cfg.topology.serverSide.entryLink.oneWayDelayMs << "ms";
    entryP2p.SetChannelAttribute("Delay", StringValue(entryDelay.str()));
    entryP2p.SetDeviceAttribute("Mtu", UintegerValue(1500));

    NetDeviceContainer entryDevs = entryP2p.Install(pgw, out.serverAp);
    SetPointToPointDeviceRate(entryDevs.Get(0), cfg.topology.serverSide.entryLink.downMbps);
    SetPointToPointDeviceRate(entryDevs.Get(1), cfg.topology.serverSide.entryLink.upMbps);
    AttachReceiveLossModelIfNeeded(entryDevs.Get(0), cfg.topology.serverSide.entryLink.lossRate);
    AttachReceiveLossModelIfNeeded(entryDevs.Get(1), cfg.topology.serverSide.entryLink.lossRate);

    PointToPointHelper serverP2p;
    std::ostringstream serverDelay;
    serverDelay << cfg.topology.serverSide.serverLink.oneWayDelayMs << "ms";
    serverP2p.SetChannelAttribute("Delay", StringValue(serverDelay.str()));
    serverP2p.SetDeviceAttribute("Mtu", UintegerValue(1500));

    NetDeviceContainer serverDevs = serverP2p.Install(out.serverAp, out.server);
    SetPointToPointDeviceRate(serverDevs.Get(0), cfg.topology.serverSide.serverLink.downMbps);
    SetPointToPointDeviceRate(serverDevs.Get(1), cfg.topology.serverSide.serverLink.upMbps);
    AttachReceiveLossModelIfNeeded(serverDevs.Get(0), cfg.topology.serverSide.serverLink.lossRate);
    AttachReceiveLossModelIfNeeded(serverDevs.Get(1), cfg.topology.serverSide.serverLink.lossRate);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("1.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer entryIf = ipv4.Assign(entryDevs);
    ipv4.SetBase("1.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer serverIf = ipv4.Assign(serverDevs);
    out.serverIp = serverIf.GetAddress(1);

    Ipv4StaticRoutingHelper routingHelper;
    Ptr<Ipv4> serverIpv4 = out.server->GetObject<Ipv4>();
    Ptr<Ipv4> serverApIpv4 = out.serverAp->GetObject<Ipv4>();
    Ptr<Ipv4> pgwIpv4 = pgw->GetObject<Ipv4>();
    NS_ABORT_MSG_UNLESS(serverIpv4, "Server IPv4 stack missing");
    NS_ABORT_MSG_UNLESS(serverApIpv4, "Server AP IPv4 stack missing");
    NS_ABORT_MSG_UNLESS(pgwIpv4, "PGW IPv4 stack missing");

    const int32_t serverIfIndex = serverIpv4->GetInterfaceForDevice(serverDevs.Get(1));
    const int32_t serverApToPgwIfIndex = serverApIpv4->GetInterfaceForDevice(entryDevs.Get(1));
    const int32_t pgwIfIndex = pgwIpv4->GetInterfaceForDevice(entryDevs.Get(0));
    NS_ABORT_MSG_UNLESS(serverIfIndex >= 0, "Could not resolve server interface index");
    NS_ABORT_MSG_UNLESS(serverApToPgwIfIndex >= 0,
                        "Could not resolve server AP -> PGW interface index");
    NS_ABORT_MSG_UNLESS(pgwIfIndex >= 0, "Could not resolve PGW interface index");

    Ptr<Ipv4StaticRouting> remoteStaticRouting = routingHelper.GetStaticRouting(serverIpv4);
    remoteStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"),
                                           Ipv4Mask("255.0.0.0"),
                                           serverIf.GetAddress(0),
                                           static_cast<uint32_t>(serverIfIndex));

    Ptr<Ipv4StaticRouting> serverApRouting = routingHelper.GetStaticRouting(serverApIpv4);
    serverApRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"),
                                       Ipv4Mask("255.0.0.0"),
                                       entryIf.GetAddress(0),
                                       static_cast<uint32_t>(serverApToPgwIfIndex));

    Ptr<Ipv4StaticRouting> pgwRouting = routingHelper.GetStaticRouting(pgwIpv4);
    pgwRouting->AddNetworkRouteTo(Ipv4Address("1.0.1.0"),
                                  Ipv4Mask("255.255.255.0"),
                                  entryIf.GetAddress(1),
                                  static_cast<uint32_t>(pgwIfIndex));

    MobilitySpec fixedMobility;
    flsim::common::InstallMobilityForNode(out.serverAp,
                                          fixedMobility,
                                          Vector(cfg.topology.serverSide.serverApPosition.x,
                                                 cfg.topology.serverSide.serverApPosition.y,
                                                 cfg.topology.serverSide.serverApPosition.z),
                                          0.0,
                                          true);
    flsim::common::InstallMobilityForNode(out.server,
                                          fixedMobility,
                                          Vector(cfg.topology.serverSide.serverPosition.x,
                                                 cfg.topology.serverSide.serverPosition.y,
                                                 cfg.topology.serverSide.serverPosition.z),
                                          0.0,
                                          true);

    for (uint32_t enbIdx = 0; enbIdx < cfg.topology.enbs.size(); ++enbIdx)
    {
        const auto& enb = cfg.topology.enbs[enbIdx];
        flsim::common::InstallMobilityForNode(out.enbs.Get(enbIdx),
                                              enb.mobility,
                                              Vector(enb.position.x,
                                                     enb.position.y,
                                                     enb.position.z),
                                              0.0,
                                              true);
        NS_ABORT_MSG_UNLESS(out.enbs.Get(enbIdx)->GetObject<MobilityModel>(),
                            "MobilityModel missing on eNB " << enbIdx);
    }

    out.clientIps.assign(cfg.clients.numClients, Ipv4Address("0.0.0.0"));

    std::vector<Ptr<NetDevice>> enbDevs;
    enbDevs.reserve(cfg.topology.enbs.size());

    for (uint32_t enbIdx = 0; enbIdx < cfg.topology.enbs.size(); ++enbIdx)
    {
        const auto& enbCfg = cfg.topology.enbs[enbIdx];
        const auto& quality = cfg.topology.cellQualityPresets.at(enbCfg.quality);

        lteHelper->SetEnbDeviceAttribute("DlBandwidth", UintegerValue(quality.dlBandwidth));
        lteHelper->SetEnbDeviceAttribute("UlBandwidth", UintegerValue(quality.ulBandwidth));

        NetDeviceContainer enbDev = lteHelper->InstallEnbDevice(out.enbs.Get(enbIdx));
        Ptr<NetDevice> dev = enbDev.Get(0);
        Ptr<LteEnbNetDevice> lteEnb = dev->GetObject<LteEnbNetDevice>();
        if (lteEnb)
        {
            Ptr<LteEnbPhy> phy = lteEnb->GetPhy();
            if (phy)
            {
                phy->SetTxPower(quality.txPowerDbm);
            }
        }
        enbDevs.push_back(dev);
    }

    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        const auto& clientCfg = cfg.topology.clients[i];

        Vector pos = clientCfg.hasExplicitPosition
                         ? Vector(clientCfg.position.x, clientCfg.position.y, clientCfg.position.z)
                         : ComputeClientPosition(cfg.topology.enbs[clientCfg.enbIndex],
                                                 clientCfg.radiusM,
                                                 i);

        MobilitySpec effectiveMobility;
        if (clientCfg.hasMobilityOverride)
        {
            effectiveMobility = clientCfg.mobility;
        }
        else
        {
            std::string mobilityPresetName = cfg.clients.mobilityPresetByClient[i];
            if (!clientCfg.mobilityPreset.empty())
            {
                mobilityPresetName = clientCfg.mobilityPreset;
            }
            const auto& mobilityPreset = cfg.mobilityPresets.at(mobilityPresetName);
            effectiveMobility.model = mobilityPreset.model;
            effectiveMobility.speedMps = mobilityPreset.speedMps;
            const double boundRadius = std::max(10.0, clientCfg.radiusM * mobilityPreset.areaScale);
            const auto& enbCfg = cfg.topology.enbs[clientCfg.enbIndex];
            effectiveMobility.minX = enbCfg.position.x - boundRadius;
            effectiveMobility.maxX = enbCfg.position.x + boundRadius;
            effectiveMobility.minY = enbCfg.position.y - boundRadius;
            effectiveMobility.maxY = enbCfg.position.y + boundRadius;
        }

        flsim::common::InstallMobilityForNode(out.clients.Get(i),
                                              effectiveMobility,
                                              pos,
                                              effectiveMobility.speedMps,
                                              true);
        NS_ABORT_MSG_UNLESS(out.clients.Get(i)->GetObject<MobilityModel>(),
                            "MobilityModel missing on UE " << i);
    }
    NetDeviceContainer ueDevs = lteHelper->InstallUeDevice(out.clients);
    internet.Install(out.clients);

    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        const auto& tier = GetDeviceTier(cfg, clientTierByIndex[i]);

        Ptr<NetDevice> ueDev = ueDevs.Get(i);
        Ptr<LteUeNetDevice> lteUe = ueDev->GetObject<LteUeNetDevice>();
        if (lteUe)
        {
            Ptr<LteUePhy> phy = lteUe->GetPhy();
            if (phy)
            {
                phy->SetTxPower(tier.txPowerDbm);
            }
        }
    }

    Ipv4InterfaceContainer ueIfaces = epcHelper->AssignUeIpv4Address(ueDevs);

    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        const auto& clientCfg = cfg.topology.clients[i];
        Ptr<NetDevice> ueDev = ueDevs.Get(i);

        out.clientIps[i] = ueIfaces.GetAddress(i);

        Ptr<Ipv4> ueIpv4 = out.clients.Get(i)->GetObject<Ipv4>();
        NS_ABORT_MSG_UNLESS(ueIpv4, "UE IPv4 stack missing for client " << i);
        Ptr<Ipv4StaticRouting> ueStaticRouting = routingHelper.GetStaticRouting(ueIpv4);
        ueStaticRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);

        Ptr<NetDevice> enbDev = enbDevs.at(clientCfg.enbIndex);
        lteHelper->Attach(ueDev, enbDev);
    }

    return out;
}

} // namespace flsim::lte
