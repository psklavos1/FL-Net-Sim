#pragma once

#include "json.hpp"

#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/mobility-module.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <limits>
#include <iostream>
#include <algorithm>
#include <vector>

#ifdef __unix__
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace flsim::common
{

// === Recording & Hashing =====================================================

struct RecordedFiles
{
    std::filesystem::path roundCsv;
    std::filesystem::path flowmonXml;
    std::filesystem::path netanimXml;
    std::filesystem::path roundDir;
};

inline double
GoodputMbps(uint64_t bytes, ns3::Time duration)
{
    const double sec = duration.GetSeconds();
    if (sec <= 0)
    {
        return -1.0;
    }
    return (bytes * 8.0) / (1e6 * sec);
}

template <typename StatsT>
inline double
AggregateDirectionalGoodputMbps(const std::vector<StatsT>& stats,
                                bool downlink,
                                ns3::Time fallbackEnd)
{
    uint64_t totalBytes = 0;
    double totalDirDurS = 0.0;

    for (const auto& s : stats)
    {
        if (!s.selected)
        {
            continue;
        }

        const uint64_t bytes = downlink ? s.dlBytes : s.ulBytes;
        totalBytes += bytes;

        const bool done = downlink ? s.dlDone : s.ulDone;
        const ns3::Time start = downlink ? s.dlStart : s.ulStart;
        const ns3::Time end = done ? (downlink ? s.dlEnd : s.ulEnd) : fallbackEnd;
        const double dirDurS = (start > ns3::Seconds(0) && end > start)
                                   ? (end - start).GetSeconds()
                                   : 0.0;
        totalDirDurS += dirDurS;
    }

    if (totalBytes == 0 || totalDirDurS <= 0.0)
    {
        return 0.0;
    }

    return (totalBytes * 8.0) / (1e6 * totalDirDurS);
}

inline uint64_t
MbToBytes(double mb)
{
    return static_cast<uint64_t>(mb * 1'000'000.0);
}

inline std::string
SanitizeSummaryToken(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    bool prevUnderscore = false;
    for (unsigned char ch : raw)
    {
        if (std::isalnum(ch))
        {
            out.push_back(static_cast<char>(std::tolower(ch)));
            prevUnderscore = false;
        }
        else if (!prevUnderscore)
        {
            out.push_back('_');
            prevUnderscore = true;
        }
    }
    while (!out.empty() && out.front() == '_')
    {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '_')
    {
        out.pop_back();
    }
    return out.empty() ? "run" : out;
}

inline std::string
BuildSummaryCsvName(const std::string& networkTypeRaw,
                    const std::string& descriptionRaw,
                    uint32_t round)
{
    const std::string networkType = SanitizeSummaryToken(networkTypeRaw);
    const std::string description = SanitizeSummaryToken(descriptionRaw);
    return networkType + "_" + description + "_round_" + std::to_string(round) + ".csv";
}

inline std::string
BuildSummaryCsvName(const nlohmann::json& cfg, uint32_t round)
{
    return BuildSummaryCsvName(cfg.value("network_type", "sim"),
                               cfg.value("description", "run"),
                               round);
}

inline std::string
HashJsonToHex(const nlohmann::json& v);

inline std::string
BuildStandaloneRecordDirName(const nlohmann::json& expJson,
                             const nlohmann::json& hashJson,
                             uint32_t round)
{
    const std::string networkType = SanitizeSummaryToken(expJson.value("network_type", "sim"));
    const std::string description = SanitizeSummaryToken(expJson.value("description", "run"));
    const std::string shortHash = HashJsonToHex(hashJson).substr(0, 8);

    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);

    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000000;

    std::ostringstream oss;
    oss << "standalone_" << networkType << "_" << description << "_" << buf << "_"
        << std::setw(6) << std::setfill('0') << micros << "_r" << round << "_" << shortHash;
    return oss.str();
}

// === Flow Monitoring =========================================================

