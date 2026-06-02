#include "scope_download.hpp"
#include "scope_analyzer.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace scope_download
{
namespace
{

constexpr int kTimeoutSec = 180;
constexpr int kRecvBufSize = 1024 * 1024;

[[nodiscard]] std::string trim(std::string_view sv)
{
    std::size_t a = 0;
    while (a < sv.size() && std::isspace(static_cast<unsigned char>(sv[a])))
        ++a;
    std::size_t b = sv.size();
    while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1])))
        --b;
    return std::string(sv.substr(a, b - a));
}

[[nodiscard]] std::string asciiUpper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

class TcpScpiSession
{
public:
    explicit TcpScpiSession(std::string host, std::uint16_t port)
        : host_(std::move(host))
        , port_(port)
    {
    }

    ~TcpScpiSession() { close(); }

    TcpScpiSession(const TcpScpiSession&) = delete;
    TcpScpiSession& operator=(const TcpScpiSession&) = delete;

    void connect()
    {
        close();

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* res = nullptr;
        const std::string portStr = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0)
            throw std::runtime_error("getaddrinfo failed for " + host_);

        int fd = -1;
        for (addrinfo* p = res; p != nullptr; p = p->ai_next)
        {
            fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0)
                continue;
            if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0)
                break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);

        if (fd < 0)
            throw std::runtime_error("Cannot connect to " + host_ + ":" + portStr);

        const timeval tv{kTimeoutSec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &kRecvBufSize, sizeof(kRecvBufSize));

        fd_ = fd;
    }

    void close()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void writeCmd(std::string_view cmd)
    {
        std::string line(cmd);
        if (line.empty() || line.back() != '\n')
            line.push_back('\n');
        sendAll(line.data(), line.size());
    }

    [[nodiscard]] std::string query(std::string_view cmd)
    {
        writeCmd(cmd);
        return readLine();
    }

    [[nodiscard]] std::vector<std::uint8_t> queryBinaryBlock(std::string_view cmd)
    {
        writeCmd(cmd);
        return readBinaryBlock();
    }

    void drainErrors(const std::string& context = "", bool quiet = false)
    {
        for (;;)
        {
            const std::string err = query(":SYST:ERR?");
            if (err.starts_with("0,") || err.starts_with("0 ") || err == "0")
                break;
            if (!quiet)
            {
                if (context.empty())
                    std::cerr << "SCPI ERROR: " << err << "\n";
                else
                    std::cerr << "SCPI ERROR [" << context << "]: " << err << "\n";
            }
        }
    }

private:
    void sendAll(const void* data, std::size_t len)
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        std::size_t sent = 0;
        while (sent < len)
        {
            const ssize_t n = ::send(fd_, p + sent, len - sent, 0);
            if (n <= 0)
                throw std::runtime_error("SCPI send failed");
            sent += static_cast<std::size_t>(n);
        }
    }

    void recvSome(std::vector<std::uint8_t>& buf, std::size_t need)
    {
        while (buf.size() < need)
        {
            std::array<std::uint8_t, 65536> chunk{};
            const ssize_t n = ::recv(fd_, chunk.data(), chunk.size(), 0);
            if (n <= 0)
                throw std::runtime_error("SCPI recv failed or timed out");
            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
        }
    }

    [[nodiscard]] std::string readLine()
    {
        std::string line;
        for (;;)
        {
            if (readBuf_.empty())
            {
                std::array<std::uint8_t, 65536> chunk{};
                const ssize_t n = ::recv(fd_, chunk.data(), chunk.size(), 0);
                if (n <= 0)
                    throw std::runtime_error("SCPI recv failed or timed out");
                readBuf_.assign(chunk.begin(), chunk.begin() + n);
            }

            const auto it = std::find(readBuf_.begin(), readBuf_.end(), '\n');
            if (it != readBuf_.end())
            {
                line.append(readBuf_.begin(), it);
                readBuf_.erase(readBuf_.begin(), it + 1);
                break;
            }
            line.append(readBuf_.begin(), readBuf_.end());
            readBuf_.clear();
        }

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        return line;
    }

    [[nodiscard]] std::vector<std::uint8_t> readBinaryBlock()
    {
        std::vector<std::uint8_t> buf;
        recvSome(buf, 2);

        if (buf[0] != '#')
            throw std::runtime_error("Expected IEEE binary block header '#'");

        const int ndigits = buf[1] - '0';
        if (ndigits < 1 || ndigits > 9)
            throw std::runtime_error("Invalid binary block digit count");

        recvSome(buf, 2 + static_cast<std::size_t>(ndigits));
        const std::string lenStr(buf.begin() + 2, buf.begin() + 2 + ndigits);
        std::size_t payloadLen = 0;
        try
        {
            payloadLen = static_cast<std::size_t>(std::stoull(lenStr));
        }
        catch (...)
        {
            throw std::runtime_error("Invalid binary block length: " + lenStr);
        }

        recvSome(buf, 2 + static_cast<std::size_t>(ndigits) + payloadLen);
        std::vector<std::uint8_t> payload(
            buf.begin() + 2 + ndigits,
            buf.begin() + 2 + ndigits + static_cast<std::ptrdiff_t>(payloadLen));

        readBuf_.assign(
            buf.begin() + 2 + ndigits + static_cast<std::ptrdiff_t>(payloadLen),
            buf.end());

        if (!readBuf_.empty() && readBuf_[0] == '\n')
            readBuf_.erase(readBuf_.begin());

        return payload;
    }

    std::string host_;
    std::uint16_t port_;
    int fd_ = -1;
    std::vector<std::uint8_t> readBuf_;
};

