#include "common/utils.h"

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/netanim-module.h"
#include "ns3/mtp-interface.h"

#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("CellularLte");

// LTE scenario entrypoint.
//
// Execution flow:
// 1) resolve runtime config (defaults + JSON + CLI),
// 2) build LTE/EPC topology,
// 3) run one FL communication round,
// 4) export summary + optional flow/netanim artifacts.
//
// Connectivity:
//   UE_0 )) LTE )) eNB_0 --+
//   UE_1 )) LTE )) eNB_0 ----+-- EPC/PGW -- entry_link -- ServerAP -- server_link -- Server
//   UE_N )) LTE )) eNB_j --+
//                 (j from network.clients[i].enb)

struct MobilitySpec
{
    std::string model = "constant_position";
    double speedMps = 0.0;
    double minX = -50.0;
    double maxX = 50.0;
    double minY = -50.0;
    double maxY = 50.0;
};

struct MobilityPreset
{
    std::string model = "constant_position";
    double speedMps = 0.0;
    double areaScale = 1.0;
};

struct CellQualityPreset
{
    double txPowerDbm = 30.0;
    uint16_t dlBandwidth = 50;
    uint16_t ulBandwidth = 50;
};

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

static std::vector<uint32_t>
ParseIndexList(const std::string& text)
{
    std::vector<uint32_t> out;
    std::string token;
    std::istringstream iss(text);
    while (std::getline(iss, token, ','))
    {
        if (token.empty())
        {
            continue;
        }
        out.push_back(static_cast<uint32_t>(std::stoul(token)));
    }
    return out;
}

