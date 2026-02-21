#include "common/json.hpp"
#include "common/utils.h"
#include "wifi/config.h"
#include "wifi/net-builder.h"
#include "wifi/presets.h"

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mtp-interface.h"
#include "ns3/netanim-module.h"
#include "ns3/network-module.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("HomeWifi");

// Wi-Fi scenario entrypoint.
//
// Execution flow:
// 1) resolve scenario config (defaults + JSON + CLI),
// 2) build network topology,
// 3) run one FL communication round,
// 4) export summary + optional flow/netanim artifacts.
//
// Connectivity:
/*
   Client_0 )) Wi-Fi )) HomeAP_0 -- access_link --\
   Client_1 )) Wi-Fi )) HomeAP_0 -- access_link ----+-- ServerAP -- server_link -- Server
   Client_N )) Wi-Fi )) HomeAP_j -- access_link --/
                         (j from network.clients[i].ap)
*/

struct ClientRoundStats
{
    bool selected = false;

    Time dlStart = Seconds(0);
    Time dlEnd = Seconds(0);
    Time ulStart = Seconds(0);
    Time ulEnd = Seconds(0);

    uint64_t dlBytes = 0;
    uint64_t ulBytes = 0;

    bool dlDone = false;
    bool ulDone = false;
};

static nlohmann::json
BuildExperimentConfigJson(const flsim::wifi::ScenarioConfig& cfg)
{
    // Serialize resolved config to a canonical shape used for record storage.
    using json = nlohmann::json;
    json root;
    root["network_type"] = "wifi";
    root["description"] = cfg.description;
    root["reproducibility"] = {{"round", cfg.round}, {"seed", cfg.seed}};
    root["sim"] = {{"simulation_time", cfg.sim.stopS}, {"poll_ms", cfg.sim.pollMs}};

    json serverSide = {{"access_link",
                        {{"up_mbps", cfg.topology.serverSide.accessLink.upMbps},
                         {"down_mbps", cfg.topology.serverSide.accessLink.downMbps},
                         {"oneway_delay_ms", cfg.topology.serverSide.accessLink.oneWayDelayMs},
                         {"loss", cfg.topology.serverSide.accessLink.lossRate}}},
                       {"server_link",
                        {{"up_mbps", cfg.topology.serverSide.serverLink.upMbps},
                         {"down_mbps", cfg.topology.serverSide.serverLink.downMbps},
                         {"oneway_delay_ms", cfg.topology.serverSide.serverLink.oneWayDelayMs},
                         {"loss", cfg.topology.serverSide.serverLink.lossRate}}}};

    if (cfg.topology.serverSide.hasServerApPosition)
    {
        serverSide["server_ap"] = {{"position",
                                    {{"x", cfg.topology.serverSide.serverApPosition.x},
                                     {"y", cfg.topology.serverSide.serverApPosition.y},
                                     {"z", cfg.topology.serverSide.serverApPosition.z}}}};
    }
    if (cfg.topology.serverSide.hasServerPosition)
    {
        serverSide["server"] = {{"position",
                                 {{"x", cfg.topology.serverSide.serverPosition.x},
                                  {"y", cfg.topology.serverSide.serverPosition.y},
                                  {"z", cfg.topology.serverSide.serverPosition.z}}}};
    }

    json apQuality = json::object();
    for (const auto& kv : cfg.topology.apQualityPresets)
    {
        apQuality[kv.first] = {{"wifi_standard", kv.second.wifiStandard},
                               {"tx_power_dbm", kv.second.txPowerDbm}};
    }

    json channel = {
        {"log_distance_exponent", cfg.topology.channel.logDistanceExponent},
        {"log_distance_reference_distance_m", cfg.topology.channel.logDistanceReferenceDistanceM},
        {"log_distance_reference_loss_db", cfg.topology.channel.logDistanceReferenceLossDb}};

    json aps = json::array();
    for (const auto& ap : cfg.topology.aps)
    {
        json mob = {{"model", ap.mobility.model},
                    {"speed_mps", ap.mobility.speedMps},
                    {"min_x", ap.mobility.minX},
                    {"max_x", ap.mobility.maxX},
                    {"min_y", ap.mobility.minY},
                    {"max_y", ap.mobility.maxY}};
        aps.push_back({{"ssid", ap.ssid},
                       {"ap_quality", ap.quality},
                       {"position",
                        {{"x", ap.position.x}, {"y", ap.position.y}, {"z", ap.position.z}}},
                       {"mobility", mob}});
    }

    std::vector<bool> selectedByClient(cfg.clients.numClients, false);
    for (auto idx : cfg.clients.selectedClients)
    {
        if (idx < selectedByClient.size())
        {
            selectedByClient[idx] = true;
        }
    }

    json clientNodes = json::array();
    for (uint32_t i = 0; i < cfg.topology.clients.size(); ++i)
    {
        const auto& c = cfg.topology.clients[i];
        json mob = {{"model", c.mobility.model},
                    {"speed_mps", c.mobility.speedMps},
                    {"min_x", c.mobility.minX},
                    {"max_x", c.mobility.maxX},
                    {"min_y", c.mobility.minY},
                    {"max_y", c.mobility.maxY}};
        const std::string tierName =
            (i < cfg.clients.deviceTierByClient.size()) ? cfg.clients.deviceTierByClient[i] : "";
        const bool selected = (i < selectedByClient.size()) ? selectedByClient[i] : true;
        clientNodes.push_back({{"ap", c.apIndex},
                               {"device_tier", tierName},
                               {"selected", selected},
                               {"mobility_preset", c.mobilityPreset},
                               {"radius_m", c.radiusM},
                               {"has_position", c.hasExplicitPosition},
                               {"x", c.x},
                               {"y", c.y},
                               {"z", c.z},
                               {"has_mobility_override", c.hasMobilityOverride},
                               {"mobility", mob}});
    }

    json deviceTiers = json::object();
    for (const auto& kv : cfg.deviceTiers)
    {
        deviceTiers[kv.first] = {{"tx_power_dbm", kv.second.txPowerDbm}};
    }

    json mobilityPresets = json::object();
    for (const auto& kv : cfg.mobilityPresets)
    {
        mobilityPresets[kv.first] = {{"model", kv.second.model},
                                     {"speed_mps", kv.second.speedMps},
                                     {"area_scale", kv.second.areaScale}};
    }

    json tcp = {{"socket_type", cfg.fl.tcp.socketType},
                {"sack", cfg.fl.tcp.sack},
                {"snd_buf_bytes", cfg.fl.tcp.sndBufBytes},
                {"rcv_buf_bytes", cfg.fl.tcp.rcvBufBytes},
                {"segment_size_bytes", cfg.fl.tcp.segmentSizeBytes},
                {"app_send_size_bytes", cfg.fl.tcp.appSendSizeBytes}};

    json fl = {{"transport", cfg.fl.transport},
               {"model_size_mb", cfg.fl.modelSizeMb},
               {"sync_start_jitter_ms", cfg.fl.syncStartJitterMs},
               {"compute_s", cfg.fl.computeS},
               {"tcp", tcp}};

    json topo = {{"wifi_manager", cfg.wifiManager},
                 {"server_side", serverSide},
                 {"channel", channel},
                 {"access_points", aps},
                 {"clients", clientNodes}};

    root["network"] = topo;
    root["presets"] = {{"ap_quality_presets", apQuality},
                       {"device_tiers", deviceTiers},
                       {"mobility_presets", mobilityPresets}};
    root["fl_traffic"] = fl;
    return root;
}