[[nodiscard]] int acquireMemoryDepth(TcpScpiSession& scope)
{
    std::string raw = trim(scope.query(":ACQ:MDEP?"));
    raw = asciiUpper(raw);
    if (raw != "AUTO" && !raw.empty())
    {
        try
        {
            return static_cast<int>(std::stod(raw));
        }
        catch (...)
        {
        }
    }

    raw = trim(scope.query(":ACQuire:MDEPth?"));
    raw = asciiUpper(raw);
    if (raw == "AUTO" || raw.empty())
        return 0;
    return static_cast<int>(std::stod(raw));
}

void resetWavSubsystem(TcpScpiSession& scope, const std::string& channel, double resetPauseSec)
{
    scope.writeCmd(":WAV:MODE NORMal");
    scope.drainErrors("WAV:MODE NORMal (reset)", true);
    scope.writeCmd(":WAV:STAR 1");
    scope.writeCmd(":WAV:STOP 1000");
    scope.drainErrors("WAV:STAR/STOP reset", true);

    scope.writeCmd(":WAV:SOUR " + channel);
    scope.drainErrors("WAV:SOUR " + channel);

    scope.writeCmd(":WAV:MODE RAW");
    scope.drainErrors("WAV:MODE RAW");
    scope.writeCmd(":WAV:FORM BYTE");
    scope.drainErrors("WAV:FORM BYTE");

    std::this_thread::sleep_for(
        std::chrono::duration<double>(resetPauseSec));
    scope.drainErrors("post-reset " + channel, true);
}