struct FlowAggregateStats
{
    bool available = false;
    uint32_t numFlows = 0;
    uint64_t txPackets = 0;
    uint64_t rxPackets = 0;
    uint64_t lostPackets = 0;
    uint64_t txBytes = 0;
    uint64_t rxBytes = 0;
    double meanDelayMs = -1.0;
    double meanJitterMs = -1.0;
    double packetLossPct = -1.0;
    double aggRxGoodputMbps = -1.0;
};

inline FlowAggregateStats
ComputeFlowAggregateStats(const ns3::FlowMonitor::FlowStatsContainer& stats)
{
    FlowAggregateStats out;
    out.available = true;
    out.numFlows = stats.size();
    double delaySumS = 0.0;
    double jitterSumS = 0.0;
    for (const auto& kv : stats)
    {
        const auto& fs = kv.second;
        out.txPackets += fs.txPackets;
        out.rxPackets += fs.rxPackets;
        out.lostPackets += fs.lostPackets;
        out.txBytes += fs.txBytes;
        out.rxBytes += fs.rxBytes;
        delaySumS += fs.delaySum.GetSeconds();
        jitterSumS += fs.jitterSum.GetSeconds();
    }
    if (out.rxPackets > 0)
    {
        out.meanDelayMs = (delaySumS / out.rxPackets) * 1000.0;
        out.meanJitterMs = (jitterSumS / out.rxPackets) * 1000.0;
    }
    if (out.txPackets > 0)
    {
        out.packetLossPct =
            (100.0 * static_cast<double>(out.txPackets - out.rxPackets)) / out.txPackets;
    }
    double flowActiveDurS = 0.0;
    for (const auto& kv : stats)
    {
        const auto& fs = kv.second;
        if (fs.rxBytes == 0)
        {
            continue;
        }
        if (fs.timeLastRxPacket > fs.timeFirstTxPacket)
        {
            flowActiveDurS += (fs.timeLastRxPacket - fs.timeFirstTxPacket).GetSeconds();
        }
    }
    if (out.rxBytes > 0 && flowActiveDurS > 0.0)
    {
        out.aggRxGoodputMbps = (out.rxBytes * 8.0) / (1e6 * flowActiveDurS);
    }
    return out;
}

// === Console Summary =========================================================

inline void
PrintRoundSummary(ns3::Time roundStart,
                  ns3::Time roundEnd,
                  uint32_t numSelected,
                  uint32_t numDlCompleted,
                  uint32_t numCompleted,
                  double maxDlDurS,
                  double maxUlDurS,
                  double maxComputeWaitS,
                  uint64_t totalDlBytes,
                  uint64_t totalUlBytes,
                  double dlCompletionRatio,
                  double ulCompletionRatio,
                  double aggDlGoodputMbps,
                  double aggUlGoodputMbps,
                  bool timeoutHit,
                  const FlowAggregateStats& flowAgg)
{
    (void)maxComputeWaitS;
    std::cout << "\n=== ROUND SUMMARY ===\n";
    std::cout << "round_start_s: " << roundStart.GetSeconds() << "\n";
    std::cout << "round_end_s:   " << roundEnd.GetSeconds() << "\n";
    std::cout << "selected:      " << numSelected << "\n";
    std::cout << "dl_completed:  " << numDlCompleted << "\n";
    std::cout << "completed:     " << numCompleted << "\n";
    std::cout << "max_dl_s:      " << maxDlDurS << "\n";
    std::cout << "max_ul_s:      " << maxUlDurS << "\n";
    std::cout << "total_dl_MB:   " << (totalDlBytes / 1e6) << "\n";
    std::cout << "total_ul_MB:   " << (totalUlBytes / 1e6) << "\n";
    std::cout << "dl_ratio:      " << dlCompletionRatio << "\n";
    std::cout << "ul_ratio:      " << ulCompletionRatio << "\n";
    std::cout << "agg_dl_Mbps:   " << aggDlGoodputMbps << "\n";
    std::cout << "agg_ul_Mbps:   " << aggUlGoodputMbps << "\n";
    std::cout << "timeout_hit:   " << (timeoutHit ? "true" : "false") << "\n";
    if (flowAgg.available)
    {
        std::cout << "flow_flows:    " << flowAgg.numFlows << "\n";
        std::cout << "flow_loss_pct: " << flowAgg.packetLossPct << "\n";
        std::cout << "flow_delay_ms: " << flowAgg.meanDelayMs << "\n";
        std::cout << "flow_rx_Mbps:  " << flowAgg.aggRxGoodputMbps << "\n";
    }
}

