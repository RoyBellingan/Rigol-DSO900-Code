#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scope_download
{

struct DownloadConfig
{
    std::string ip = "192.168.1.162";
    std::uint16_t port = 5555;
    std::vector<std::string> channels = {"CHAN1", "CHAN2", "CHAN3", "CHAN4"};
    std::string outPrefix;
    std::string outDirPrefix = "aq_";
    std::size_t chunkPoints = 250'000;
    std::size_t outputPoints = 10'000;
    double resetPauseSec = 0.5;
    bool saveRawCsv = true;
    bool saveAlignedCsv = true;
    bool xzDecimated = false;
    bool plots = true;
    bool screenshot = true;
    bool analysis = true;
    double targetFundamentalHz = 50.0;
    int maxHarmonic = 15;
};

struct Waveform
{
    std::string channel;
    std::size_t points = 0;
    double xinc = 0.0;
    double xorig = 0.0;
    double xref = 0.0;
    std::vector<double> values;
};

struct DownloadResult
{
    std::filesystem::path outDir;
    std::vector<Waveform> waveforms;
    Waveform refWaveform;
    int memoryDepth = 0;
    std::filesystem::path analysisLog;
};

void validateChannels(const std::vector<std::string>& channels);

[[nodiscard]] DownloadResult runDownload(const DownloadConfig& config);

[[nodiscard]] DownloadConfig parseArgs(int argc, char** argv);
void printUsage(const char* program);

} // namespace scope_download