static std::string
ToLower(std::string input)
{
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return input;
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

static bool
LoadConfigOverrides(const std::string& path,
                    std::string& description,
                    uint32_t& round,
                    uint64_t& seed,
                    uint32_t& numUes,
                    uint32_t& numEnbs,
                    Time& simTime,
                    double& pollMs,
                    double& modelSizeMb,
                    uint32_t& packetSize,
                    double& computeS,
                    double& startJitterMs,
                    std::string& transport,
                    std::string& tcpSocketType,
                    bool& tcpSack,
                    uint32_t& tcpSndBufBytes,
                    uint32_t& tcpRcvBufBytes,
                    uint32_t& tcpSegmentSizeBytes,
                    uint32_t& appSendSizeBytes,
                    std::vector<uint32_t>& selectedFromConfig,
                    std::vector<std::string>& deviceTierByClient,
                    std::vector<Vector>& enbPositions,
                    std::vector<double>& enbTxPowerDbm,
                    std::vector<uint16_t>& enbDlBandwidth,
                    std::vector<uint16_t>& enbUlBandwidth,
                    std::map<std::string, double>& deviceTierTxPowerDbm,
                    std::map<std::string, double>& deviceTierNoiseFigureDb,
                    Vector& serverApPosition,
                    Vector& serverPosition,
                    double& entryUpMbps,
                    double& entryDownMbps,
                    double& entryDelayMs,
                    double& entryLossRate,
                    double& serverUpMbps,
                    double& serverDownMbps,
                    double& serverDelayMs,
                    double& serverLossRate,
                    std::string& pathlossModel,
                    double& logDistanceExponent,
                    double& logDistanceReferenceDistanceM,
                    double& logDistanceReferenceLossDb,
                    bool& useIdealRrc,
                    bool& useCa,
                    uint16_t& numComponentCarriers,
                    std::string& schedulerType,
                    std::string& rlcMode,
                    uint32_t& rlcBufferBytes,
                    bool& enableUplinkPowerControl,
                    std::string& attachMode,
                    std::vector<Vector>& uePositions,
                    std::vector<uint32_t>& ueEnbIndex,
                    std::vector<double>& ueRadius,
                    std::vector<MobilitySpec>& ueMobility,
                    std::vector<bool>& ueHasExplicitPosition,
                    std::vector<bool>& ueHasMobilityOverride,
                    std::map<std::string, MobilityPreset>& mobilityPresets,
                    std::vector<std::string>& mobilityByClient,
                    bool& flowMonitor,
                    bool& eventLog,
                    bool& netAnim)
{
    // Load optional JSON overrides on top of in-code defaults.
    if (path.empty())
    {
        return false;
    }

    std::ifstream in(path);
    if (!in.is_open())
    {
        std::cerr << "[lte] config not found: " << path << "\n";
        return false;
    }

    nlohmann::json cfg;
    try
    {
        in >> cfg;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[lte] failed to parse config: " << e.what() << "\n";
        return false;
    }

    std::map<std::string, CellQualityPreset> qualityPresets;

    if (cfg.contains("description"))
    {
        description = cfg["description"].get<std::string>();
    }

    if (cfg.contains("reproducibility"))
    {
        const auto& repro = cfg["reproducibility"];
        if (repro.contains("round"))
        {
            round = repro["round"].get<uint32_t>();
        }
        if (repro.contains("seed"))
        {
            seed = repro["seed"].get<uint64_t>();
        }
    }

    if (cfg.contains("sim") && cfg["sim"].contains("simulation_time"))
    {
        simTime = Seconds(cfg["sim"]["simulation_time"].get<double>());
    }
    if (cfg.contains("sim") && cfg["sim"].contains("poll_ms"))
    {
        pollMs = cfg["sim"]["poll_ms"].get<double>();
    }

    if (cfg.contains("fl_traffic"))
    {
        const auto& fl = cfg["fl_traffic"];
        if (fl.contains("transport"))
        {
            transport = fl["transport"].get<std::string>();
        }
        if (fl.contains("model_size_mb"))
        {
            modelSizeMb = fl["model_size_mb"].get<double>();
        }
        if (fl.contains("compute_s"))
        {
            computeS = fl["compute_s"].get<double>();
        }
        if (fl.contains("sync_start_jitter_ms"))
        {
            startJitterMs = fl["sync_start_jitter_ms"].get<double>();
        }
        if (fl.contains("tcp"))
        {
            const auto& tcp = fl["tcp"];
            if (tcp.contains("socket_type"))
            {
                tcpSocketType = tcp["socket_type"].get<std::string>();
            }
            if (tcp.contains("sack"))
            {
                tcpSack = tcp["sack"].get<bool>();
            }
            if (tcp.contains("snd_buf_bytes"))
            {
                tcpSndBufBytes = tcp["snd_buf_bytes"].get<uint32_t>();
            }
            if (tcp.contains("rcv_buf_bytes"))
            {
                tcpRcvBufBytes = tcp["rcv_buf_bytes"].get<uint32_t>();
            }
            if (tcp.contains("segment_size_bytes"))
            {
                tcpSegmentSizeBytes = tcp["segment_size_bytes"].get<uint32_t>();
            }
            if (tcp.contains("app_send_size_bytes"))
            {
                appSendSizeBytes = tcp["app_send_size_bytes"].get<uint32_t>();
                packetSize = appSendSizeBytes;
            }
        }
    }

    if (cfg.contains("presets") && cfg["presets"].contains("mobility_presets"))
    {
        const auto& presets = cfg["presets"]["mobility_presets"];
        mobilityPresets.clear();
        for (auto it = presets.begin(); it != presets.end(); ++it)
        {
            MobilityPreset mp;
            const auto& v = it.value();
            mp.model = v.value("model", "constant_position");
            mp.speedMps = v.value("speed_mps", 0.0);
            mp.areaScale = v.value("area_scale", 1.0);
            mobilityPresets[it.key()] = mp;
        }
    }
    if (cfg.contains("presets") && cfg["presets"].contains("cell_quality_presets"))
    {
        const auto& presets = cfg["presets"]["cell_quality_presets"];
        for (auto it = presets.begin(); it != presets.end(); ++it)
        {
            CellQualityPreset p = qualityPresets.count(it.key()) ? qualityPresets[it.key()]
                                                                 : CellQualityPreset{};
            const auto& v = it.value();
            p.txPowerDbm = v.value("tx_power_dbm", 30.0);
            p.dlBandwidth = v.value("dl_bandwidth", 50);
            p.ulBandwidth = v.value("ul_bandwidth", 50);
            qualityPresets[it.key()] = p;
        }
    }

    if (cfg.contains("presets") && cfg["presets"].contains("device_tiers"))
    {
        const auto& tiers = cfg["presets"]["device_tiers"];
        for (auto it = tiers.begin(); it != tiers.end(); ++it)
        {
            const auto& v = it.value();
            deviceTierTxPowerDbm[it.key()] = v.value("tx_power_dbm", 23.0);
            deviceTierNoiseFigureDb[it.key()] = v.value("noise_figure_db", 9.0);
        }
    }

    if (cfg.contains("network") && cfg["network"].contains("server_side"))
    {
        const auto& serverSide = cfg["network"]["server_side"];

        if (serverSide.contains("server_ap") && serverSide["server_ap"].contains("position"))
        {
            const auto& p = serverSide["server_ap"]["position"];
            serverApPosition =
                Vector(p.value("x", serverApPosition.x),
                       p.value("y", serverApPosition.y),
                       p.value("z", serverApPosition.z));
        }
        if (serverSide.contains("server") && serverSide["server"].contains("position"))
        {
            const auto& p = serverSide["server"]["position"];
            serverPosition = Vector(p.value("x", serverPosition.x),
                                    p.value("y", serverPosition.y),
                                    p.value("z", serverPosition.z));
        }
        if (serverSide.contains("entry_link"))
        {
            const auto& link = serverSide["entry_link"];
            entryUpMbps = link.value("up_mbps", entryUpMbps);
            entryDownMbps = link.value("down_mbps", entryDownMbps);
            entryDelayMs = link.value("oneway_delay_ms", entryDelayMs);
            entryLossRate = link.value("loss", entryLossRate);
        }
        if (serverSide.contains("server_link"))
        {
            const auto& link = serverSide["server_link"];
            serverUpMbps = link.value("up_mbps", serverUpMbps);
            serverDownMbps = link.value("down_mbps", serverDownMbps);
            serverDelayMs = link.value("oneway_delay_ms", serverDelayMs);
            serverLossRate = link.value("loss", serverLossRate);
        }
    }

    if (cfg.contains("network") && cfg["network"].contains("channel"))
    {
        const auto& ch = cfg["network"]["channel"];
        pathlossModel = ch.value("pathloss_model", pathlossModel);
        logDistanceExponent = ch.value("log_distance_exponent", logDistanceExponent);
        logDistanceReferenceDistanceM =
            ch.value("log_distance_reference_distance_m", logDistanceReferenceDistanceM);
        logDistanceReferenceLossDb =
            ch.value("log_distance_reference_loss_db", logDistanceReferenceLossDb);
    }

    if (cfg.contains("lte") && cfg["lte"].is_object())
    {
        const auto& lte = cfg["lte"];
        useIdealRrc = lte.value("use_ideal_rrc", useIdealRrc);
        useCa = lte.value("use_ca", useCa);
        numComponentCarriers = static_cast<uint16_t>(
            lte.value("num_component_carriers", static_cast<int>(numComponentCarriers)));
        schedulerType = lte.value("scheduler", schedulerType);
        rlcMode = lte.value("rlc_mode", rlcMode);
        rlcBufferBytes = lte.value("rlc_buffer_bytes", rlcBufferBytes);
        enableUplinkPowerControl =
            lte.value("enable_uplink_power_control", enableUplinkPowerControl);
        attachMode = lte.value("attach_mode", attachMode);
    }

    if (cfg.contains("network") && cfg["network"].contains("enbs"))
    {
        const auto& enbs = cfg["network"]["enbs"];
        numEnbs = static_cast<uint32_t>(enbs.size());
        enbPositions.clear();
        enbTxPowerDbm.assign(enbs.size(), 30.0);
        enbDlBandwidth.assign(enbs.size(), 50);
        enbUlBandwidth.assign(enbs.size(), 50);

        for (const auto& enb : enbs)
        {
            const size_t idx = enbPositions.size();
            if (enb.contains("position"))
            {
                const auto& p = enb["position"];
                enbPositions.emplace_back(p.value("x", 0.0), p.value("y", 0.0), p.value("z", 0.0));
            }
            else
            {
                enbPositions.emplace_back(0.0, 0.0, 0.0);
            }

            if (enb.contains("cell_quality"))
            {
                const std::string quality = enb["cell_quality"].get<std::string>();
                auto it = qualityPresets.find(quality);
                if (it != qualityPresets.end() && idx < enbTxPowerDbm.size())
                {
                    enbTxPowerDbm[idx] = it->second.txPowerDbm;
                    enbDlBandwidth[idx] = it->second.dlBandwidth;
                    enbUlBandwidth[idx] = it->second.ulBandwidth;
                }
            }
        }
    }

    if (cfg.contains("network") && cfg["network"].contains("clients"))
    {
        const auto& clients = cfg["network"]["clients"];
        numUes = static_cast<uint32_t>(clients.size());
        uePositions.assign(clients.size(), Vector(0.0, 0.0, 0.0));
        ueEnbIndex.assign(clients.size(), 0);
        ueRadius.assign(clients.size(), 50.0);
        ueMobility.assign(clients.size(), MobilitySpec{});
        ueHasExplicitPosition.assign(clients.size(), false);
        ueHasMobilityOverride.assign(clients.size(), false);
        deviceTierByClient.assign(clients.size(), "");
        mobilityByClient.assign(clients.size(), "");
        std::vector<bool> selectedByClient(clients.size(), true);
        bool anySelectedField = false;
        for (size_t i = 0; i < clients.size(); ++i)
        {
            const auto& c = clients[i];
            ueEnbIndex[i] = c.value("enb", 0);
            if (c.contains("device_tier"))
            {
                deviceTierByClient[i] = c["device_tier"].get<std::string>();
            }
            if (c.contains("mobility_preset"))
            {
                mobilityByClient[i] = c["mobility_preset"].get<std::string>();
            }
            if (c.contains("selected"))
            {
                selectedByClient[i] = c["selected"].get<bool>();
                anySelectedField = true;
            }
            ueRadius[i] = c.value("radius_m", 50.0);
            if (c.contains("position"))
            {
                ueHasExplicitPosition[i] = true;
                const auto& p = c["position"];
                uePositions[i] =
                    Vector(p.value("x", 0.0), p.value("y", 0.0), p.value("z", 0.0));
            }
            if (c.contains("mobility"))
            {
                ueHasMobilityOverride[i] = true;
                const auto& m = c["mobility"];
                ueMobility[i].model = m.value("model", "constant_position");
                ueMobility[i].speedMps = m.value("speed_mps", 0.0);
                ueMobility[i].minX = m.value("min_x", -50.0);
                ueMobility[i].maxX = m.value("max_x", 50.0);
                ueMobility[i].minY = m.value("min_y", -50.0);
                ueMobility[i].maxY = m.value("max_y", 50.0);
            }
        }

        for (size_t i = 0; i < clients.size(); ++i)
        {
            if (deviceTierByClient[i].empty())
            {
                deviceTierByClient[i] = "basic";
            }
            if (mobilityByClient[i].empty())
            {
                mobilityByClient[i] = "static";
            }
        }

        if (anySelectedField)
        {
            selectedFromConfig.clear();
            for (size_t i = 0; i < selectedByClient.size(); ++i)
            {
                if (selectedByClient[i])
                {
                    selectedFromConfig.push_back(static_cast<uint32_t>(i));
                }
            }
        }
        else
        {
            selectedFromConfig.clear();
            for (size_t i = 0; i < clients.size(); ++i)
            {
                selectedFromConfig.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    if (cfg.contains("metrics"))
    {
        const auto& m = cfg["metrics"];
        if (m.contains("flow_monitor"))
        {
            flowMonitor = m["flow_monitor"].get<bool>();
        }
        if (m.contains("event_log"))
        {
            eventLog = m["event_log"].get<bool>();
        }
        if (m.contains("netanim"))
        {
            netAnim = m["netanim"].get<bool>();
        }
    }

    return true;
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
    // Round execution for a single resolved LTE effective state.
    // MtpInterface::Enable();

    uint32_t round = 1;
    uint64_t seed = 1;
    uint32_t numUes = 2;
    uint32_t numEnbs = 2;
    double distance = 200.0;
    Time simTime = Seconds(5.0);
    double pollMs = 50.0;
    bool enableDl = true;
    bool enableUl = true;
    double modelSizeMb = 5.0;
    uint32_t packetSize = 1448;
    double computeS = 6.0;
    double startJitterMs = 100.0;
    std::string selectedUes = "";
    std::string configPath = "";
    std::string description = "cellular_lte";
    bool flowMonitor = true;
    bool eventLog = true;
    bool netAnim = false;
    Vector serverApPosition(0.0, 200.0, 10.0);
    Vector serverPosition(20.0, 200.0, 0.0);
    double entryUpMbps = 1000.0;
    double entryDownMbps = 1000.0;
    double entryDelayMs = 5.0;
    double entryLossRate = 0.0;
    double serverUpMbps = 5000.0;
    double serverDownMbps = 12000.0;
    double serverDelayMs = 1.0;
    double serverLossRate = 0.0;
    std::string pathlossModel = "ns3::LogDistancePropagationLossModel";
    double logDistanceExponent = 3.0;
    double logDistanceReferenceDistanceM = 1.0;
    double logDistanceReferenceLossDb = 44.0;
    std::string transport = "udp";
    std::string tcpSocketType = "ns3::TcpCubic";
    bool tcpSack = true;
    uint32_t tcpSndBufBytes = 4 * 1024 * 1024;
    uint32_t tcpRcvBufBytes = 4 * 1024 * 1024;
    uint32_t tcpSegmentSizeBytes = 1448;
    uint32_t appSendSizeBytes = 1448;
    // Throughput-oriented LTE defaults; JSON/CLI can still override these.
    std::string schedulerType = "ns3::PfFfMacScheduler";
    bool useIdealRrc = true;
    bool useCa = true;
    uint16_t numComponentCarriers = 2;
    std::string rlcMode = "um";
    uint32_t rlcBufferBytes = 10 * 1024 * 1024;
    bool enableUplinkPowerControl = true;
    std::string attachMode = "configured";

    CommandLine cmd(__FILE__);
    cmd.AddValue("config", "JSON config path (optional)", configPath);
    cmd.AddValue("round", "Experiment round", round);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("numClients", "Number of clients", numUes);
    cmd.AddValue("numUes", "Number of UEs", numUes);
    cmd.AddValue("numEnbs", "Number of eNBs", numEnbs);
    cmd.AddValue("distance", "Distance between eNBs (m)", distance);
    cmd.AddValue("simTime", "Simulation time", simTime);
    cmd.AddValue("stopS", "Simulation time", simTime);
    cmd.AddValue("pollMs", "Polling period (ms)", pollMs);
    cmd.AddValue("enableDl", "Enable downlink traffic", enableDl);
    cmd.AddValue("enableUl", "Enable uplink traffic", enableUl);
    cmd.AddValue("modelSizeMb", "Model size (MB) per transfer", modelSizeMb);
    cmd.AddValue("packetSize", "UDP packet size (bytes)", packetSize);
    cmd.AddValue("computeS", "Compute time before UL (s)", computeS);
    cmd.AddValue("startJitterMs", "Random start jitter (ms)", startJitterMs);
    cmd.AddValue("selectedUes",
                 "Comma-separated UE indices to participate (empty = all)",
                 selectedUes);
    cmd.AddValue("selectedClients",
                 "Comma-separated client indices to participate (empty = all)",
                 selectedUes);
    cmd.AddValue("flowMonitor", "Enable FlowMonitor", flowMonitor);
    cmd.AddValue("eventLog", "Enable event logging", eventLog);
    cmd.AddValue("netAnim", "Enable NetAnim output", netAnim);
    cmd.AddValue("transport", "Transport protocol (udp or tcp)", transport);
    cmd.AddValue("tcpSocketType", "TCP socket type", tcpSocketType);
    cmd.AddValue("tcpSack", "Enable TCP SACK", tcpSack);
    cmd.AddValue("tcpSndBufBytes", "TCP send buffer (bytes)", tcpSndBufBytes);
    cmd.AddValue("tcpRcvBufBytes", "TCP recv buffer (bytes)", tcpRcvBufBytes);
    cmd.AddValue("tcpSegmentSizeBytes", "TCP segment size (bytes)", tcpSegmentSizeBytes);
    cmd.AddValue("appSendSizeBytes", "App send size (bytes)", appSendSizeBytes);
    cmd.AddValue("attachMode", "UE attach mode: configured or auto", attachMode);
    cmd.Parse(argc, argv);

    std::vector<uint32_t> selectedFromConfig;
    std::vector<std::string> deviceTierByClient;
    std::vector<Vector> enbPositions;
    std::vector<double> enbTxPowerDbm;
    std::vector<uint16_t> enbDlBandwidth;
    std::vector<uint16_t> enbUlBandwidth;
    std::map<std::string, double> deviceTierTxPowerDbm = {
        {"very_weak", 10.0},
        {"weak", 14.0},
        {"basic", 18.0},
        {"strong", 21.0},
        {"very_strong", 23.0},
    };
    std::map<std::string, double> deviceTierNoiseFigureDb = {
        {"very_weak", 13.0},
        {"weak", 11.0},
        {"basic", 9.0},
        {"strong", 7.0},
        {"very_strong", 5.0},
    };
    std::vector<Vector> uePositions;
    std::vector<uint32_t> ueEnbIndex;
    std::vector<double> ueRadius;
    std::vector<MobilitySpec> ueMobility;
    std::vector<bool> ueHasExplicitPosition;
    std::vector<bool> ueHasMobilityOverride;
    std::map<std::string, MobilityPreset> mobilityPresets;
    std::vector<std::string> mobilityByClient;

    LoadConfigOverrides(configPath,
                        description,
                        round,
                        seed,
                        numUes,
                        numEnbs,
                        simTime,
                        pollMs,
                        modelSizeMb,
                        packetSize,
                        computeS,
                        startJitterMs,
                        transport,
                        tcpSocketType,
                        tcpSack,
                        tcpSndBufBytes,
                        tcpRcvBufBytes,
                        tcpSegmentSizeBytes,
                        appSendSizeBytes,
                        selectedFromConfig,
                        deviceTierByClient,
                        enbPositions,
                        enbTxPowerDbm,
                        enbDlBandwidth,
                        enbUlBandwidth,
                        deviceTierTxPowerDbm,
                        deviceTierNoiseFigureDb,
                        serverApPosition,
                        serverPosition,
                        entryUpMbps,
                        entryDownMbps,
                        entryDelayMs,
                        entryLossRate,
                        serverUpMbps,
                        serverDownMbps,
                        serverDelayMs,
                        serverLossRate,
                        pathlossModel,
                        logDistanceExponent,
                        logDistanceReferenceDistanceM,
                        logDistanceReferenceLossDb,
                        useIdealRrc,
                        useCa,
                        numComponentCarriers,
                        schedulerType,
                        rlcMode,
                        rlcBufferBytes,
                        enableUplinkPowerControl,
                        attachMode,
                        uePositions,
                        ueEnbIndex,
                        ueRadius,
                        ueMobility,
                        ueHasExplicitPosition,
                        ueHasMobilityOverride,
                        mobilityPresets,
                        mobilityByClient,
                        flowMonitor,
                        eventLog,
                        netAnim);

    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(round);

    const bool useUdp = (transport == "udp");
    const std::string socketFactory = useUdp ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory";

    if (!useUdp)
    {
        TypeId tcpTid;
        NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(tcpSocketType, &tcpTid),
                            "TypeId " << tcpSocketType << " not found");
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(tcpTid));
        Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(tcpSack));
        Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(tcpSndBufBytes));
        Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(tcpRcvBufBytes));
        Config::SetDefault("ns3::TcpSocket::SegmentSize",
                           UintegerValue(tcpSegmentSizeBytes));
    }

    const std::string rlcModeLower = ToLower(rlcMode);
    const auto rlcMapping = (rlcModeLower == "am") ? LteEnbRrc::RLC_AM_ALWAYS
                                                   : LteEnbRrc::RLC_UM_ALWAYS;
    if (!useCa)
    {
        numComponentCarriers = 1;
    }
    else if (numComponentCarriers < 2)
    {
        numComponentCarriers = 2;
    }

    std::string attachModeLower = ToLower(attachMode);
    if (attachModeLower != "configured" && attachModeLower != "auto")
    {
        NS_LOG_WARN("Unknown attachMode '" << attachMode
                                           << "'; falling back to configured");
        attachModeLower = "configured";
    }
    const bool useAutoAttach = (attachModeLower == "auto");

    const long double recommendedRlcBuffer =
        std::ceil(static_cast<long double>(modelSizeMb) * 1'000'000.0L * 2.0L);
    const uint32_t minRlcBufferBytes =
        static_cast<uint32_t>(std::min<long double>(
            static_cast<long double>(std::numeric_limits<uint32_t>::max()),
            std::max<long double>(0.0L, recommendedRlcBuffer)));
    if (rlcBufferBytes < minRlcBufferBytes)
    {
        NS_LOG_WARN("Increasing rlc_buffer_bytes from "
                    << rlcBufferBytes << " to " << minRlcBufferBytes
                    << " to avoid RLC buffer pressure for model_size_mb=" << modelSizeMb);
        rlcBufferBytes = minRlcBufferBytes;
    }

    Config::SetDefault("ns3::LteHelper::UseIdealRrc", BooleanValue(useIdealRrc));
    Config::SetDefault("ns3::LteHelper::UsePdschForCqiGeneration", BooleanValue(true));
    Config::SetDefault("ns3::LteEnbRrc::EpsBearerToRlcMapping", EnumValue(rlcMapping));
    Config::SetDefault("ns3::LteUePhy::EnableUplinkPowerControl",
                       BooleanValue(enableUplinkPowerControl));
    Config::SetDefault("ns3::LteRlcUm::MaxTxBufferSize", UintegerValue(rlcBufferBytes));
    Config::SetDefault("ns3::LteRlcAm::MaxTxBufferSize", UintegerValue(rlcBufferBytes));
    Config::SetDefault("ns3::LteRlcUm::EnablePdcpDiscarding", BooleanValue(false));

    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    lteHelper->SetEpcHelper(epcHelper);
    lteHelper->SetAttribute("UseCa", BooleanValue(useCa));
    lteHelper->SetAttribute("NumberOfComponentCarriers", UintegerValue(numComponentCarriers));
    lteHelper->SetEnbComponentCarrierManagerType("ns3::RrComponentCarrierManager");
    lteHelper->SetUeComponentCarrierManagerType("ns3::SimpleUeComponentCarrierManager");
    lteHelper->SetSchedulerType(schedulerType);
    Config::SetDefault("ns3::FfMacScheduler::UlCqiFilter",
                       EnumValue(FfMacScheduler::PUSCH_UL_CQI));

    if (!pathlossModel.empty())
    {
        TypeId pathlossType;
        NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(pathlossModel, &pathlossType),
                            "Unsupported LTE pathloss model: " << pathlossModel);
        lteHelper->SetPathlossModelType(pathlossType);
        if (pathlossModel == "ns3::LogDistancePropagationLossModel")
        {
            lteHelper->SetPathlossModelAttribute("Exponent", DoubleValue(logDistanceExponent));
            lteHelper->SetPathlossModelAttribute("ReferenceDistance",
                                                 DoubleValue(logDistanceReferenceDistanceM));
            lteHelper->SetPathlossModelAttribute("ReferenceLoss",
                                                 DoubleValue(logDistanceReferenceLossDb));
        }
    }

    Ptr<Node> pgw = epcHelper->GetPgwNode();

    NodeContainer serverApContainer;
    serverApContainer.Create(1);
    Ptr<Node> serverAp = serverApContainer.Get(0);

    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);

    InternetStackHelper internet;
    internet.Install(serverApContainer);
    internet.Install(remoteHostContainer);

    MobilitySpec fixedMobility;
    flsim::common::InstallMobilityForNode(serverAp, fixedMobility, serverApPosition, 0.0, true);
    flsim::common::InstallMobilityForNode(remoteHost,
                                          fixedMobility,
                                          serverPosition,
                                          0.0,
                                          true);

    PointToPointHelper entryP2p;
    entryP2p.SetDeviceAttribute("Mtu", UintegerValue(1500));
    entryP2p.SetChannelAttribute("Delay", TimeValue(MilliSeconds(entryDelayMs)));
    NetDeviceContainer entryDevices = entryP2p.Install(pgw, serverAp);
    // PGW TX direction carries UE->server traffic (up), server-AP TX carries down.
    SetPointToPointDeviceRate(entryDevices.Get(0), entryUpMbps);
    SetPointToPointDeviceRate(entryDevices.Get(1), entryDownMbps);
    AttachReceiveLossModelIfNeeded(entryDevices.Get(0), entryLossRate);
    AttachReceiveLossModelIfNeeded(entryDevices.Get(1), entryLossRate);

    PointToPointHelper serverP2p;
    serverP2p.SetDeviceAttribute("Mtu", UintegerValue(1500));
    serverP2p.SetChannelAttribute("Delay", TimeValue(MilliSeconds(serverDelayMs)));
    NetDeviceContainer serverDevices = serverP2p.Install(serverAp, remoteHost);
    // Server-AP TX direction carries UE->server traffic (up), server TX carries down.
    SetPointToPointDeviceRate(serverDevices.Get(0), serverUpMbps);
    SetPointToPointDeviceRate(serverDevices.Get(1), serverDownMbps);
    AttachReceiveLossModelIfNeeded(serverDevices.Get(0), serverLossRate);
    AttachReceiveLossModelIfNeeded(serverDevices.Get(1), serverLossRate);

    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer entryIfaces = ipv4h.Assign(entryDevices);
    ipv4h.SetBase("1.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer serverIfaces = ipv4h.Assign(serverDevices);
    Ipv4Address remoteHostAddr = serverIfaces.GetAddress(1);

    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    Ptr<Ipv4> remoteHostIpv4 = remoteHost->GetObject<Ipv4>();
    Ptr<Ipv4> serverApIpv4 = serverAp->GetObject<Ipv4>();
    Ptr<Ipv4> pgwIpv4 = pgw->GetObject<Ipv4>();
    NS_ABORT_MSG_UNLESS(remoteHostIpv4, "Remote host IPv4 stack missing");
    NS_ABORT_MSG_UNLESS(serverApIpv4, "Server AP IPv4 stack missing");
    NS_ABORT_MSG_UNLESS(pgwIpv4, "PGW IPv4 stack missing");

    const int32_t remoteHostIf = remoteHostIpv4->GetInterfaceForDevice(serverDevices.Get(1));
    const int32_t serverApToPgwIf = serverApIpv4->GetInterfaceForDevice(entryDevices.Get(1));
    const int32_t pgwIf = pgwIpv4->GetInterfaceForDevice(entryDevices.Get(0));
    NS_ABORT_MSG_UNLESS(remoteHostIf >= 0, "Could not resolve remote host interface");
    NS_ABORT_MSG_UNLESS(serverApToPgwIf >= 0, "Could not resolve server AP interface");
    NS_ABORT_MSG_UNLESS(pgwIf >= 0, "Could not resolve PGW interface");

    Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(remoteHostIpv4);
    remoteHostStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"),
                                               Ipv4Mask("255.0.0.0"),
                                               serverIfaces.GetAddress(0),
                                               static_cast<uint32_t>(remoteHostIf));

    Ptr<Ipv4StaticRouting> serverApStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(serverApIpv4);
    serverApStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"),
                                             Ipv4Mask("255.0.0.0"),
                                             entryIfaces.GetAddress(0),
                                             static_cast<uint32_t>(serverApToPgwIf));

    Ptr<Ipv4StaticRouting> pgwStaticRouting = ipv4RoutingHelper.GetStaticRouting(pgwIpv4);
    pgwStaticRouting->AddNetworkRouteTo(Ipv4Address("1.0.1.0"),
                                        Ipv4Mask("255.255.255.0"),
                                        entryIfaces.GetAddress(1),
                                        static_cast<uint32_t>(pgwIf));

    NodeContainer enbNodes;
    NodeContainer ueNodes;
    enbNodes.Create(numEnbs);
    ueNodes.Create(numUes);

    for (uint32_t i = 0; i < numEnbs; ++i)
    {
        Vector pos = (i < enbPositions.size()) ? enbPositions[i] : Vector(distance * i, 0.0, 0.0);
        MobilitySpec m;
        flsim::common::InstallMobilityForNode(enbNodes.Get(i), m, pos, 0.0, true);
    }

    for (uint32_t u = 0; u < numUes; ++u)
    {
        Vector pos = (u < uePositions.size()) ? uePositions[u] : Vector(0.0, 0.0, 0.0);
        if (u < ueEnbIndex.size() && u < ueRadius.size() && u < enbPositions.size())
        {
            const bool hasExplicit =
                (u < ueHasExplicitPosition.size()) ? ueHasExplicitPosition[u] : false;
            if (!hasExplicit)
            {
                const Vector enbPos = enbPositions[ueEnbIndex[u]];
                double angle = (2.0 * M_PI * (u + 1)) / 16.0;
                pos = Vector(enbPos.x + ueRadius[u] * std::cos(angle),
                             enbPos.y + ueRadius[u] * std::sin(angle),
                             1.5);
            }
        }
        MobilitySpec m = (u < ueMobility.size()) ? ueMobility[u] : MobilitySpec{};
        if (u < ueHasMobilityOverride.size() && !ueHasMobilityOverride[u])
        {
            if (u < mobilityByClient.size() && !mobilityByClient[u].empty())
            {
                auto it = mobilityPresets.find(mobilityByClient[u]);
                if (it != mobilityPresets.end())
                {
                    const MobilityPreset& p = it->second;
                    m.model = p.model;
                    m.speedMps = p.speedMps;
                    if (u < ueEnbIndex.size() && ueEnbIndex[u] < enbPositions.size())
                    {
                        const Vector enbPos = enbPositions[ueEnbIndex[u]];
                        const double boundRadius = std::max(10.0, ueRadius[u] * p.areaScale);
                        m.minX = enbPos.x - boundRadius;
                        m.maxX = enbPos.x + boundRadius;
                        m.minY = enbPos.y - boundRadius;
                        m.maxY = enbPos.y + boundRadius;
                    }
                }
            }
        }
        flsim::common::InstallMobilityForNode(ueNodes.Get(u), m, pos, m.speedMps, true);
    }

    NetDeviceContainer enbLteDevs;
    for (uint32_t i = 0; i < numEnbs; ++i)
    {
        if (i < enbDlBandwidth.size())
        {
            lteHelper->SetEnbDeviceAttribute("DlBandwidth", UintegerValue(enbDlBandwidth[i]));
        }
        if (i < enbUlBandwidth.size())
        {
            lteHelper->SetEnbDeviceAttribute("UlBandwidth", UintegerValue(enbUlBandwidth[i]));
        }
        NetDeviceContainer dev = lteHelper->InstallEnbDevice(enbNodes.Get(i));
        Ptr<NetDevice> enbDev = dev.Get(0);
        enbLteDevs.Add(enbDev);
        Ptr<LteEnbNetDevice> lteEnb = enbDev->GetObject<LteEnbNetDevice>();
        if (lteEnb && i < enbTxPowerDbm.size())
        {
            Ptr<LteEnbPhy> phy = lteEnb->GetPhy();
            if (phy)
            {
                phy->SetTxPower(enbTxPowerDbm[i]);
            }
        }
    }

    NetDeviceContainer ueLteDevs = lteHelper->InstallUeDevice(ueNodes);

    for (uint32_t u = 0; u < ueLteDevs.GetN(); ++u)
    {
        std::string tierName =
            (u < deviceTierByClient.size()) ? deviceTierByClient[u] : "basic";
        auto it = deviceTierTxPowerDbm.find(tierName);
        auto noiseIt = deviceTierNoiseFigureDb.find(tierName);
        const double txPowerDbm = (it != deviceTierTxPowerDbm.end()) ? it->second : 23.0;
        const double noiseFigureDb = (noiseIt != deviceTierNoiseFigureDb.end())
                                         ? noiseIt->second
                                         : 9.0;

        Ptr<NetDevice> ueDev = ueLteDevs.Get(u);
        Ptr<LteUeNetDevice> lteUe = ueDev->GetObject<LteUeNetDevice>();
        if (lteUe)
        {
            Ptr<LteUePhy> phy = lteUe->GetPhy();
            if (phy)
            {
                phy->SetTxPower(txPowerDbm);
                phy->SetNoiseFigure(noiseFigureDb);
            }
        }
    }

    internet.Install(ueNodes);
    Ipv4InterfaceContainer ueIpIfaces = epcHelper->AssignUeIpv4Address(ueLteDevs);
    for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
    {
        Ptr<Ipv4StaticRouting> ueStaticRouting =
            ipv4RoutingHelper.GetStaticRouting(ueNodes.Get(u)->GetObject<Ipv4>());
        ueStaticRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
    }

    if (useAutoAttach)
    {
        // Cell-selection attach as in LTE examples using EPC.
        lteHelper->Attach(ueLteDevs);
    }
    else
    {
        // Deterministic attach to configured serving eNB indices.
        for (uint32_t u = 0; u < ueLteDevs.GetN(); ++u)
        {
            const uint32_t enbIdx = (u < ueEnbIndex.size()) ? ueEnbIndex[u] : 0;
            if (enbIdx < enbLteDevs.GetN())
            {
                lteHelper->Attach(ueLteDevs.Get(u), enbLteDevs.Get(enbIdx));
            }
            else
            {
                NS_LOG_WARN("Client " << u << " has invalid enb index " << enbIdx
                                      << "; falling back to auto-attach");
                lteHelper->Attach(ueLteDevs.Get(u));
            }
        }
    }

    const uint64_t modelBytes = flsim::common::MbToBytes(modelSizeMb);
    const uint32_t maxPackets =
        packetSize > 0 ? static_cast<uint32_t>((modelBytes + packetSize - 1) / packetSize) : 0;

    std::vector<uint32_t> selected =
        !selectedUes.empty() ? ParseIndexList(selectedUes) : selectedFromConfig;
    std::vector<bool> isSelected(ueNodes.GetN(), true);
    if (!selected.empty())
    {
        std::fill(isSelected.begin(), isSelected.end(), false);
        for (uint32_t idx : selected)
        {
            if (idx < isSelected.size())
            {
                isSelected[idx] = true;
            }
        }
    }

    // For TCP, delay application start slightly to avoid early SYN loss during attach/setup.
    const Time roundStart = useUdp ? Seconds(0.05) : Seconds(0.20);
    const Time timeout = simTime - MilliSeconds(50);
    const Time pollPeriod = MilliSeconds(pollMs);

    std::vector<ClientRoundStats> stats(ueNodes.GetN());
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        stats[i].selected = isSelected[i];
    }

    const uint16_t dlBasePort = 5000;
    const uint16_t ulBasePort = 6000;

    std::vector<Ptr<PacketSink>> dlSinks(ueNodes.GetN(), nullptr);
    std::vector<Ptr<PacketSink>> ulSinks(ueNodes.GetN(), nullptr);

    for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
    {
        PacketSinkHelper dlSink(socketFactory,
                                InetSocketAddress(Ipv4Address::GetAny(), dlBasePort + u));
        auto dlApps = dlSink.Install(ueNodes.Get(u));
        dlApps.Start(Seconds(0.0));
        dlApps.Stop(simTime);
        dlSinks[u] = DynamicCast<PacketSink>(dlApps.Get(0));

        PacketSinkHelper ulSink(socketFactory,
                                InetSocketAddress(Ipv4Address::GetAny(), ulBasePort + u));
        auto ulApps = ulSink.Install(remoteHost);
        ulApps.Start(Seconds(0.0));
        ulApps.Stop(simTime);
        ulSinks[u] = DynamicCast<PacketSink>(ulApps.Get(0));
    }

    Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
    uv->SetAttribute("Min", DoubleValue(0.0));
    uv->SetAttribute("Max", DoubleValue(startJitterMs / 1000.0));

    for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
    {
        if (!isSelected[u] || !enableDl)
        {
            continue;
        }
        Time dlStart = roundStart + Seconds(uv->GetValue());
        stats[u].dlStart = dlStart;

        ApplicationContainer dlClientApp;
        if (useUdp)
        {
            UdpClientHelper dlClient(ueIpIfaces.GetAddress(u), dlBasePort + u);
            dlClient.SetAttribute("Interval", TimeValue(MilliSeconds(10)));
            dlClient.SetAttribute("MaxPackets", UintegerValue(maxPackets));
            dlClient.SetAttribute("PacketSize", UintegerValue(packetSize));
            dlClientApp = dlClient.Install(remoteHost);
        }
        else
        {
            BulkSendHelper dlClient(socketFactory,
                                    InetSocketAddress(ueIpIfaces.GetAddress(u), dlBasePort + u));
            dlClient.SetAttribute("MaxBytes", UintegerValue(modelBytes));
            dlClient.SetAttribute("SendSize", UintegerValue(appSendSizeBytes));
            dlClientApp = dlClient.Install(remoteHost);
        }
        dlClientApp.Start(dlStart);
        dlClientApp.Stop(timeout);
        if (eventLog)
        {
            NS_LOG_UNCOND("[round " << round << "] schedule DL client=" << u
                                    << " start_s=" << dlStart.GetSeconds()
                                    << " bytes=" << modelBytes);
        }
    }

    std::vector<bool> uplinkScheduled(ueNodes.GetN(), false);

    std::function<void(void)> pollFn;
    pollFn = [&]() {
        const Time now = Simulator::Now();

        for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
        {
            if (!isSelected[u])
            {
                continue;
            }
            if (!stats[u].dlDone)
            {
                uint64_t rx = dlSinks[u]->GetTotalRx();
                stats[u].dlBytes = rx;
                if (rx >= modelBytes)
                {
                    stats[u].dlDone = true;
                    stats[u].dlEnd = now;
                    if (eventLog)
                    {
                        NS_LOG_UNCOND("[round " << round << "] DL done client=" << u
                                                << " end_s=" << now.GetSeconds());
                    }

                    if (enableUl && !uplinkScheduled[u])
                    {
                        uplinkScheduled[u] = true;
                        Time ulStart = now + Seconds(computeS) + Seconds(uv->GetValue());
                        stats[u].ulStart = ulStart;

                        ApplicationContainer ulClientApp;
                        if (useUdp)
                        {
                            UdpClientHelper ulClient(remoteHostAddr, ulBasePort + u);
                            ulClient.SetAttribute("Interval", TimeValue(MilliSeconds(10)));
                            ulClient.SetAttribute("MaxPackets", UintegerValue(maxPackets));
                            ulClient.SetAttribute("PacketSize", UintegerValue(packetSize));
                            ulClientApp = ulClient.Install(ueNodes.Get(u));
                        }
                        else
                        {
                            BulkSendHelper ulClient(socketFactory,
                                                    InetSocketAddress(remoteHostAddr,
                                                                      ulBasePort + u));
                            ulClient.SetAttribute("MaxBytes", UintegerValue(modelBytes));
                            ulClient.SetAttribute("SendSize", UintegerValue(appSendSizeBytes));
                            ulClientApp = ulClient.Install(ueNodes.Get(u));
                        }
                        ulClientApp.Start(ulStart);
                        ulClientApp.Stop(timeout);
                        if (eventLog)
                        {
                            NS_LOG_UNCOND("[round " << round << "] schedule UL client=" << u
                                                    << " start_s=" << ulStart.GetSeconds()
                                                    << " bytes=" << modelBytes);
                        }
                    }
                }
            }
        }

        for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
        {
            if (!isSelected[u] || !enableUl)
            {
                continue;
            }
            if (!stats[u].ulDone)
            {
                uint64_t rx = ulSinks[u]->GetTotalRx();
                stats[u].ulBytes = rx;
                if (rx >= modelBytes)
                {
                    stats[u].ulDone = true;
                    stats[u].ulEnd = now;
                    if (eventLog)
                    {
                        NS_LOG_UNCOND("[round " << round << "] UL done client=" << u
                                                << " end_s=" << now.GetSeconds());
                    }
                }
            }
        }

        bool allDone = true;
        for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
        {
            if (!isSelected[u])
            {
                continue;
            }
            if (enableUl)
            {
                if (!stats[u].ulDone)
                {
                    allDone = false;
                    break;
                }
            }
            else if (!stats[u].dlDone)
            {
                allDone = false;
                break;
            }
        }

        if (allDone || now >= timeout)
        {
            Simulator::Stop(MilliSeconds(1));
            return;
        }

        Simulator::Schedule(pollPeriod, pollFn);
    };

    Simulator::Schedule(roundStart, pollFn);

    Ptr<FlowMonitor> flowMon;
    FlowMonitorHelper flowHelper;
    if (flowMonitor)
    {
        NodeContainer flowNodes;
        flowNodes.Add(remoteHost);
        flowNodes.Add(ueNodes);
        flowMon = flowHelper.Install(flowNodes);
    }

    std::unique_ptr<AnimationInterface> anim;
    std::string netAnimName;
    if (netAnim)
    {
        netAnimName = "netanim_" + std::to_string(round) + ".xml";
        anim = std::make_unique<AnimationInterface>(netAnimName);
    }

    Simulator::Stop(simTime);
    Simulator::Run();

    const Time simEnd = Simulator::Now();
    const bool timeoutHit = (simEnd >= timeout);

    uint32_t numSelected = 0;
    uint32_t numCompleted = 0;
    for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
    {
        stats[u].dlBytes = dlSinks[u]->GetTotalRx();
        stats[u].ulBytes = ulSinks[u]->GetTotalRx();
        if (isSelected[u])
        {
            numSelected++;
            if (enableUl)
            {
                if (stats[u].ulDone)
                {
                    numCompleted++;
                }
            }
            else if (stats[u].dlDone)
            {
                numCompleted++;
            }
        }
    }

    std::string flowXmlName;
    flsim::common::FlowAggregateStats flowAgg;
    if (flowMon)
    {
        flowMon->CheckForLostPackets();
        flowAgg = flsim::common::ComputeFlowAggregateStats(flowMon->GetFlowStats());

        flowXmlName = "flowmon_" + std::to_string(round) + ".xml";
        flowMon->SerializeToXmlFile(flowXmlName, true, true);
        std::cout << "Wrote FlowMonitor: " << flowXmlName << "\n";
    }

    const std::string reportCsv =
        flsim::common::BuildSummaryCsvName("lte", description, round);
    auto tiers = deviceTierByClient;
    flsim::common::WriteReportCsv(reportCsv,
                                  description,
                                  round,
                                  ueNodes.GetN(),
                                  numSelected,
                                  tiers,
                                  stats,
                                  modelBytes,
                                  flowAgg,
                                  roundStart,
                                  simEnd,
                                  numCompleted,
                                  timeoutHit);

    uint32_t numDlCompleted = 0;
    uint64_t totalDlBytes = 0;
    uint64_t totalUlBytes = 0;
    double maxDlDurS = 0.0;
    double maxUlDurS = 0.0;
    double maxComputeWaitS = 0.0;
    for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
    {
        if (!isSelected[u])
        {
            continue;
        }
        totalDlBytes += stats[u].dlBytes;
        totalUlBytes += stats[u].ulBytes;
        if (stats[u].dlDone)
        {
            numDlCompleted++;
            maxDlDurS = std::max(maxDlDurS, (stats[u].dlEnd - stats[u].dlStart).GetSeconds());
        }
        if (stats[u].ulDone)
        {
            maxUlDurS = std::max(maxUlDurS, (stats[u].ulEnd - stats[u].ulStart).GetSeconds());
        }
        if (stats[u].dlDone && stats[u].ulStart > stats[u].dlEnd)
        {
            maxComputeWaitS =
                std::max(maxComputeWaitS, (stats[u].ulStart - stats[u].dlEnd).GetSeconds());
        }
    }

    const double expectedBytes = static_cast<double>(modelBytes) * numSelected;
    const double dlCompletionRatio = expectedBytes > 0 ? totalDlBytes / expectedBytes : 0.0;
    const double ulCompletionRatio = expectedBytes > 0 ? totalUlBytes / expectedBytes : 0.0;
    const double aggDlGoodputMbps =
        flsim::common::AggregateDirectionalGoodputMbps(stats, true, simEnd);
    const double aggUlGoodputMbps =
        flsim::common::AggregateDirectionalGoodputMbps(stats, false, simEnd);

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

    // Serialize resolved runtime state for record storage and hashing.
    nlohmann::json expJson;
    expJson["network_type"] = "lte";
    expJson["description"] = description;
    expJson["reproducibility"] = {{"round", round}, {"seed", seed}};
    expJson["sim"] = {{"simulation_time", simTime.GetSeconds()}, {"poll_ms", pollMs}};
    expJson["fl_traffic"] = {{"model_size_mb", modelSizeMb},
                             {"compute_s", computeS},
                             {"sync_start_jitter_ms", startJitterMs}};
    expJson["lte"] = {{"scheduler", schedulerType},
                      {"rlc_mode", rlcMode},
                      {"rlc_buffer_bytes", rlcBufferBytes},
                      {"use_ideal_rrc", useIdealRrc},
                      {"use_ca", useCa},
                      {"num_component_carriers", numComponentCarriers},
                      {"enable_uplink_power_control", enableUplinkPowerControl},
                      {"attach_mode", useAutoAttach ? "auto" : "configured"}};
    expJson["network"]["server_side"] = {
        {"server_ap",
         {{"position",
           {{"x", serverApPosition.x}, {"y", serverApPosition.y}, {"z", serverApPosition.z}}}}},
        {"server",
         {{"position",
           {{"x", serverPosition.x}, {"y", serverPosition.y}, {"z", serverPosition.z}}}}},
        {"entry_link",
         {{"up_mbps", entryUpMbps},
          {"down_mbps", entryDownMbps},
          {"oneway_delay_ms", entryDelayMs},
          {"loss", entryLossRate}}},
        {"server_link",
         {{"up_mbps", serverUpMbps},
          {"down_mbps", serverDownMbps},
          {"oneway_delay_ms", serverDelayMs},
          {"loss", serverLossRate}}},
    };
    expJson["network"]["enbs"] = nlohmann::json::array();
    for (const auto& p : enbPositions)
    {
        expJson["network"]["enbs"].push_back(
            {{"position", {{"x", p.x}, {"y", p.y}, {"z", p.z}}}});
    }
    expJson["network"]["clients"] = nlohmann::json::array();
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        const uint32_t enbIndex = (i < ueEnbIndex.size()) ? ueEnbIndex[i] : 0;
        const std::string tierName =
            (i < deviceTierByClient.size()) ? deviceTierByClient[i] : "basic";
        const std::string mobilityPreset =
            (i < mobilityByClient.size()) ? mobilityByClient[i] : "static";
        const double radius = (i < ueRadius.size()) ? ueRadius[i] : 50.0;
        const bool selectedClient = (i < isSelected.size()) ? isSelected[i] : true;

        nlohmann::json client = {{"enb", enbIndex},
                                 {"device_tier", tierName},
                                 {"mobility_preset", mobilityPreset},
                                 {"radius_m", radius},
                                 {"selected", selectedClient}};

        if (i < ueHasExplicitPosition.size() && ueHasExplicitPosition[i] &&
            i < uePositions.size())
        {
            const auto& p = uePositions[i];
            client["position"] = {{"x", p.x}, {"y", p.y}, {"z", p.z}};
        }
        if (i < ueHasMobilityOverride.size() && ueHasMobilityOverride[i] &&
            i < ueMobility.size())
        {
            const auto& m = ueMobility[i];
            client["mobility"] = {{"model", m.model},
                                  {"speed_mps", m.speedMps},
                                  {"min_x", m.minX},
                                  {"max_x", m.maxX},
                                  {"min_y", m.minY},
                                  {"max_y", m.maxY}};
        }

        expJson["network"]["clients"].push_back(client);
    }

    std::string summary = "scheduler=" + schedulerType + "; attach=" +
                          (useAutoAttach ? std::string("auto") : std::string("configured")) +
                          "; enbs=" + std::to_string(numEnbs);

    nlohmann::json hashJson = LoadHashJsonFromConfig(configPath);
    if (!hashJson.is_object())
    {
        hashJson = expJson;
    }

    const flsim::common::RecordedFiles recorded =
        flsim::common::RecordOutputsWithHash(expJson,
                                             hashJson,
                                             round,
                                             seed,
                                             modelSizeMb,
                                             computeS,
                                             ueNodes.GetN(),
                                             numSelected,
                                             summary,
                                             reportCsv,
                                             flowXmlName,
                                             netAnimName);

    std::cout << "Wrote round report: " << reportCsv << "\n";
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
