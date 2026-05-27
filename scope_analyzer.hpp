#pragma once

#include <filesystem>
#include <iosfwd>
#include <limits>
#include <string>
#include <vector>

namespace scope_analyzer
{

constexpr double PI = 3.1415926535897932384626433832795;

struct CsvData
{
    std::vector<std::string> headers;
    std::vector<std::vector<double>> columns;
};

struct HarmonicInfo
{
    int harmonic = 0;
    double freqHz = 0.0;
    int fftBin = -1;
    double amplitude = 0.0;
    double phaseRad = 0.0;
    double phaseDeg = 0.0;
    double relToFundamental = 0.0;
};

struct ChannelStats
{
    std::string name;

    double mean = 0.0;
    double rms = 0.0;
    double stddev = 0.0;
    double minv = 0.0;
    double maxv = 0.0;
    double p2p = 0.0;
    double crestFactor = 0.0;

    double fundamentalHz = 0.0;
    int fundamentalBin = -1;
    double fundamentalAmplitude = 0.0;
    double fundamentalPhaseRad = 0.0;
    double fundamentalPhaseDeg = 0.0;

    double dominantFreqHz = 0.0;
    int dominantBin = -1;
    double dominantAmplitude = 0.0;

    double thd = 0.0;
    double totalSpectralEnergy = 0.0;
    double harmonicEnergy = 0.0;
    double fundamentalEnergyRatio = 0.0;

    double sineResidualRMS = 0.0;
    double sineResidualToSignalRMS = 0.0;
    double sineResidualToSignalStd = 0.0;

    double zeroCrossingTime = std::numeric_limits<double>::quiet_NaN();

    std::vector<HarmonicInfo> harmonics;
    std::vector<double> detrended;
    std::vector<double> sineResidual;
};

struct PairStats
{
    std::string a;
    std::string b;

    double pearsonCorrelation = 0.0;
    double normalizedDot = 0.0;
    double amplitudeRatio = 0.0;

    double phaseDiffRad = 0.0;
    double phaseDiffDeg = 0.0;
    double timeShiftSec = 0.0;
    double timeShiftUs = 0.0;

    int bestLagSamples = 0;
    double bestLagSec = 0.0;
    double bestLagUs = 0.0;
    double bestLagCorrelation = 0.0;

    double alignedCorrelation = 0.0;
    double harmonicSimilarity = 0.0;
    double residualCorrelation = 0.0;
};

struct AnalysisOptions
{
    double targetFundamentalHz = 50.0;
    int maxHarmonic = 15;
};

struct AnalysisResult
{
    CsvData csv;
    std::size_t timeColumnIndex = 0;
    double dt = 0.0;
    double fs = 0.0;
    double duration = 0.0;
    std::size_t rowCount = 0;
    std::vector<ChannelStats> channels;
    std::vector<PairStats> pairs;
};

struct ReportOptions
{
    std::string inputFile;
    std::string outputLog;
    double targetFundamentalHz = 50.0;
    int maxHarmonic = 15;
};

struct RunPaths
{
    std::string inputCsv;
    std::string outputLog;
    double targetFundamentalHz = 50.0;
    int maxHarmonic = 15;
};

[[nodiscard]] CsvData readCsv(const std::string& path);
[[nodiscard]] AnalysisResult analyzeCsv(const CsvData& csv, const AnalysisOptions& options);
[[nodiscard]] AnalysisResult analyzeCsvFile(const std::string& path, const AnalysisOptions& options);

[[nodiscard]] ChannelStats analyzeChannel(
    const std::string& name,
    const std::vector<double>& time,
    const std::vector<double>& raw,
    double fs,
    double targetFundamentalHz,
    int maxHarmonic);

[[nodiscard]] PairStats analyzePair(
    const ChannelStats& a,
    const ChannelStats& b,
    double fs,
    double fundamentalHz);

[[nodiscard]] double estimateDt(const std::vector<double>& time);
[[nodiscard]] std::size_t findTimeColumnIndex(const std::vector<std::string>& headers);
[[nodiscard]] bool isTimeColumnHeader(const std::string& header);
[[nodiscard]] bool isAuxiliarySkipHeader(const std::string& header);

void writeChannelReport(std::ostream& os, const ChannelStats& s);
void writePairReport(std::ostream& os, const PairStats& p);
void writeAnalysisReport(std::ostream& os, const AnalysisResult& result, const ReportOptions& report);

[[nodiscard]] RunPaths parseRunPaths(int argc, char** argv);
void printUsage(const char* program);

[[nodiscard]] std::string stripFileUrl(std::string path);
[[nodiscard]] std::string resolveDecimatedCsv(const std::filesystem::path& dir);

} // namespace scope_analyzer