inline uint64_t
Fnv1a64(const std::string& s)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : s)
    {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::string
ToHex(uint64_t value);

inline uint64_t
Fnv1a64Bytes(const uint8_t* data, size_t len, uint64_t h)
{
    for (size_t i = 0; i < len; ++i)
    {
        h ^= data[i];
        h = (h * 1099511628211ULL) & 0xFFFFFFFFFFFFFFFFULL;
    }
    return h;
}

inline uint64_t
HashString(uint64_t h, const std::string& s)
{
    return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size(), h);
}

inline uint64_t
HashJsonValue(const nlohmann::json& v, uint64_t h)
{
    if (v.is_null())
    {
        return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("N"), 1, h);
    }
    if (v.is_boolean())
    {
        const char b = v.get<bool>() ? 'T' : 'F';
        return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>(&b), 1, h);
    }
    if (v.is_number_integer())
    {
        h = Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("I"), 1, h);
        const int64_t val = v.get<int64_t>();
        return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>(&val), sizeof(val), h);
    }
    if (v.is_number_unsigned())
    {
        h = Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("I"), 1, h);
        const uint64_t uval = v.get<uint64_t>();
        if (uval > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            const std::string s = std::to_string(uval);
            return HashString(h, s);
        }
        const int64_t val = static_cast<int64_t>(uval);
        return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>(&val), sizeof(val), h);
    }
    if (v.is_number_float())
    {
        h = Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("D"), 1, h);
        const double val = v.get<double>();
        return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>(&val), sizeof(val), h);
    }
    if (v.is_string())
    {
        h = Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("S"), 1, h);
        return HashString(h, v.get<std::string>());
    }
    if (v.is_array())
    {
        h = Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("["), 1, h);
        for (const auto& item : v)
        {
            h = HashJsonValue(item, h);
        }
        return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("]"), 1, h);
    }
    if (v.is_object())
    {
        h = Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("{"), 1, h);
        std::vector<std::string> keys;
        keys.reserve(v.size());
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            keys.push_back(it.key());
        }
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys)
        {
            h = Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("K"), 1, h);
            h = HashString(h, key);
            h = HashJsonValue(v.at(key), h);
        }
        return Fnv1a64Bytes(reinterpret_cast<const uint8_t*>("}"), 1, h);
    }
    const std::string fallback = v.dump();
    return HashString(h, fallback);
}

inline std::string
HashJsonToHex(const nlohmann::json& v)
{
    const uint64_t h = HashJsonValue(v, 14695981039346656037ULL);
    return ToHex(h);
}