[[nodiscard]] Waveform readChannelRaw(
    TcpScpiSession& scope,
    const std::string& channel,
    int memoryDepth,
    std::size_t chunkPoints,
    double resetPauseSec)
{
    resetWavSubsystem(scope, channel, resetPauseSec);

    scope.writeCmd(":WAV:POIN " + std::to_string(memoryDepth));
    scope.drainErrors("WAV:POIN " + std::to_string(memoryDepth) + " " + channel, true);
    const std::string accepted = trim(scope.query(":WAV:POIN?"));
    std::cout << channel << ": :WAV:POIN " << memoryDepth << " -> accepted " << accepted << "\n";

    const std::string preamble = trim(scope.query(":WAV:PRE?"));
    scope.drainErrors("WAV:PRE? " + channel);

    std::vector<std::string> parts;
    {
        std::stringstream ss(preamble);
        std::string item;
        while (std::getline(ss, item, ','))
            parts.push_back(trim(item));
    }
    if (parts.size() < 10)
        throw std::runtime_error("Unexpected preamble for " + channel + ": " + preamble);

    const int points = static_cast<int>(std::stod(parts[2]));
    std::cout << channel << ": preamble reports " << points << " RAW points "
              << "(memory depth setting: " << memoryDepth << ")\n";

    const double xinc = std::stod(parts[4]);
    const double xorig = std::stod(parts[5]);
    const double xref = std::stod(parts[6]);
    const double yinc = std::stod(parts[7]);
    const double yorig = std::stod(parts[8]);
    const double yref = std::stod(parts[9]);

    Waveform wf;
    wf.channel = channel;
    wf.xinc = xinc;
    wf.xorig = xorig;
    wf.xref = xref;
    wf.values.reserve(static_cast<std::size_t>(points));

    const double yScale = yinc;
    const double yOff = yref + yorig;

    int start = 1;
    while (start <= points)
    {
        const int stop = std::min(start + static_cast<int>(chunkPoints) - 1, points);
        scope.writeCmd(":WAV:STAR " + std::to_string(start));
        scope.writeCmd(":WAV:STOP " + std::to_string(stop));
        scope.drainErrors("WAV:STAR/STOP " + std::to_string(start) + ".." + std::to_string(stop));

        const auto raw = scope.queryBinaryBlock(":WAV:DATA?");
        scope.drainErrors("WAV:DATA? " + channel + " " + std::to_string(start) + ".." + std::to_string(stop));

        const std::size_t expected = static_cast<std::size_t>(stop - start + 1);
        if (raw.size() != expected)
        {
            throw std::runtime_error(
                channel + ": expected " + std::to_string(expected) + " samples for "
                + std::to_string(start) + ".." + std::to_string(stop) + ", got "
                + std::to_string(raw.size()));
        }

        const std::size_t base = wf.values.size();
        wf.values.resize(base + raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
            wf.values[base + i] = (static_cast<double>(raw[i]) - yOff) * yScale;

        std::cout << "  " << channel << ": read " << start << ".." << stop << " / " << points << "\n"
                  << std::flush;
        start = stop + 1;
    }

    wf.points = wf.values.size();
    return wf;
}

[[nodiscard]] double refTime(const Waveform& ref, double idx)
{
    return ref.xorig + (idx - ref.xref) * ref.xinc;
}

[[nodiscard]] std::vector<std::size_t> evenlySpacedIndices(std::size_t n, std::size_t k)
{
    if (n == 0 || k == 0)
        return {};
    if (n <= k)
    {
        std::vector<std::size_t> idx(n);
        for (std::size_t i = 0; i < n; ++i)
            idx[i] = i;
        return idx;
    }
    if (k == 1)
        return {n / 2};

    std::vector<std::size_t> idx(k);
    for (std::size_t j = 0; j < k; ++j)
        idx[j] = static_cast<std::size_t>(std::llround(
            static_cast<double>(j) * static_cast<double>(n - 1) / static_cast<double>(k - 1)));
    return idx;
}

void appendChar(std::string& buf, char c)
{
    buf.push_back(c);
}

void appendInt(std::string& buf, std::int64_t v)
{
    char tmp[32];
    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), v);
    if (ec != std::errc{})
    {
        std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(v));
        buf.append(tmp);
        return;
    }
    buf.append(tmp, ptr);
}

void appendDouble(std::string& buf, double v)
{
    char tmp[64];
    const int n = std::snprintf(tmp, sizeof(tmp), "%.17g", v);
    if (n > 0)
        buf.append(tmp, static_cast<std::size_t>(n));
    else
        buf.append("0");
}

void flushBuffer(std::ofstream& out, std::string& buf, std::size_t flushAt = 2 * 1024 * 1024)
{
    if (buf.size() >= flushAt)
    {
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        buf.clear();
    }
}

void saveSingleChannelCsv(
    const Waveform& wf,
    const Waveform& ref,
    const std::string& prefix,
    const std::filesystem::path& outDir)
{
    const std::size_t n = wf.points;
    const std::size_t refN = ref.points;
    const double ratio = (n > 1) ? static_cast<double>(refN - 1) / static_cast<double>(n - 1) : 1.0;

    const auto path = outDir / (prefix + "_" + wf.channel + ".csv");
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Cannot open " + path.string());

    std::string buf;
    buf.reserve(4 * 1024 * 1024);
    buf.append("index,time_s,voltage_V\n");

    for (std::size_t i = 0; i < n; ++i)
    {
        appendInt(buf, static_cast<std::int64_t>(i));
        appendChar(buf, ',');
        appendDouble(buf, refTime(ref, static_cast<double>(i) * ratio));
        appendChar(buf, ',');
        appendDouble(buf, wf.values[i]);
        appendChar(buf, '\n');
        flushBuffer(out, buf);
    }

    if (!buf.empty())
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));

    std::cout << "Saved " << path << "  (" << n << " rows)\n";
}

[[nodiscard]] std::size_t alignedCount(const std::vector<Waveform>& waveforms)
{
    std::size_t n = std::numeric_limits<std::size_t>::max();
    for (const auto& wf : waveforms)
        n = std::min(n, wf.points);
    return n;
}