static nlohmann::json
LoadHashJsonFromConfig(const std::string& path)
{
    // Build hash input from config while removing non-semantic metadata fields.
    // If orchestrator provides an explicit hash payload, use it directly.
    if (path.empty())
    {
        return nlohmann::json();
    }
    std::ifstream in(path);
    if (!in.is_open())
    {
        return nlohmann::json();
    }
    nlohmann::json cfg;
    try
    {
        in >> cfg;
    }
    catch (const std::exception&)
    {
        return nlohmann::json();
    }

    if (cfg.contains("hash_json") && cfg["hash_json"].is_object())
    {
        return cfg["hash_json"];
    }

    if (cfg.contains("orchestration"))
    {
        cfg.erase("orchestration");
    }
    if (cfg.contains("description"))
    {
        cfg.erase("description");
    }
    if (cfg.contains("scenario"))
    {
        cfg.erase("scenario");
    }
    if (cfg.contains("reproducibility"))
    {
        auto& repro = cfg["reproducibility"];
        repro.erase("seed");
        repro.erase("round");
        if (repro.empty())
        {
            cfg.erase("reproducibility");
        }
    }
    return cfg;
}

int
main(int argc, char* argv[])
{
    // Round execution for a single resolved Wi-Fi effective state.
    Time::SetResolution(Time::NS);

    flsim::wifi::CliOverrides o;
    bool noMtp = false;
    uint32_t mtpThreads = 0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("no-mtp", "Disable MTP simulator", noMtp);
    cmd.AddValue("mtp-threads",
                 "Enable MTP with a fixed thread count (>0); ignored when --no-mtp is set",
                 mtpThreads);
    flsim::wifi::AddCommandLineArgs(cmd, o);
    cmd.Parse(argc, argv);

    if (!noMtp)
    {
        if (mtpThreads > 0)
        {
            MtpInterface::Enable(mtpThreads);
        }
        else
        {
            MtpInterface::Enable();
        }
    }

    flsim::wifi::ScenarioConfig cfg = flsim::wifi::ResolveConfig(o);
    flsim::wifi::PrintResolvedConfig(cfg);
    std::cout << std::flush;
    RngSeedManager::SetSeed(cfg.seed);
    RngSeedManager::SetRun(cfg.round);

    TypeId tcpTid;
    NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(cfg.fl.tcp.socketType, &tcpTid),
                        "TypeId " << cfg.fl.tcp.socketType << " not found");
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(tcpTid));
    Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(cfg.fl.tcp.sack));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(cfg.fl.tcp.sndBufBytes));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(cfg.fl.tcp.rcvBufBytes));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(cfg.fl.tcp.segmentSizeBytes));

    auto tiers = flsim::wifi::ExpandDeviceTierAssignment(cfg);

    flsim::wifi::BuiltNetwork net = flsim::wifi::BuildHomeWifiNetwork(cfg, tiers);

    const uint64_t modelBytes = flsim::common::MbToBytes(cfg.fl.modelSizeMb);

    const double startJitterMs = cfg.fl.syncStartJitterMs;
    const Time roundStart = Seconds(0.10);
    const Time timeout = Seconds(cfg.sim.stopS - 0.05);

    std::vector<uint32_t> selected = cfg.clients.selectedClients;
    const uint32_t numSelected = static_cast<uint32_t>(selected.size());

    std::vector<ClientRoundStats> stats(cfg.clients.numClients);
    for (auto idx : selected)
    {
        stats[idx].selected = true;
    }

    const uint16_t dlBasePort = 5000;
    const uint16_t ulBasePort = 6000;

    std::vector<Ptr<PacketSink>> dlSinks(cfg.clients.numClients, nullptr);
    std::vector<Ptr<PacketSink>> ulSinks(cfg.clients.numClients, nullptr);

    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        PacketSinkHelper sink("ns3::TcpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), dlBasePort + i));
        auto apps = sink.Install(net.clients.Get(i));
        apps.Start(Seconds(0.0));
        apps.Stop(Seconds(cfg.sim.stopS));
        dlSinks[i] = DynamicCast<PacketSink>(apps.Get(0));
    }

    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        PacketSinkHelper sink("ns3::TcpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), ulBasePort + i));
        auto apps = sink.Install(net.server);
        apps.Start(Seconds(0.0));
        apps.Stop(Seconds(cfg.sim.stopS));
        ulSinks[i] = DynamicCast<PacketSink>(apps.Get(0));
    }

    for (uint32_t idx : selected)
    {
        BulkSendHelper sender("ns3::TcpSocketFactory",
                              InetSocketAddress(net.clientIps.at(idx), dlBasePort + idx));
        sender.SetAttribute("MaxBytes", UintegerValue(modelBytes));
        sender.SetAttribute("SendSize", UintegerValue(cfg.fl.tcp.appSendSizeBytes));

        ApplicationContainer app = sender.Install(net.server);

        Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
        uv->SetAttribute("Min", DoubleValue(0.0));
        uv->SetAttribute("Max", DoubleValue(startJitterMs / 1000.0));
        Time st = roundStart + Seconds(uv->GetValue());

        stats[idx].dlStart = st;
        if (cfg.metrics.eventLog)
        {
            NS_LOG_UNCOND("[round " << cfg.round << "] schedule DL client=" << idx
                                    << " start_s=" << st.GetSeconds() << " bytes=" << modelBytes);
        }
        app.Start(st);
        app.Stop(timeout);
    }

    const Time pollPeriod = MilliSeconds(cfg.sim.pollMs);
    std::vector<bool> uplinkScheduled(cfg.clients.numClients, false);

    std::function<void(void)> pollFn;
    pollFn = [&]() {
        const Time now = Simulator::Now();

        for (uint32_t idx : selected)
        {
            if (!stats[idx].dlDone)
            {
                uint64_t rx = dlSinks[idx]->GetTotalRx();
                stats[idx].dlBytes = rx;
                if (rx >= modelBytes)
                {
                    stats[idx].dlDone = true;
                    stats[idx].dlEnd = now;
                    if (cfg.metrics.eventLog)
                    {
                        NS_LOG_UNCOND("[round "
                                      << cfg.round << "] DL done client=" << idx
                                      << " end_s=" << now.GetSeconds() << " dur_s="
                                      << (stats[idx].dlEnd - stats[idx].dlStart).GetSeconds());
                    }

                    if (!uplinkScheduled[idx])
                    {
                        uplinkScheduled[idx] = true;
                        Time ulStart = now + Seconds(cfg.fl.computeS);

                        Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
                        uv->SetAttribute("Min", DoubleValue(0.0));
                        uv->SetAttribute("Max", DoubleValue(startJitterMs / 1000.0));
                        ulStart += Seconds(uv->GetValue());
                        stats[idx].ulStart = ulStart;

                        BulkSendHelper up("ns3::TcpSocketFactory",
                                          InetSocketAddress(net.serverIp, ulBasePort + idx));
                        up.SetAttribute("MaxBytes", UintegerValue(modelBytes));
                        up.SetAttribute("SendSize", UintegerValue(cfg.fl.tcp.appSendSizeBytes));
                        ApplicationContainer ulApp = up.Install(net.clients.Get(idx));
                        ulApp.Start(ulStart);
                        ulApp.Stop(timeout);
                        if (cfg.metrics.eventLog)
                        {
                            NS_LOG_UNCOND("[round " << cfg.round << "] schedule UL client=" << idx
                                                    << " start_s=" << ulStart.GetSeconds()
                                                    << " bytes=" << modelBytes);
                        }
                    }
                }
            }
        }

        for (uint32_t idx : selected)
        {
            if (!stats[idx].ulDone)
            {
                uint64_t rx = ulSinks[idx]->GetTotalRx();
                stats[idx].ulBytes = rx;
                if (rx >= modelBytes)
                {
                    stats[idx].ulDone = true;
                    stats[idx].ulEnd = now;
                    if (cfg.metrics.eventLog)
                    {
                        NS_LOG_UNCOND("[round "
                                      << cfg.round << "] UL done client=" << idx
                                      << " end_s=" << now.GetSeconds() << " dur_s="
                                      << (stats[idx].ulEnd - stats[idx].ulStart).GetSeconds());
                    }
                }
            }
        }

        uint32_t completed = 0;
        for (uint32_t idx : selected)
        {
            if (stats[idx].ulDone)
            {
                completed++;
            }
        }

        if (completed == numSelected || now >= timeout)
        {
            Simulator::Stop(MilliSeconds(1));
            return;
        }

        Simulator::Schedule(pollPeriod, pollFn);
    };

    Simulator::Schedule(roundStart, pollFn);

    Ptr<FlowMonitor> flowMon;
    FlowMonitorHelper flowHelper;
    if (cfg.metrics.flowMonitor)
    {
        flowMon = flowHelper.InstallAll();
    }

    std::string flowXmlName;
    std::string netAnimName;
    std::unique_ptr<AnimationInterface> anim;
    if (cfg.metrics.netAnim)
    {
        netAnimName = "netanim_" + std::to_string(cfg.round) + ".xml";
        anim = std::make_unique<AnimationInterface>(netAnimName);
        anim->SkipPacketTracing(); // avoid WifiPhyRxBeginTrace UID assertion
    }

    Simulator::Stop(Seconds(cfg.sim.stopS));
    Simulator::Run();

    const Time simEnd = Simulator::Now();
    const bool timeoutHit = (simEnd >= timeout);

    uint32_t numCompleted = 0;
    uint32_t numDlCompleted = 0;
    uint64_t totalDlBytes = 0;
    uint64_t totalUlBytes = 0;
    double maxDlDurS = 0.0;
    double maxUlDurS = 0.0;
    double maxComputeWaitS = 0.0;
    for (uint32_t idx : selected)
    {
        stats[idx].dlBytes = dlSinks[idx]->GetTotalRx();
        stats[idx].ulBytes = ulSinks[idx]->GetTotalRx();
        totalDlBytes += stats[idx].dlBytes;
        totalUlBytes += stats[idx].ulBytes;
        if (stats[idx].dlDone)
        {
            numDlCompleted++;
            maxDlDurS = std::max(maxDlDurS, (stats[idx].dlEnd - stats[idx].dlStart).GetSeconds());
        }
        if (stats[idx].ulDone)
        {
            numCompleted++;
            maxUlDurS = std::max(maxUlDurS, (stats[idx].ulEnd - stats[idx].ulStart).GetSeconds());
        }
        if (stats[idx].dlDone && stats[idx].ulStart > stats[idx].dlEnd)
        {
            maxComputeWaitS =
                std::max(maxComputeWaitS, (stats[idx].ulStart - stats[idx].dlEnd).GetSeconds());
        }
    }

    const double expectedBytes = static_cast<double>(modelBytes) * numSelected;
    const double dlCompletionRatio = expectedBytes > 0 ? totalDlBytes / expectedBytes : 0.0;
    const double ulCompletionRatio = expectedBytes > 0 ? totalUlBytes / expectedBytes : 0.0;
    const double aggDlGoodputMbps =
        flsim::common::AggregateDirectionalGoodputMbps(stats, true, simEnd);
    const double aggUlGoodputMbps =
        flsim::common::AggregateDirectionalGoodputMbps(stats, false, simEnd);

    flsim::common::FlowAggregateStats flowAgg;
    if (flowMon)
    {
        flowMon->CheckForLostPackets();
        flowAgg = flsim::common::ComputeFlowAggregateStats(flowMon->GetFlowStats());

        flowXmlName = "flowmon_" + std::to_string(cfg.round) + ".xml";
        flowMon->SerializeToXmlFile(flowXmlName, true, true);
        std::cout << "Wrote FlowMonitor: " << flowXmlName << "\n";
    }

    flsim::common::PrintRoundSummary(roundStart,
                                     simEnd,
                                     numSelected,
                                     numDlCompleted,
                                     numCompleted,
                                     maxDlDurS,
                                     maxUlDurS,
                                     maxComputeWaitS,
                                     totalDlBytes,
                                     totalUlBytes,
                                     dlCompletionRatio,
                                     ulCompletionRatio,
                                     aggDlGoodputMbps,
                                     aggUlGoodputMbps,
                                     timeoutHit,
                                     flowAgg);

    const std::string reportCsv =
        flsim::common::BuildSummaryCsvName("wifi", cfg.description, cfg.round);
    flsim::common::WriteReportCsv(reportCsv,
                                  cfg.description,
                                  cfg.round,
                                  cfg.clients.numClients,
                                  numSelected,
                                  tiers,
                                  stats,
                                  modelBytes,
                                  flowAgg,
                                  roundStart,
                                  simEnd,
                                  numCompleted,
                                  timeoutHit);
    std::cout << "Wrote round report: " << reportCsv << "\n";

    nlohmann::json expJson = BuildExperimentConfigJson(cfg);
    nlohmann::json hashJson = LoadHashJsonFromConfig(o.configPath);
    if (!hashJson.is_object())
    {
        hashJson = expJson;
    }
    const std::string summary =
        "wifi_manager=" + cfg.wifiManager + "; aps=" + std::to_string(cfg.topology.aps.size()) +
        "; server_access=" + std::to_string(cfg.topology.serverSide.accessLink.upMbps) + "/" +
        std::to_string(cfg.topology.serverSide.accessLink.downMbps) + "Mbps" +
        "; server_link=" + std::to_string(cfg.topology.serverSide.serverLink.upMbps) + "/" +
        std::to_string(cfg.topology.serverSide.serverLink.downMbps) + "Mbps";

    const flsim::common::RecordedFiles recorded =
        flsim::common::RecordOutputsWithHash(expJson,
                                             hashJson,
                                             cfg.round,
                                             cfg.seed,
                                             cfg.fl.modelSizeMb,
                                             cfg.fl.computeS,
                                             cfg.clients.numClients,
                                             cfg.clients.selectedClients.size(),
                                             summary,
                                             reportCsv,
                                             flowXmlName,
                                             netAnimName);
    std::cout << "Recorded outputs: " << recorded.roundDir.generic_string() << "\n";
    std::cout << "Recorded round report: " << recorded.roundCsv.generic_string() << "\n";
    if (!recorded.flowmonXml.empty())
    {
        std::cout << "Recorded FlowMonitor: " << recorded.flowmonXml.generic_string() << "\n";
    }
    if (!recorded.netanimXml.empty())
    {
        std::cout << "Recorded NetAnim: " << recorded.netanimXml.generic_string() << "\n";
    }
    std::cout << "\n";

    Simulator::Destroy();
    return 0;
}