inline std::string
ToHex(uint64_t value)
{
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

inline std::string
EscapeCsvField(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos)
    {
        return value;
    }
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char c : value)
    {
        if (c == '"')
        {
            out.push_back('"');
            out.push_back('"');
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

inline int
AcquireIndexLock(const std::filesystem::path& lockPath)
{
#ifdef __unix__
    const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd == -1)
    {
        return -1;
    }
    if (::flock(fd, LOCK_EX) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
#else
    (void)lockPath;
    return -1;
#endif
}

inline void
ReleaseIndexLock(int fd)
{
#ifdef __unix__
    if (fd >= 0)
    {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
#else
    (void)fd;
#endif
}

inline void
MoveFileIfExists(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    if (!std::filesystem::exists(src))
    {
        return;
    }
    std::filesystem::create_directories(dst.parent_path());
    if (std::filesystem::exists(dst))
    {
        std::filesystem::remove(dst);
    }
    try
    {
        std::filesystem::rename(src, dst);
        return;
    }
    catch (const std::filesystem::filesystem_error&)
    {
    }

    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(src);
}

inline bool
ParseRoundFromFilename(const std::string& name,
                       const std::string& prefix,
                       const std::string& suffix,
                       uint32_t& roundOut)
{
    if (name.size() <= prefix.size() + suffix.size())
    {
        return false;
    }
    if (name.rfind(prefix, 0) != 0)
    {
        return false;
    }
    if (name.substr(name.size() - suffix.size()) != suffix)
    {
        return false;
    }
    const std::string number = name.substr(prefix.size(),
                                           name.size() - prefix.size() - suffix.size());
    if (number.empty())
    {
        return false;
    }
    for (char c : number)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    roundOut = static_cast<uint32_t>(std::stoul(number));
    return true;
}

inline std::vector<uint32_t>
CollectRounds(const std::filesystem::path& dir,
              const std::string& prefix,
              const std::string& suffix)
{
    namespace fs = std::filesystem;
    std::vector<uint32_t> rounds;
    if (!fs::exists(dir))
    {
        return rounds;
    }
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        uint32_t round = 0;
        if (ParseRoundFromFilename(name, prefix, suffix, round))
        {
            rounds.push_back(round);
        }
    }
    std::sort(rounds.begin(), rounds.end());
    rounds.erase(std::unique(rounds.begin(), rounds.end()), rounds.end());
    return rounds;
}

inline std::string
CompactRanges(const std::vector<uint32_t>& values)
{
    if (values.empty())
    {
        return "-";
    }
    std::ostringstream out;
    uint32_t start = values.front();
    uint32_t prev = start;
    for (size_t i = 1; i < values.size(); ++i)
    {
        const uint32_t v = values[i];
        if (v == prev + 1)
        {
            prev = v;
            continue;
        }
        if (out.tellp() > 0)
        {
            out << ",";
        }
        if (start == prev)
        {
            out << start;
        }
        else
        {
            out << start << "-" << prev;
        }
        start = prev = v;
    }
    if (out.tellp() > 0)
    {
        out << ",";
    }
    if (start == prev)
    {
        out << start;
    }
    else
    {
        out << start << "-" << prev;
    }
    return out.str();
}

inline void
UpdateRecordsIndex(const std::filesystem::path& baseDir)
{
    namespace fs = std::filesystem;
    fs::create_directories(baseDir);

    const fs::path lockPath = baseDir / "index.lock";
    const int lockFd = AcquireIndexLock(lockPath);

    nlohmann::json indexJson = nlohmann::json::array();
    std::vector<nlohmann::json> rows;

    for (const auto& expEntry : fs::directory_iterator(baseDir))
    {
        if (!expEntry.is_directory())
        {
            continue;
        }
        const std::string expName = expEntry.path().filename().string();
        if (expName.empty() || expName[0] == '.')
        {
            continue;
        }

        for (const auto& seedEntry : fs::directory_iterator(expEntry.path()))
        {
            if (!seedEntry.is_directory())
            {
                continue;
            }
            const std::string seedName = seedEntry.path().filename().string();
            if (seedName.rfind("seed_", 0) != 0)
            {
                continue;
            }
            uint64_t seed = 0;
            try
            {
                seed = static_cast<uint64_t>(std::stoull(seedName.substr(5)));
            }
            catch (const std::exception&)
            {
                seed = 0;
            }

            const fs::path seedDir = seedEntry.path();
            const std::vector<uint32_t> csvRounds =
                CollectRounds(seedDir / "csv", "round_", ".csv");
            const std::vector<uint32_t> flowRounds =
                CollectRounds(seedDir / "flowmon", "flowmon_", ".xml");
            const std::vector<uint32_t> vizRounds =
                CollectRounds(seedDir / "viz", "netanim_", ".xml");

            std::vector<uint32_t> allRounds = csvRounds;
            allRounds.insert(allRounds.end(), flowRounds.begin(), flowRounds.end());
            allRounds.insert(allRounds.end(), vizRounds.begin(), vizRounds.end());
            std::sort(allRounds.begin(), allRounds.end());
            allRounds.erase(std::unique(allRounds.begin(), allRounds.end()), allRounds.end());

            std::string networkType = "-";
            std::string description = "-";
            const fs::path cfgPath = seedDir / "config.json";
            if (fs::exists(cfgPath))
            {
                try
                {
                    std::ifstream in(cfgPath);
                    nlohmann::json cfg;
                    in >> cfg;
                    if (cfg.is_object())
                    {
                        networkType = cfg.value("network_type", "-");
                        description = cfg.value("description", "-");
                    }
                }
                catch (const std::exception&)
                {
                }
            }

            nlohmann::json entry = {
                {"exp_hash", expName},
                {"seed", seed},
                {"network_type", networkType},
                {"description", description},
                {"rounds", allRounds},
                {"rounds_compact", CompactRanges(allRounds)},
                {"csv", csvRounds.size()},
                {"flowmon", flowRounds.size()},
                {"viz", vizRounds.size()},
                {"exp_dir", expEntry.path().generic_string()},
            };
            indexJson.push_back(entry);
            rows.push_back(entry);
        }
    }

    const fs::path indexJsonPath = baseDir / "index.json";
    const fs::path indexCsvPath = baseDir / "index.csv";

    {
        const fs::path tmpPath = baseDir / "index.json.tmp";
        std::ofstream out(tmpPath, std::ios::out | std::ios::trunc);
        if (out)
        {
            out << indexJson.dump(2) << "\n";
        }
        std::error_code ec;
        fs::rename(tmpPath, indexJsonPath, ec);
        if (ec)
        {
            fs::copy_file(tmpPath, indexJsonPath, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmpPath, ec);
        }
    }

    {
        const fs::path tmpPath = baseDir / "index.csv.tmp";
        std::ofstream out(tmpPath, std::ios::out | std::ios::trunc);
        if (out)
        {
            out << "exp_hash,seed,network_type,description,rounds,csv,flowmon,viz,exp_dir\n";
            for (const auto& row : rows)
            {
                out << EscapeCsvField(row.value("exp_hash", "-")) << ","
                    << row.value("seed", 0) << ","
                    << EscapeCsvField(row.value("network_type", "-")) << ","
                    << EscapeCsvField(row.value("description", "-")) << ","
                    << EscapeCsvField(row.value("rounds_compact", "-")) << ","
                    << row.value("csv", 0) << ","
                    << row.value("flowmon", 0) << ","
                    << row.value("viz", 0) << ","
                    << EscapeCsvField(row.value("exp_dir", "-")) << "\n";
            }
        }
        std::error_code ec;
        fs::rename(tmpPath, indexCsvPath, ec);
        if (ec)
        {
            fs::copy_file(tmpPath, indexCsvPath, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmpPath, ec);
        }
    }

    ReleaseIndexLock(lockFd);
}

inline RecordedFiles
RecordOutputsWithHash(const nlohmann::json& expJson,
                      const nlohmann::json& hashJsonInput,
                      uint32_t round,
                      uint64_t seed,
                      double modelSizeMb,
                      double computeS,
                      uint32_t numClients,
                      uint32_t selectedClients,
                      const std::string& summary,
                      const std::string& roundCsv,
                      const std::string& flowmonXml,
                      const std::string& netanimXml)
{
    namespace fs = std::filesystem;

    nlohmann::json hashJson = hashJsonInput;
    if (hashJson.contains("description"))
    {
        hashJson.erase("description");
    }
    if (hashJson.contains("scenario"))
    {
        hashJson.erase("scenario");
    }
    if (hashJson.contains("reproducibility"))
    {
        auto& repro = hashJson["reproducibility"];
        repro.erase("seed");
        repro.erase("round");
        if (repro.empty())
        {
            hashJson.erase("reproducibility");
        }
    }

    std::string hash;
    if (hashJson.is_object() && hashJson.contains("_force_hash_dir") &&
        hashJson["_force_hash_dir"].is_string())
    {
        hash = hashJson["_force_hash_dir"].get<std::string>();
        std::cout << "hash_json=forced(" << hash << ")\n";
    }
    else
    {
        hash = BuildStandaloneRecordDirName(expJson, hashJson, round);
        std::cout << "hash_json=auto(" << hash << ")\n";
    }
    if (hash.empty())
    {
        hash = HashJsonToHex(hashJson);
    }
    std::cout << "experiment_hash=" << hash << "\n";
    const std::string seedTag = std::string("seed_") + std::to_string(seed);

    fs::path baseDir = fs::path("flsim_records");
    fs::path expDir = baseDir / hash / seedTag;
    fs::path roundDir = expDir;
    fs::create_directories(roundDir);

    {
        std::ofstream out(expDir / "config.json", std::ios::out);
        if (!out)
        {
            NS_FATAL_ERROR("Could not write experiment config snapshot");
        }
        out << expJson.dump(2) << "\n";
    }

    fs::path roundCsvPath(roundCsv);
    fs::path flowXmlPath(flowmonXml);

    fs::path csvDir = roundDir / "csv";
    fs::path flowDir = roundDir / "flowmon";
    fs::path movedRoundCsv = csvDir / ("round_" + std::to_string(round) + ".csv");
    MoveFileIfExists(roundCsvPath, movedRoundCsv);

    fs::path movedFlowXml;
    if (!flowmonXml.empty())
    {
        movedFlowXml = flowDir / ("flowmon_" + std::to_string(round) + ".xml");
        MoveFileIfExists(flowXmlPath, movedFlowXml);
    }

    fs::path movedNetAnim;
    if (!netanimXml.empty())
    {
        fs::path vizDir = roundDir / "viz";
        fs::path netAnimPath(netanimXml);
        movedNetAnim = vizDir / netAnimPath.filename();
        MoveFileIfExists(netAnimPath, movedNetAnim);
    }
    UpdateRecordsIndex(baseDir);

    return {movedRoundCsv, movedFlowXml, movedNetAnim, roundDir};
}

inline RecordedFiles
RecordOutputs(const nlohmann::json& expJson,
              uint32_t round,
              uint64_t seed,
              double modelSizeMb,
              double computeS,
              uint32_t numClients,
              uint32_t selectedClients,
              const std::string& summary,
              const std::string& roundCsv,
              const std::string& flowmonXml,
              const std::string& netanimXml)
{
    return RecordOutputsWithHash(expJson,
                                 expJson,
                                 round,
                                 seed,
                                 modelSizeMb,
                                 computeS,
                                 numClients,
                                 selectedClients,
                                 summary,
                                 roundCsv,
                                 flowmonXml,
                                 netanimXml);
}

// === CSV Reporting ===========================================================

template <typename StatsT, typename ExtraWriter>
inline void
WriteReportCsvWithExtra(const std::string& filename,
                        const std::string& description,
                        uint32_t round,
                        uint32_t numClients,
                        uint32_t numSelected,
                        const std::vector<std::string>& tiers,
                        const std::vector<StatsT>& stats,
                        uint64_t modelBytes,
                        const FlowAggregateStats& flowAgg,
                        ns3::Time roundStart,
                        ns3::Time roundEnd,
                        uint32_t numCompleted,
                        bool timeoutHit,
                        ExtraWriter extraWriter)
{
    std::ofstream out(filename.c_str(), std::ios::out);
    if (!out)
    {
        NS_FATAL_ERROR("Could not open report file: " << filename);
    }

    uint32_t dlDoneCount = 0;
    uint32_t ulDoneCount = 0;
    uint64_t totalDlBytes = 0;
    uint64_t totalUlBytes = 0;
    double maxDlDurS = 0.0;
    double maxUlDurS = 0.0;
    for (uint32_t idx = 0; idx < stats.size(); ++idx)
    {
        const auto& s = stats[idx];
        if (!s.selected)
        {
            continue;
        }
        totalDlBytes += s.dlBytes;
        totalUlBytes += s.ulBytes;
        if (s.dlDone)
        {
            dlDoneCount++;
            maxDlDurS = std::max(maxDlDurS, (s.dlEnd - s.dlStart).GetSeconds());
        }
        if (s.ulDone)
        {
            ulDoneCount++;
            maxUlDurS = std::max(maxUlDurS, (s.ulEnd - s.ulStart).GetSeconds());
        }
    }

    const double roundAggDlGoodputMbps =
        AggregateDirectionalGoodputMbps(stats, true, roundEnd);
    const double roundAggUlGoodputMbps =
        AggregateDirectionalGoodputMbps(stats, false, roundEnd);
    const double expectedBytes = static_cast<double>(modelBytes) * numSelected;
    const double dlCompletionRatio = expectedBytes > 0 ? totalDlBytes / expectedBytes : 0.0;
    const double ulCompletionRatio = expectedBytes > 0 ? totalUlBytes / expectedBytes : 0.0;

    out << "description,round,num_silos,num_selected,dl_done,ul_done,timeout_hit,round_start_s,round_"
           "end_s,max_dl_s,max_ul_s,total_dl_bytes,total_ul_bytes,dl_completion_"
           "ratio,ul_completion_ratio,agg_dl_goodput_mbps,agg_ul_goodput_mbps\n";
    out << description << "," << round << "," << numClients << "," << numSelected << ","
        << dlDoneCount << "," << numCompleted << "," << (timeoutHit ? 1 : 0) << ","
        << std::fixed << std::setprecision(6) << roundStart.GetSeconds() << ","
        << roundEnd.GetSeconds() << "," << maxDlDurS << "," << maxUlDurS << ","
        << totalDlBytes << "," << totalUlBytes << ","
        << dlCompletionRatio << "," << ulCompletionRatio << "," << roundAggDlGoodputMbps << ","
        << roundAggUlGoodputMbps << "\n\n";

    extraWriter(out);

    out << "flow_monitor_enabled,num_flows,tx_packets,rx_packets,lost_packets,tx_bytes,rx_bytes,"
           "mean_delay_ms,mean_jitter_ms,packet_loss_pct,agg_rx_goodput_mbps\n";
    out << (flowAgg.available ? 1 : 0) << "," << flowAgg.numFlows << "," << flowAgg.txPackets << ","
        << flowAgg.rxPackets << "," << flowAgg.lostPackets << "," << flowAgg.txBytes << ","
        << flowAgg.rxBytes << "," << flowAgg.meanDelayMs << "," << flowAgg.meanJitterMs << ","
        << flowAgg.packetLossPct << "," << flowAgg.aggRxGoodputMbps << "\n\n";

    out << "client,tier,selected,dl_start_s,dl_end_s,dl_dur_s,dl_throughput_mbps,dl_bytes,dl_bytes_"
           "missing,dl_completion_ratio,ul_start_s,ul_end_s,ul_dur_s,ul_throughput_"
           "mbps,ul_bytes,ul_bytes_missing,ul_completion_ratio\n";
    for (uint32_t i = 0; i < stats.size(); ++i)
    {
        const auto& s = stats[i];
        const std::string tier = (i < tiers.size()) ? tiers[i] : "";
        out << i << "," << tier << "," << (s.selected ? 1 : 0) << ",";

        const double dlDur = (s.dlDone ? (s.dlEnd - s.dlStart).GetSeconds() : -1.0);
        const double ulDur = (s.ulDone ? (s.ulEnd - s.ulStart).GetSeconds() : -1.0);
        const uint64_t dlMissing = (modelBytes > s.dlBytes) ? (modelBytes - s.dlBytes) : 0;
        const uint64_t ulMissing = (modelBytes > s.ulBytes) ? (modelBytes - s.ulBytes) : 0;
        const double dlRatio =
            (modelBytes > 0) ? std::min(1.0, static_cast<double>(s.dlBytes) / modelBytes) : 0.0;
        const double ulRatio =
            (modelBytes > 0) ? std::min(1.0, static_cast<double>(s.ulBytes) / modelBytes) : 0.0;
        const double dlTp = (dlDur > 0.0) ? ((s.dlBytes * 8.0) / (1e6 * dlDur)) : -1.0;
        const double ulTp = (ulDur > 0.0) ? ((s.ulBytes * 8.0) / (1e6 * ulDur)) : -1.0;

        out << s.dlStart.GetSeconds() << "," << s.dlEnd.GetSeconds() << "," << dlDur << ","
            << dlTp << "," << s.dlBytes << "," << dlMissing << "," << dlRatio << ","
            << s.ulStart.GetSeconds() << "," << s.ulEnd.GetSeconds() << ","
            << ulDur << "," << ulTp << "," << s.ulBytes << "," << ulMissing << "," << ulRatio
            << "\n";
    }
}

template <typename StatsT>
inline void
WriteReportCsv(const std::string& filename,
               const std::string& description,
               uint32_t round,
               uint32_t numClients,
               uint32_t numSelected,
               const std::vector<std::string>& tiers,
               const std::vector<StatsT>& stats,
               uint64_t modelBytes,
               const FlowAggregateStats& flowAgg,
               ns3::Time roundStart,
               ns3::Time roundEnd,
               uint32_t numCompleted,
               bool timeoutHit)
{
    WriteReportCsvWithExtra(filename,
                            description,
                            round,
                            numClients,
                            numSelected,
                            tiers,
                            stats,
                            modelBytes,
                            flowAgg,
                            roundStart,
                            roundEnd,
                            numCompleted,
                            timeoutHit,
                            [](std::ostream&) {});
}

} // namespace flsim::common

// === Mobility Helpers ========================================================

namespace flsim::common
{

inline void
SetConstantPosition(ns3::Ptr<ns3::Node> node, const ns3::Vector& pos)
{
    ns3::Ptr<ns3::ConstantPositionMobilityModel> model =
        ns3::CreateObject<ns3::ConstantPositionMobilityModel>();
    model->SetPosition(pos);
    node->AggregateObject(model);
}

template <typename MobilitySpecT>
inline void
InstallMobilityForNode(ns3::Ptr<ns3::Node> node,
                       const MobilitySpecT& mobility,
                       const ns3::Vector& initialPos,
                       double fallbackSpeed,
                       bool clampToBounds)
{
    ns3::MobilityHelper mobilityHelper;
    ns3::Ptr<ns3::ListPositionAllocator> posAlloc =
        ns3::CreateObject<ns3::ListPositionAllocator>();
    ns3::Vector start = initialPos;
    if (clampToBounds)
    {
        start.x = std::max(mobility.minX, std::min(mobility.maxX, start.x));
        start.y = std::max(mobility.minY, std::min(mobility.maxY, start.y));
    }
    posAlloc->Add(start);
    mobilityHelper.SetPositionAllocator(posAlloc);

    if (mobility.model == "constant_position")
    {
        mobilityHelper.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        mobilityHelper.Install(node);
        return;
    }

    if (mobility.model == "random_walk_2d")
    {
        const double speed = (mobility.speedMps > 0.0) ? mobility.speedMps : fallbackSpeed;
        std::ostringstream rv;
        rv << "ns3::ConstantRandomVariable[Constant=" << speed << "]";
        mobilityHelper.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                                        "Bounds",
                                        ns3::RectangleValue(ns3::Rectangle(mobility.minX,
                                                                          mobility.maxX,
                                                                          mobility.minY,
                                                                          mobility.maxY)),
                                        "Speed",
                                        ns3::StringValue(rv.str()));
        mobilityHelper.Install(node);
        return;
    }

    NS_FATAL_ERROR("Unsupported mobility.model='" << mobility.model
                                                   << "'. Expected constant_position or random_walk_2d");
}

} // namespace flsim::common