[[nodiscard]] double alignedRefIndex(std::size_t i, std::size_t alignedN, std::size_t refN)
{
    if (alignedN <= 1)
        return 0.0;
    return std::llround(static_cast<double>(i) * static_cast<double>(refN - 1)
                       / static_cast<double>(alignedN - 1));
}

[[nodiscard]] std::size_t alignedChannelIndex(
    std::size_t i,
    std::size_t alignedN,
    std::size_t wfN)
{
    if (wfN == alignedN)
        return i;
    if (alignedN <= 1)
        return 0;
    return static_cast<std::size_t>(std::llround(
        static_cast<double>(i) * static_cast<double>(wfN - 1) / static_cast<double>(alignedN - 1)));
}

void saveAlignedCsv(
    const std::vector<Waveform>& waveforms,
    const Waveform& ref,
    const std::string& prefix,
    const std::filesystem::path& outDir)
{
    const std::size_t alignedN = alignedCount(waveforms);
    const std::size_t refN = ref.points;

    const auto path = outDir / (prefix + "_aligned.csv");
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Cannot open " + path.string());

    std::string buf;
    buf.reserve(4 * 1024 * 1024);
    buf.append("rowid,time_s");
    for (const auto& wf : waveforms)
    {
        appendChar(buf, ',');
        buf.append(wf.channel);
    }
    appendChar(buf, '\n');

    for (std::size_t i = 0; i < alignedN; ++i)
    {
        const double refIdx = alignedRefIndex(i, alignedN, refN);
        appendInt(buf, static_cast<std::int64_t>(i));
        appendChar(buf, ',');
        appendDouble(buf, refTime(ref, refIdx));
        for (const auto& wf : waveforms)
        {
            appendChar(buf, ',');
            const std::size_t j = alignedChannelIndex(i, alignedN, wf.points);
            appendDouble(buf, wf.values[j]);
        }
        appendChar(buf, '\n');
        flushBuffer(out, buf);
    }

    if (!buf.empty())
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));

    std::cout << "Saved " << path << "  (" << alignedN << " rows)\n";
}

void saveDecimatedCsv(
    const std::vector<Waveform>& waveforms,
    const Waveform& ref,
    const std::string& prefix,
    const std::filesystem::path& outDir,
    std::size_t outputPoints)
{
    const std::size_t alignedN = alignedCount(waveforms);
    const std::size_t refN = ref.points;
    const auto idxs = evenlySpacedIndices(alignedN, std::min(outputPoints, alignedN));

    const auto path = outDir / (prefix + "_decimated.csv");
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Cannot open " + path.string());

    std::string buf;
    buf.reserve(512 * 1024);
    buf.append("rowid,time_s");
    for (const auto& wf : waveforms)
    {
        appendChar(buf, ',');
        buf.append(wf.channel);
    }
    appendChar(buf, '\n');

    for (std::size_t newI = 0; newI < idxs.size(); ++newI)
    {
        const std::size_t srcI = idxs[newI];
        const double refIdx = alignedRefIndex(srcI, alignedN, refN);

        appendInt(buf, static_cast<std::int64_t>(newI));
        appendChar(buf, ',');
        appendDouble(buf, refTime(ref, refIdx));
        for (const auto& wf : waveforms)
        {
            appendChar(buf, ',');
            const std::size_t j = alignedChannelIndex(srcI, alignedN, wf.points);
            appendDouble(buf, wf.values[j]);
        }
        appendChar(buf, '\n');
    }

    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout << "Saved " << path << "  (" << idxs.size() << " rows)\n";
}

[[nodiscard]] std::filesystem::path decimatedCsvPath(
    const std::filesystem::path& outDir,
    const std::string& prefix)
{
    return outDir / (prefix + "_decimated.csv");
}

void xzCompressDecimatedCsv(const std::filesystem::path& csvPath)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(csvPath, ec))
        throw std::runtime_error("Decimated CSV not found for xz: " + csvPath.string());

    std::ostringstream cmd;
    cmd << "xz -6 -f \"" << csvPath.string() << "\"";
    std::cout << "Compressing " << csvPath.filename() << " with xz -6...\n";
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0)
        throw std::runtime_error("xz failed (exit " + std::to_string(rc) + ") on " + csvPath.string());

    const auto xzPath = csvPath.string() + ".xz";
    std::cout << "Saved " << xzPath << "\n";
}

