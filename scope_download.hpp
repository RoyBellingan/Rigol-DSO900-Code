#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scope_download
{

// Transfer width for :WAV:FORM. The DHO800/900 digitizer is 12-bit: BYTE truncates
// each sample to 8 bits and throws the low 4 bits away, WORD keeps all 12.
enum class WaveFormat
{
    Byte,
    Word
};

// Byte order of a WORD sample. Auto inspects the first chunk and picks the order
// that keeps every code inside the 12-bit range.
enum class WordOrder
{
    Auto,
    Little,
    Big
};

[[nodiscard]] const char* toString(WaveFormat f);
[[nodiscard]] const char* toString(WordOrder o);

struct DownloadConfig
{
    std::string ip = "192.168.1.162";
    std::uint16_t port = 5555;
    std::vector<std::string> channels = {"CHAN1", "CHAN2", "CHAN3", "CHAN4"};
    std::string outPrefix;
    std::string outDirPrefix = "aq_";
    WaveFormat waveFormat = WaveFormat::Word;
    WordOrder wordOrder = WordOrder::Auto;
    std::size_t chunkPoints = 250'000;
    std::size_t outputPoints = 10'000;
    double resetPauseSec = 0.5;
    // Filling the vertical window buys resolution everywhere, and losing a few
    // samples at the peak is cheap: measured on real captures, 0.1 % clipping costs
    // under 0.1 dB in every band. Only warn once clipping is heavy enough to matter.
    double clipTolerancePercent = 0.5;
    bool saveRawBin = true;
    bool saveRawCsv = false;
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
    // Signed int16 ADC codes with a 32768 bias so WORD 0..65535 fits: raw = code - 32768.
    // Volts: v = raw * scale + offset. Time: t = t0 + i * dt with t0 = xorig - xref * xinc.
    std::vector<std::int16_t> raw;
    double yorig = 0.0;
    double yref = 0.0;
    double scale = 0.0;            // volts per stored int16 count (equals yinc)
    double offset = 0.0;           // volts at raw = 0

    // Transfer detail, kept so the caller can report resolution and spot clipping.
    WaveFormat format = WaveFormat::Byte;
    WordOrder resolvedOrder = WordOrder::Little;
    int bits = 8;                  // container width: 8 for BYTE, 16 for WORD
    unsigned codeQuantum = 1;      // smallest observed step, in codes
    double effectiveLsbVolts = 0.0;// codeQuantum * yinc: the resolution actually delivered
    double effectiveBits = 8.0;    // log2(codeFull+1 / codeQuantum)
    double yinc = 0.0;             // volts per code, straight from the preamble
    unsigned codeMin = 0;
    unsigned codeMax = 0;
    unsigned codeFull = 255;       // highest code the format can carry
    bool clipped = false;          // a rail code appears in the record
    double clippedPercent = 0.0;   // share of samples sitting on a rail
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
