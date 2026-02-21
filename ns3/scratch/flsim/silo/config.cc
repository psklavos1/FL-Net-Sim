#include "config.h"

#include "../common/json.hpp"

#include "ns3/core-module.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flsim
{
using json = nlohmann::json;

static std::string
ReadTextFile(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::in);
    if (!in)
    {
        NS_FATAL_ERROR("Could not open JSON config file: " << path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

template <typename T>
static bool
GetIfPresent(const json& j, const std::string& key, T& out)
{
    if (!j.is_object())
    {
        return false;
    }
    auto it = j.find(key);
    if (it == j.end() || it->is_null())
    {
        return false;
    }
    out = it->get<T>();
    return true;
}

static bool
HasObject(const json& j, const std::string& key)
{
    return j.is_object() && j.contains(key) && j.at(key).is_object();
}

static NodePosition
ParseNodePosition(const json& node, const std::string& fieldName)
{
    if (!node.is_object())
    {
        NS_FATAL_ERROR(fieldName << " must be an object");
    }
    NodePosition pos;
    if (!node.contains("x") || !node.contains("y"))
    {
        NS_FATAL_ERROR(fieldName << " must include x and y");
    }
    pos.x = node.at("x").get<double>();
    pos.y = node.at("y").get<double>();
    if (node.contains("z"))
    {
        pos.z = node.at("z").get<double>();
    }
    return pos;
}

ScenarioConfig
DefaultScenarioConfig()
{
    ScenarioConfig cfg;
    cfg.presets = {
        {"very_weak", TierPreset{40.0, 120.0, 35.0, 0.0015}},
        {"weak", TierPreset{80.0, 250.0, 24.0, 0.0010}},
        {"basic", TierPreset{180.0, 600.0, 14.0, 0.0004}},
        {"strong", TierPreset{500.0, 1800.0, 7.0, 0.0001}},
        {"very_strong", TierPreset{1200.0, 5000.0, 3.0, 0.00003}},
    };

    cfg.clients.numClients = 5;
    cfg.clients.presetByClient = {"strong", "strong", "basic", "basic", "weak"};
    cfg.clients.selectedClients = {0, 1, 2, 3, 4};
    cfg.topology.clients.resize(cfg.clients.numClients);
    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        cfg.topology.clients[i].preset = cfg.clients.presetByClient[i];
        cfg.topology.clients[i].hasSelected = true;
        cfg.topology.clients[i].selected = true;
    }
    return cfg;
}

static void
ApplyJsonConfig(ScenarioConfig& cfg, const json& root)
{
    if (!root.is_object())
    {
        NS_FATAL_ERROR("Top-level JSON must be an object.");
    }


    GetIfPresent(root, "description", cfg.description);

    if (root.contains("round") || root.contains("seed"))
    {
        NS_FATAL_ERROR("round/seed must be set under reproducibility.{round,seed}");
    }
    if (HasObject(root, "reproducibility"))
    {
        const auto& r = root.at("reproducibility");
        GetIfPresent(r, "round", cfg.round);
        GetIfPresent(r, "seed", cfg.seed);
    }

    if (HasObject(root, "sim"))
    {
        const auto& sim = root.at("sim");
        GetIfPresent(sim, "simulation_time", cfg.sim.stopS);
        GetIfPresent(sim, "poll_ms", cfg.sim.pollMs);
    }

    if (HasObject(root, "network"))
    {
        const auto& topo = root.at("network");
        if (topo.contains("num_silos"))
        {
            NS_FATAL_ERROR("network.num_silos is not supported; use network.clients");
        }

        if (topo.contains("server_side") && !HasObject(topo, "server_side"))
        {
            NS_FATAL_ERROR("network.server_side must be an object");
        }

        if (HasObject(topo, "server_side"))
        {
            const auto& s = topo.at("server_side");
            if (HasObject(s, "server_link"))
            {
                const auto& c = s.at("server_link");
                GetIfPresent(c, "up_mbps", cfg.topology.serverSide.serverLink.upMbps);
                GetIfPresent(c, "down_mbps", cfg.topology.serverSide.serverLink.downMbps);
                GetIfPresent(c,
                             "oneway_delay_ms",
                             cfg.topology.serverSide.serverLink.oneWayDelayMs);
                GetIfPresent(c, "loss", cfg.topology.serverSide.serverLink.lossRate);
            }
            if (HasObject(s, "queue_disc"))
            {
                const auto& q = s.at("queue_disc");
                GetIfPresent(q, "type", cfg.topology.serverSide.queueDiscType);
            }

            if (HasObject(s, "server_ap"))
            {
                const auto& ap = s.at("server_ap");
                if (ap.contains("position"))
                {
                    cfg.topology.serverSide.serverApPosition =
                        ParseNodePosition(ap.at("position"),
                                          "network.server_side.server_ap.position");
                    cfg.topology.serverSide.hasServerApPosition = true;
                }
            }
            if (HasObject(s, "server"))
            {
                const auto& srv = s.at("server");
                if (srv.contains("position"))
                {
                    cfg.topology.serverSide.serverPosition =
                        ParseNodePosition(srv.at("position"),
                                          "network.server_side.server.position");
                    cfg.topology.serverSide.hasServerPosition = true;
                }
            }
        }

        if (topo.contains("clients"))
        {
            const auto& clients = topo.at("clients");
            if (!clients.is_array())
            {
                NS_FATAL_ERROR("network.clients must be an array");
            }
            cfg.topology.clients.clear();
            for (const auto& entry : clients)
            {
                if (!entry.is_object())
                {
                    NS_FATAL_ERROR("network.clients entries must be objects");
                }
                SiloClientConfig c;
                GetIfPresent(entry, "preset", c.preset);
                if (entry.contains("selected"))
                {
                    c.hasSelected = true;
                    GetIfPresent(entry, "selected", c.selected);
                }
                if (HasObject(entry, "position"))
                {
                    c.hasPosition = true;
                    c.position =
                        ParseNodePosition(entry.at("position"), "network.clients.position");
                }
                cfg.topology.clients.push_back(c);
            }
        }
    }

    if (HasObject(root, "presets"))
    {
        const auto& p = root.at("presets");
        for (auto it = p.begin(); it != p.end(); ++it)
        {
            const std::string tier = it.key();
            const json& obj = it.value();
            if (!obj.is_object())
            {
                NS_FATAL_ERROR("presets." << tier << " must be an object");
            }

            TierPreset tp = cfg.presets.count(tier) ? cfg.presets[tier] : TierPreset{};
            GetIfPresent(obj, "up_mbps", tp.upMbps);
            GetIfPresent(obj, "down_mbps", tp.downMbps);
            GetIfPresent(obj, "oneway_delay_ms", tp.oneWayDelayMs);
            GetIfPresent(obj, "loss", tp.lossRate);
            cfg.presets[tier] = tp;
        }
    }

    if (!cfg.topology.clients.empty())
    {
        const size_t numClients = cfg.topology.clients.size();
        cfg.clients.numClients = static_cast<uint32_t>(numClients);
        cfg.clients.presetByClient.resize(numClients, "basic");

        bool hasClientSelected = false;
        for (size_t i = 0; i < numClients; ++i)
        {
            auto& c = cfg.topology.clients[i];
            if (!c.preset.empty())
            {
                cfg.clients.presetByClient[i] = c.preset;
            }
            else
            {
                c.preset = cfg.clients.presetByClient[i];
            }

            if (c.hasSelected)
            {
                hasClientSelected = true;
            }
        }

        if (hasClientSelected)
        {
            cfg.clients.selectedClients.clear();
            for (size_t i = 0; i < numClients; ++i)
            {
                if (cfg.topology.clients[i].selected)
                {
                    cfg.clients.selectedClients.push_back(static_cast<uint32_t>(i));
                }
            }
        }
        else
        {
            cfg.clients.selectedClients.clear();
            for (size_t i = 0; i < numClients; ++i)
            {
                cfg.clients.selectedClients.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    if (HasObject(root, "fl_traffic"))
    {
        const auto& f = root.at("fl_traffic");
        GetIfPresent(f, "transport", cfg.fl.transport);
        GetIfPresent(f, "model_size_mb", cfg.fl.modelSizeMb);
        GetIfPresent(f, "sync_start_jitter_ms", cfg.fl.syncStartJitterMs);
        GetIfPresent(f, "compute_s", cfg.fl.computeS);

        if (f.contains("selected_clients"))
        {
            NS_FATAL_ERROR(
                "fl_traffic.selected_clients is not supported; use network.clients[].selected");
        }

        if (HasObject(f, "tcp"))
        {
            const auto& tcp = f.at("tcp");
            GetIfPresent(tcp, "socket_type", cfg.fl.tcp.socketType);
            GetIfPresent(tcp, "sack", cfg.fl.tcp.sack);
            GetIfPresent(tcp, "snd_buf_bytes", cfg.fl.tcp.sndBufBytes);
            GetIfPresent(tcp, "rcv_buf_bytes", cfg.fl.tcp.rcvBufBytes);
            GetIfPresent(tcp, "segment_size_bytes", cfg.fl.tcp.segmentSizeBytes);
            GetIfPresent(tcp, "app_send_size_bytes", cfg.fl.tcp.appSendSizeBytes);
        }
    }

    if (HasObject(root, "metrics"))
    {
        const auto& m = root.at("metrics");
        GetIfPresent(m, "flow_monitor", cfg.metrics.flowMonitor);
        GetIfPresent(m, "event_log", cfg.metrics.eventLog);
        GetIfPresent(m, "netanim", cfg.metrics.netAnim);
    }
}

static void
ApplyCliOverrides(ScenarioConfig& cfg, const CliOverrides& o)
{
    if (o.round != std::numeric_limits<uint32_t>::max())
    {
        cfg.round = o.round;
    }
    if (o.seed != std::numeric_limits<uint64_t>::max())
    {
        cfg.seed = o.seed;
    }

    if (o.numSilos != std::numeric_limits<uint32_t>::max())
    {
        cfg.clients.numClients = o.numSilos;
        cfg.topology.clients.resize(o.numSilos);
        cfg.clients.presetByClient.resize(o.numSilos, "basic");
        cfg.clients.selectedClients.clear();
        for (uint32_t i = 0; i < o.numSilos; ++i)
        {
            cfg.topology.clients[i].preset = cfg.clients.presetByClient[i];
            cfg.topology.clients[i].hasSelected = true;
            cfg.topology.clients[i].selected = true;
            cfg.clients.selectedClients.push_back(i);
        }
    }

    if (!std::isnan(o.stopS))
    {
        cfg.sim.stopS = o.stopS;
    }

    if (!std::isnan(o.modelSizeMb))
    {
        cfg.fl.modelSizeMb = o.modelSizeMb;
    }
}

static void
ValidateConfig(ScenarioConfig& cfg)
{
    if (cfg.seed == 0)
    {
        NS_FATAL_ERROR("seed must be > 0");
    }
    if (cfg.round == 0)
    {
        NS_FATAL_ERROR("round must be > 0");
    }

    if (cfg.clients.numClients == 0)
    {
        NS_FATAL_ERROR("number of clients must be > 0 (set network.clients)");
    }

    if (cfg.sim.stopS <= 0)
    {
        NS_FATAL_ERROR("sim.simulation_time must be > 0");
    }
    if (cfg.sim.pollMs <= 0)
    {
        NS_FATAL_ERROR("sim.poll_ms must be > 0");
    }

    if (cfg.fl.modelSizeMb <= 0)
    {
        NS_FATAL_ERROR("fl_traffic.model_size_mb must be > 0");
    }

    if (cfg.fl.computeS < 0)
    {
        NS_FATAL_ERROR("fl_traffic.compute_s must be >= 0");
    }

    if (cfg.fl.transport != "tcp")
    {
        NS_FATAL_ERROR("Only fl_traffic.transport='tcp' is currently supported");
    }

    if (cfg.fl.tcp.sndBufBytes < 64 * 1024 || cfg.fl.tcp.rcvBufBytes < 64 * 1024)
    {
        NS_FATAL_ERROR("fl_traffic.tcp snd/rcv buffers must be >= 65536 bytes");
    }

    if (cfg.fl.tcp.segmentSizeBytes < 536 || cfg.fl.tcp.segmentSizeBytes > 8960)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.segment_size_bytes must be in [536, 8960]");
    }

    if (cfg.fl.tcp.appSendSizeBytes == 0)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.app_send_size_bytes must be > 0");
    }

    if (cfg.fl.tcp.appSendSizeBytes > cfg.fl.tcp.segmentSizeBytes)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.app_send_size_bytes must be <= segment_size_bytes");
    }

    for (const auto& kv : cfg.presets)
    {
        const auto& t = kv.second;
        if (t.upMbps <= 0 || t.downMbps <= 0)
        {
            NS_FATAL_ERROR("preset '" << kv.first << "' up/down must be > 0");
        }
        if (t.oneWayDelayMs < 0)
        {
            NS_FATAL_ERROR("preset '" << kv.first << "' oneway_delay_ms must be >= 0");
        }
        if (t.lossRate < 0 || t.lossRate > 1)
        {
            NS_FATAL_ERROR("preset '" << kv.first << "' loss must be in [0,1]");
        }
    }

    if (cfg.topology.serverSide.serverLink.upMbps <= 0 ||
        cfg.topology.serverSide.serverLink.downMbps <= 0)
    {
        NS_FATAL_ERROR("network.server_side.server_link up/down must be > 0");
    }
    if (cfg.topology.serverSide.serverLink.oneWayDelayMs < 0)
    {
        NS_FATAL_ERROR("network.server_side.server_link.oneway_delay_ms must be >= 0");
    }
    if (cfg.topology.serverSide.serverLink.lossRate < 0 ||
        cfg.topology.serverSide.serverLink.lossRate > 1)
    {
        NS_FATAL_ERROR("network.server_side.server_link.loss must be in [0,1]");
    }

    if (cfg.clients.presetByClient.size() != cfg.clients.numClients)
    {
        NS_FATAL_ERROR("preset list size ("
                       << cfg.clients.presetByClient.size()
                       << ") must match network.clients size (" << cfg.clients.numClients << ")");
    }

    if (!cfg.topology.clients.empty() &&
        cfg.topology.clients.size() != cfg.clients.numClients)
    {
        NS_FATAL_ERROR("network.clients size (" << cfg.topology.clients.size()
                                                << ") must match number of clients ("
                                                << cfg.clients.numClients << ")");
    }

    for (const auto& tier : cfg.clients.presetByClient)
    {
        if (!cfg.presets.count(tier))
        {
            NS_FATAL_ERROR("preset '" << tier << "' is missing from presets");
        }
    }

    if (cfg.clients.selectedClients.empty())
    {
        NS_FATAL_ERROR("at least one client must be selected");
    }

    std::set<uint32_t> uniq(cfg.clients.selectedClients.begin(), cfg.clients.selectedClients.end());
    if (uniq.size() != cfg.clients.selectedClients.size())
    {
        NS_FATAL_ERROR("selected clients contains duplicates");
    }

    for (auto idx : cfg.clients.selectedClients)
    {
        if (idx >= cfg.clients.numClients)
        {
            NS_FATAL_ERROR("selected client index " << idx
                                                    << " out of range for network.clients size="
                                                    << cfg.clients.numClients);
        }
    }

    // Keep topology and clients in sync.
    cfg.topology.numSilos = cfg.clients.numClients;
}

void
AddCommandLineArgs(ns3::CommandLine& cmd, CliOverrides& o)
{
    cmd.AddValue("config", "Path to JSON config file", o.configPath);
    cmd.AddValue("round", "Round id (also used as RNG run id)", o.round);
    cmd.AddValue("seed", "Experiment-level RNG seed", o.seed);
    cmd.AddValue("numClients", "Override number of network clients", o.numSilos);
    cmd.AddValue("numSilos", "Override number of network clients", o.numSilos);
    cmd.AddValue("simTime", "Override sim.simulation_time", o.stopS);
    cmd.AddValue("stopS", "Override sim.simulation_time", o.stopS);
    cmd.AddValue("modelSizeMb", "Override fl_traffic.model_size_mb", o.modelSizeMb);
}

ScenarioConfig
ResolveConfig(const CliOverrides& in)
{
    ScenarioConfig cfg = DefaultScenarioConfig();

    // JSON
    if (!in.configPath.empty())
    {
        const std::string text = ReadTextFile(in.configPath);
        json root;
        try
        {
            root = json::parse(text);
        }
        catch (const std::exception& e)
        {
            NS_FATAL_ERROR("JSON parse error: " << e.what());
        }
        ApplyJsonConfig(cfg, root);
    }

    // CLI
    ApplyCliOverrides(cfg, in);

    ValidateConfig(cfg);
    return cfg;
}

void
PrintResolvedConfig(const ScenarioConfig& c)
{
    std::cout << "\n==== Resolved Configuration ====\n";
    std::cout << "description: " << c.description << "\n";
    std::cout << "round: " << c.round << "\n";
    std::cout << "seed: " << c.seed << "\n";
    std::cout << "sim.simulation_time: " << c.sim.stopS << "\n";
    std::cout << "sim.poll_ms: " << c.sim.pollMs << "\n";

    std::cout << "\n[network]\n";
    std::cout << "num_silos: " << c.topology.numSilos << "\n";
    std::cout << "server_side.server_link: up=" << c.topology.serverSide.serverLink.upMbps
              << " down=" << c.topology.serverSide.serverLink.downMbps
              << " delay_ms=" << c.topology.serverSide.serverLink.oneWayDelayMs
              << " loss=" << c.topology.serverSide.serverLink.lossRate << "\n";
    std::cout << "server_side.queue_disc.type: " << c.topology.serverSide.queueDiscType << "\n";
    std::cout << "clients.positions: "
              << (c.topology.clients.empty() ? "auto" : "mixed/explicit") << "\n";

    std::cout << "\n[network.clients]\n";
    std::cout << "num_clients: " << c.clients.numClients << "\n";
    std::cout << "selected:";
    for (auto idx : c.clients.selectedClients)
    {
        std::cout << " " << idx;
    }
    std::cout << "\n";
    std::cout << "preset:";
    for (const auto& presetName : c.clients.presetByClient)
    {
        std::cout << " " << presetName;
    }
    std::cout << "\n";

    std::cout << "\n[presets]\n";
    for (const auto& kv : c.presets)
    {
        const auto& t = kv.second;
        std::cout << kv.first << ": {up_mbps=" << t.upMbps << ", down_mbps=" << t.downMbps
                  << ", oneway_delay_ms=" << t.oneWayDelayMs << ", loss=" << t.lossRate << "}\n";
    }

    std::cout << "\n[fl_traffic]\n";
    std::cout << "transport: " << c.fl.transport << "\n";
    std::cout << "model_mb: " << c.fl.modelSizeMb << "\n";
    std::cout << "sync_start_jitter_ms: " << c.fl.syncStartJitterMs << "\n";
    std::cout << "compute_s: " << c.fl.computeS << "\n";

    std::cout << "\n[fl_traffic.tcp]\n";
    std::cout << "socket_type: " << c.fl.tcp.socketType << "\n";
    std::cout << "sack: " << (c.fl.tcp.sack ? "true" : "false") << "\n";
    std::cout << "snd_buf_bytes: " << c.fl.tcp.sndBufBytes << "\n";
    std::cout << "rcv_buf_bytes: " << c.fl.tcp.rcvBufBytes << "\n";
    std::cout << "segment_size_bytes: " << c.fl.tcp.segmentSizeBytes << "\n";
    std::cout << "app_send_size_bytes: " << c.fl.tcp.appSendSizeBytes << "\n";

    std::cout << "\n[metrics]\n";
    std::cout << "flow_monitor: " << (c.metrics.flowMonitor ? "true" : "false") << "\n";
    std::cout << "event_log: " << (c.metrics.eventLog ? "true" : "false") << "\n";
    std::cout << "netanim: " << (c.metrics.netAnim ? "true" : "false") << "\n";

    std::cout << "================================\n\n";
}

} // namespace flsim