void saveScreenshot(TcpScpiSession& scope, const std::filesystem::path& outDir, const std::string& prefix)
{
    std::cout << "Capturing screenshot...\n";
    const auto png = scope.queryBinaryBlock(":DISP:DATA? PNG");
    scope.drainErrors("DISP:DATA? PNG");

    const auto path = outDir / (prefix + "screenshot.png");
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot open " + path.string());
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    std::cout << "Saved " << path << "  (" << png.size() << " bytes)\n";
}

[[nodiscard]] std::filesystem::path findPlotScript()
{
    const std::array candidates = {
        std::filesystem::current_path() / "plot_checks.py",
        std::filesystem::path(__FILE__).parent_path() / "plot_checks.py",
    };
    for (const auto& p : candidates)
    {
        if (std::filesystem::exists(p))
            return p;
    }
    return {};
}

void runPlotChecks(
    const std::filesystem::path& outDir,
    const std::string& prefix,
    const std::vector<std::string>& channels)
{
    const auto plotScript = findPlotScript();
    if (plotScript.empty())
    {
        std::cerr << "plot_checks.py not found; skipping plots\n";
        return;
    }

    std::ostringstream cmd;
    cmd << "python3 \"" << plotScript.string() << "\" \"" << outDir.string() << "\" \"" << prefix
        << "\"";
    for (const auto& ch : channels)
        cmd << " " << ch;

    std::cout << "Running plot_checks.py (decimated only)...\n";
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0)
        std::cerr << "plot_checks.py exited with code " << rc << "\n";
}

void runAnalysis(
    const std::filesystem::path& outDir,
    const std::string& prefix,
    double targetFundamentalHz,
    int maxHarmonic,
    std::filesystem::path& analysisLogOut)
{
    const auto decimatedPath = outDir / (prefix + "_decimated.csv");
    analysisLogOut = outDir / "output.log";

    scope_analyzer::AnalysisOptions options;
    options.targetFundamentalHz = targetFundamentalHz;
    options.maxHarmonic = maxHarmonic;

    std::cout << "Running waveform analysis on " << decimatedPath.filename() << "...\n";
    const auto result = scope_analyzer::analyzeCsvFile(decimatedPath.string(), options);

    std::ofstream log(analysisLogOut);
    if (!log)
        throw std::runtime_error("Cannot open " + analysisLogOut.string());

    scope_analyzer::ReportOptions report;
    report.inputFile = decimatedPath.string();
    report.outputLog = analysisLogOut.string();
    report.targetFundamentalHz = targetFundamentalHz;
    report.maxHarmonic = maxHarmonic;

    scope_analyzer::writeAnalysisReport(log, result, report);
    std::cout << "Saved " << analysisLogOut << "\n";
}

[[nodiscard]] std::string timestampDirName(const std::string& prefix)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &tm);
    return prefix + buf;
}

[[nodiscard]] std::string channelNumberToName(int n)
{
    if (n < 1 || n > 4)
        throw std::invalid_argument("Channel number must be 1..4, got " + std::to_string(n));
    return "CHAN" + std::to_string(n);
}

[[nodiscard]] bool isChannelNumberToken(std::string_view tok)
{
    if (tok.empty() || tok.size() > 1)
        return false;
    const char c = tok[0];
    return c >= '1' && c <= '4';
}

[[nodiscard]] std::string normalizeChannelToken(std::string tok)
{
    tok = trim(tok);
    if (tok.empty())
        throw std::invalid_argument("Empty channel token");

    if (tok.size() >= 4 && (tok.starts_with("CHAN") || tok.starts_with("chan")))
    {
        const int n = std::stoi(tok.substr(4));
        return channelNumberToName(n);
    }

    if (isChannelNumberToken(tok))
        return channelNumberToName(tok[0] - '0');

    throw std::invalid_argument("Invalid channel: " + tok + " (use 1..4 or CHANn)");
}

[[nodiscard]] std::vector<std::string> parseChannelList(std::string_view list)
{
    std::vector<std::string> out;
    std::stringstream ss{std::string(list)};
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        tok = trim(tok);
        if (!tok.empty())
            out.push_back(normalizeChannelToken(tok));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> parseChannelNumber(int n)
{
    return {channelNumberToName(n)};
}

void sortChannelsByNumber(std::vector<std::string>& channels)
{
    std::ranges::sort(channels, [](const std::string& a, const std::string& b) {
        return std::stoi(a.substr(4)) < std::stoi(b.substr(4));
    });
}

void dedupeChannels(std::vector<std::string>& channels)
{
    std::vector<std::string> unique;
    unique.reserve(channels.size());
    for (const auto& ch : channels)
    {
        if (std::find(unique.begin(), unique.end(), ch) == unique.end())
            unique.push_back(ch);
    }
    channels = std::move(unique);
}

} // namespace

void validateChannels(const std::vector<std::string>& channels)
{
    if (channels.empty())
        throw std::invalid_argument("At least one channel is required (1..4)");

    static const std::array<const char*, 4> valid = {"CHAN1", "CHAN2", "CHAN3", "CHAN4"};
    for (const auto& ch : channels)
    {
        const bool ok = std::find(valid.begin(), valid.end(), ch) != valid.end();
        if (!ok)
        {
            throw std::invalid_argument(
                "Invalid channel: " + ch + ". Valid: 1, 2, 3, 4");
        }
    }
}

DownloadResult runDownload(const DownloadConfig& config)
{
    validateChannels(config.channels);

    DownloadResult result;
    result.outDir = timestampDirName(config.outDirPrefix);
    std::filesystem::create_directories(result.outDir);
    std::cout << "Output directory: " << std::filesystem::absolute(result.outDir) << "\n";
    std::cout << "Channels:";
    for (const auto& ch : config.channels)
        std::cout << " " << ch;
    std::cout << "\n";

    TcpScpiSession scope(config.ip, config.port);
    scope.connect();

    std::cout << scope.query("*IDN?") << "\n";
    scope.drainErrors("startup", true);

    scope.writeCmd(":STOP");
    scope.drainErrors("STOP");

    result.memoryDepth = acquireMemoryDepth(scope);
    scope.drainErrors("ACQ:MDEP");
    std::cout << "Memory depth (points): " << result.memoryDepth << "\n";

    for (const auto& ch : config.channels)
    {
        auto wf = readChannelRaw(
            scope, ch, result.memoryDepth, config.chunkPoints, config.resetPauseSec);
        result.waveforms.push_back(std::move(wf));
    }

    result.refWaveform = *std::max_element(
        result.waveforms.begin(),
        result.waveforms.end(),
        [](const Waveform& a, const Waveform& b) { return a.points < b.points; });

    for (const auto& wf : result.waveforms)
    {
        const double t0 = refTime(result.refWaveform, 0);
        const double t1 = refTime(result.refWaveform, static_cast<double>(result.refWaveform.points - 1));
        const double ratio = static_cast<double>(result.refWaveform.points) / static_cast<double>(wf.points);
        std::cout << "  " << wf.channel << ": " << wf.points << " pts "
                  << "(ratio " << static_cast<int>(ratio + 0.5) << "x), "
                  << "time [" << std::scientific << t0 << " .. " << t1 << "]\n"
                  << std::defaultfloat;

        if (result.memoryDepth > 0
            && wf.points < static_cast<std::size_t>(result.memoryDepth * 0.9)
            && wf.channel != result.refWaveform.channel)
        {
            std::cerr << "WARNING: " << wf.channel << " may be truncated ("
                      << wf.points << " vs memory depth " << result.memoryDepth
                      << "). Try increasing --reset-pause.\n";
        }
    }

    if (config.saveRawCsv)
    {
        for (const auto& wf : result.waveforms)
            saveSingleChannelCsv(wf, result.refWaveform, config.outPrefix, result.outDir);
    }

    if (config.saveAlignedCsv)
        saveAlignedCsv(result.waveforms, result.refWaveform, config.outPrefix, result.outDir);

    saveDecimatedCsv(
        result.waveforms, result.refWaveform, config.outPrefix, result.outDir, config.outputPoints);

    if (config.analysis)
    {
        runAnalysis(
            result.outDir,
            config.outPrefix,
            config.targetFundamentalHz,
            config.maxHarmonic,
            result.analysisLog);
    }

    if (config.plots)
        runPlotChecks(result.outDir, config.outPrefix, config.channels);

    if (config.xzDecimated)
        xzCompressDecimatedCsv(decimatedCsvPath(result.outDir, config.outPrefix));

    if (config.screenshot)
        saveScreenshot(scope, result.outDir, config.outPrefix);

    scope.close();
    return result;
}

DownloadConfig parseArgs(int argc, char** argv)
{
    DownloadConfig cfg;
    bool channelsFromFlag = false;
    std::vector<std::string> positionalChannels;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("Missing value for ") + name);
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help")
            throw std::runtime_error("help");
        if (arg.starts_with("--"))
        {
            if (arg == "--ip")
                cfg.ip = needValue("--ip");
            else if (arg == "--port")
                cfg.port = static_cast<std::uint16_t>(std::stoi(needValue("--port")));
            else if (arg == "--channels")
            {
                cfg.channels = parseChannelList(needValue("--channels"));
                channelsFromFlag = true;
            }
            else if (arg == "--chunk")
                cfg.chunkPoints = static_cast<std::size_t>(std::stoull(needValue("--chunk")));
            else if (arg == "--decimate")
                cfg.outputPoints = static_cast<std::size_t>(std::stoull(needValue("--decimate")));
            else if (arg == "--reset-pause")
                cfg.resetPauseSec = std::stod(needValue("--reset-pause"));
            else if (arg == "--out-prefix")
                cfg.outPrefix = needValue("--out-prefix");
            else if (arg == "--out-dir-prefix")
                cfg.outDirPrefix = needValue("--out-dir-prefix");
            else if (arg == "--no-raw")
                cfg.saveRawCsv = false;
            else if (arg == "--no-aligned")
                cfg.saveAlignedCsv = false;
            else if (arg == "--xzDecimated")
                cfg.xzDecimated = true;
            else if (arg == "--no-plots")
                cfg.plots = false;
            else if (arg == "--no-screenshot")
                cfg.screenshot = false;
            else if (arg == "--no-analysis")
                cfg.analysis = false;
            else if (arg == "--fundamental")
                cfg.targetFundamentalHz = std::stod(needValue("--fundamental"));
            else if (arg == "--max-harmonic")
                cfg.maxHarmonic = std::stoi(needValue("--max-harmonic"));
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        else if (isChannelNumberToken(arg))
        {
            auto ch = parseChannelNumber(arg[0] - '0');
            positionalChannels.insert(positionalChannels.end(), ch.begin(), ch.end());
        }
        else
            throw std::runtime_error("Unknown argument: " + arg + " (channels: 1..4)");
    }

    if (!positionalChannels.empty())
    {
        cfg.channels = std::move(positionalChannels);
    }
    else if (!channelsFromFlag)
    {
        cfg.channels = {"CHAN1", "CHAN2", "CHAN3", "CHAN4"};
    }

    dedupeChannels(cfg.channels);
    sortChannelsByNumber(cfg.channels);
    validateChannels(cfg.channels);

    return cfg;
}

void printUsage(const char* program)
{
    std::cout
        << "Usage: " << program << " [options] [channels...]\n"
        << "Download deep-memory waveforms from Rigol DHO800/DHO900.\n\n"
        << "Channels (pick one style):\n"
        << "  " << program << " 1 2 3 4          Positional channel numbers\n"
        << "  " << program << " 1 3              Same as --channels 1,3\n"
        << "  --channels 1,3,4     Comma-separated 1..4 (default: all four)\n\n"
        << "Options:\n"
        << "  --ip ADDR            Scope IP (default: 192.168.1.162)\n"
        << "  --port PORT          SCPI TCP port (default: 5555)\n"
        << "  --chunk N            Samples per :WAV:DATA? request (default: 250000)\n"
        << "  --decimate N         Decimated CSV rows (default: 10000)\n"
        << "  --reset-pause SEC    Pause between channel reads (default: 0.5)\n"
        << "  --out-prefix PREFIX  Output file prefix (default: empty -> _CHAN1.csv)\n"
        << "  --out-dir-prefix P   Output folder prefix (default: aq_)\n"
        << "  --no-raw             Skip per-channel full-depth CSVs\n"
        << "  --no-aligned         Skip time-aligned multi-channel CSV\n"
        << "  --xzDecimated        Compress _decimated.csv with xz -6 (after analysis/plots)\n"
        << "  --no-plots           Skip verification PNGs\n"
        << "  --no-screenshot      Skip display screenshot\n"
        << "  --no-analysis        Skip FFT report (output.log)\n"
        << "  --fundamental HZ     Target fundamental for analysis (default: 50)\n"
        << "  --max-harmonic N     Max harmonic order for analysis (default: 15)\n"
        << "  -h, --help           Show this help\n";
}

} // namespace scope_download
