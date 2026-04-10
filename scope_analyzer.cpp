#include <fftw3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
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
        double amplitude = 0.0;      // peak amplitude estimate
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
        double harmonicSimilarity = 0.0; // 1 is ideal
        double residualCorrelation = 0.0;
    };

    [[nodiscard]] static std::string trim(std::string_view sv)
    {
        std::size_t a = 0;
        while (a < sv.size() && std::isspace(static_cast<unsigned char>(sv[a])))
            ++a;

        std::size_t b = sv.size();
        while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1])))
            --b;

        if (b > a && sv[a] == '"' && sv[b - 1] == '"' && (b - a) >= 2)
        {
            ++a;
            --b;
        }

        return std::string(sv.substr(a, b - a));
    }

    [[nodiscard]] static std::string asciiLowerCopy(std::string_view sv)
    {
        std::string s(trim(sv));
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    [[nodiscard]] static bool isTimeColumnHeader(const std::string& header)
    {
        const std::string k = asciiLowerCopy(header);
        if (k == "time" || k == "time_s" || k == "t_s" || k == "timestamp" || k == "seconds" || k == "sec")
            return true;
        std::string compact;
        compact.reserve(k.size());
        for (char c : k)
        {
            if (!std::isspace(static_cast<unsigned char>(c)))
                compact.push_back(c);
        }
        return compact == "time(s)" || compact == "time[s]";
    }

    [[nodiscard]] static bool isAuxiliarySkipHeader(const std::string& header)
    {
        const std::string k = asciiLowerCopy(header);
        return k == "rowid" || k == "row_id" || k == "rowindex" || k == "row_index";
    }

    [[nodiscard]] static std::size_t findTimeColumnIndex(const std::vector<std::string>& headers)
    {
        for (std::size_t i = 0; i < headers.size(); ++i)
        {
            if (isTimeColumnHeader(headers[i]))
                return i;
        }
        return 0;
    }

    [[nodiscard]] static std::vector<std::string> splitCsvLine(const std::string& line)
    {
        std::vector<std::string> out;
        std::string cur;
        bool inQuotes = false;

        for (std::size_t i = 0; i < line.size(); ++i)
        {
            char c = line[i];
            if (c == '"')
            {
                if (inQuotes && i + 1 < line.size() && line[i + 1] == '"')
                {
                    cur.push_back('"');
                    ++i;
                }
                else
                {
                    inQuotes = !inQuotes;
                }
            }
            else if (c == ',' && !inQuotes)
            {
                out.push_back(trim(cur));
                cur.clear();
            }
            else
            {
                cur.push_back(c);
            }
        }
        out.push_back(trim(cur));
        return out;
    }

    [[nodiscard]] static std::optional<double> parseDouble(std::string_view sv)
    {
        sv = std::string_view(sv).substr(0, sv.size());

        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
            sv.remove_prefix(1);
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
            sv.remove_suffix(1);

        if (sv.empty())
            return std::nullopt;

        double value = 0.0;
        const auto* first = sv.data();
        const auto* last  = sv.data() + sv.size();

        auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec == std::errc{} && ptr == last)
            return value;

        try
        {
            return std::stod(std::string(sv));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    [[nodiscard]] static CsvData readCsv(const std::string& path)
    {
        std::ifstream fin(path);
        if (!fin)
            throw std::runtime_error("Cannot open input CSV: " + path);

        std::string line;
        if (!std::getline(fin, line))
            throw std::runtime_error("CSV is empty");

        CsvData data;
        data.headers = splitCsvLine(line);
        if (data.headers.empty())
            throw std::runtime_error("CSV header is empty");

        data.columns.resize(data.headers.size());

        std::size_t row = 0;
        while (std::getline(fin, line))
        {
            ++row;
            if (line.empty())
                continue;

            auto fields = splitCsvLine(line);
            if (fields.size() < data.headers.size())
                fields.resize(data.headers.size());

            bool anyValid = false;
            std::vector<double> parsed(data.headers.size(), std::numeric_limits<double>::quiet_NaN());

            for (std::size_t c = 0; c < data.headers.size(); ++c)
            {
                if (auto v = parseDouble(fields[c]); v.has_value())
                {
                    parsed[c] = *v;
                    anyValid = true;
                }
            }

            if (!anyValid)
                continue;

            for (std::size_t c = 0; c < data.headers.size(); ++c)
                data.columns[c].push_back(parsed[c]);
        }

        if (data.columns.empty() || data.columns[0].empty())
            throw std::runtime_error("No numeric rows found in CSV");

        return data;
    }

    [[nodiscard]] static bool isFiniteVector(const std::vector<double>& v)
    {
        return std::ranges::all_of(v, [](double x){ return std::isfinite(x); });
    }

    [[nodiscard]] static double mean(const std::vector<double>& v)
    {
        return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    }

    [[nodiscard]] static double rms(const std::vector<double>& v)
    {
        double s = 0.0;
        for (double x : v) s += x * x;
        return std::sqrt(s / static_cast<double>(v.size()));
    }

    [[nodiscard]] static double stddev(const std::vector<double>& v, double mu)
    {
        double s = 0.0;
        for (double x : v)
        {
            const double d = x - mu;
            s += d * d;
        }
        return std::sqrt(s / static_cast<double>(v.size()));
    }

    [[nodiscard]] static double minValue(const std::vector<double>& v)
    {
        return *std::ranges::min_element(v);
    }

    [[nodiscard]] static double maxValue(const std::vector<double>& v)
    {
        return *std::ranges::max_element(v);
    }

    [[nodiscard]] static std::vector<double> detrendDc(const std::vector<double>& v, double mu)
    {
        std::vector<double> out(v.size());
        for (std::size_t i = 0; i < v.size(); ++i)
            out[i] = v[i] - mu;
        return out;
    }

    [[nodiscard]] static double estimateDt(const std::vector<double>& t)
    {
        if (t.size() < 2)
            throw std::runtime_error("Need at least 2 time samples");

        std::vector<double> dts;
        dts.reserve(t.size() - 1);
        for (std::size_t i = 1; i < t.size(); ++i)
        {
            const double dt = t[i] - t[i - 1];
            if (std::isfinite(dt) && dt > 0)
                dts.push_back(dt);
        }

        if (dts.empty())
            throw std::runtime_error("Invalid time axis");

        std::ranges::sort(dts);
        return dts[dts.size() / 2];
    }

    struct FftResult
    {
        int n = 0;
        double fs = 0.0;
        std::vector<std::complex<double>> bins; // 0..N/2
    };

    [[nodiscard]] static FftResult computeRfft(const std::vector<double>& x, double fs)
    {
        const int N = static_cast<int>(x.size());
        if (N < 4)
            throw std::runtime_error("Signal too short for FFT");

        std::vector<double> in(x.begin(), x.end());
        const int nOut = N / 2 + 1;
        fftw_complex* out = reinterpret_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * nOut));
        if (!out)
            throw std::runtime_error("fftw_malloc failed");

        fftw_plan plan = fftw_plan_dft_r2c_1d(N, in.data(), out, FFTW_ESTIMATE);
        if (!plan)
        {
            fftw_free(out);
            throw std::runtime_error("fftw_plan_dft_r2c_1d failed");
        }

        fftw_execute(plan);

        FftResult result;
        result.n = N;
        result.fs = fs;
        result.bins.resize(nOut);

        for (int k = 0; k < nOut; ++k)
            result.bins[k] = std::complex<double>(out[k][0], out[k][1]);

        fftw_destroy_plan(plan);
        fftw_free(out);
        return result;
    }

    [[nodiscard]] static double binFrequency(const FftResult& fft, int k)
    {
        return static_cast<double>(k) * fft.fs / static_cast<double>(fft.n);
    }

    [[nodiscard]] static double binAmplitudePeak(const FftResult& fft, int k)
    {
        const double mag = std::abs(fft.bins[k]);
        if (k == 0 || (fft.n % 2 == 0 && k == fft.n / 2))
            return mag / static_cast<double>(fft.n);
        return 2.0 * mag / static_cast<double>(fft.n);
    }

    [[nodiscard]] static double binPhaseRad(const FftResult& fft, int k)
    {
        return std::atan2(fft.bins[k].imag(), fft.bins[k].real());
    }

    [[nodiscard]] static int nearestBin(const FftResult& fft, double freqHz)
    {
        int k = static_cast<int>(std::llround(freqHz * static_cast<double>(fft.n) / fft.fs));
        k = std::clamp(k, 0, static_cast<int>(fft.bins.size()) - 1);
        return k;
    }

    [[nodiscard]] static int dominantBinIgnoringDc(const FftResult& fft)
    {
        int best = 1;
        double bestMag = 0.0;
        for (int k = 1; k < static_cast<int>(fft.bins.size()); ++k)
        {
            const double a = binAmplitudePeak(fft, k);
            if (a > bestMag)
            {
                bestMag = a;
                best = k;
            }
        }
        return best;
    }

    [[nodiscard]] static double wrapPhasePi(double x)
    {
        while (x > PI)  x -= 2.0 * PI;
        while (x < -PI) x += 2.0 * PI;
        return x;
    }

    [[nodiscard]] static double radToDeg(double r)
    {
        return r * 180.0 / PI;
    }

    [[nodiscard]] static double degToTimeSec(double deg, double freqHz)
    {
        return deg / 360.0 / freqHz;
    }

    [[nodiscard]] static double pearsonCorrelation(const std::vector<double>& a, const std::vector<double>& b)
    {
        const std::size_t n = std::min(a.size(), b.size());
        if (n < 2) return 0.0;

        const double ma = std::accumulate(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(n), 0.0) / static_cast<double>(n);
        const double mb = std::accumulate(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n), 0.0) / static_cast<double>(n);

        double num = 0.0;
        double da2 = 0.0;
        double db2 = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double da = a[i] - ma;
            const double db = b[i] - mb;
            num += da * db;
            da2 += da * da;
            db2 += db * db;
        }

        const double den = std::sqrt(da2 * db2);
        if (den <= 0.0) return 0.0;
        return num / den;
    }

    [[nodiscard]] static double normalizedDot(const std::vector<double>& a, const std::vector<double>& b)
    {
        const std::size_t n = std::min(a.size(), b.size());
        if (n == 0) return 0.0;

        double num = 0.0;
        double da2 = 0.0;
        double db2 = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            num += a[i] * b[i];
            da2 += a[i] * a[i];
            db2 += b[i] * b[i];
        }

        const double den = std::sqrt(da2 * db2);
        if (den <= 0.0) return 0.0;
        return num / den;
    }

    [[nodiscard]] static std::pair<int, double> bestCrossCorrelationLag(
        const std::vector<double>& a,
        const std::vector<double>& b,
        int maxLag)
    {
        const int n = static_cast<int>(std::min(a.size(), b.size()));
        if (n < 8) return {0, 0.0};

        double bestScore = -std::numeric_limits<double>::infinity();
        int bestLag = 0;

        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            double num = 0.0;
            double aa = 0.0;
            double bb = 0.0;

            for (int i = 0; i < n; ++i)
            {
                const int j = i + lag;
                if (j < 0 || j >= n) continue;

                num += a[i] * b[j];
                aa += a[i] * a[i];
                bb += b[j] * b[j];
            }

            if (aa <= 0.0 || bb <= 0.0)
                continue;

            const double corr = num / std::sqrt(aa * bb);
            if (corr > bestScore)
            {
                bestScore = corr;
                bestLag = lag;
            }
        }

        return {bestLag, bestScore};
    }

    [[nodiscard]] static std::vector<double> shiftSignal(const std::vector<double>& x, int lag)
    {
        std::vector<double> out(x.size(), 0.0);

        if (lag == 0)
            return x;

        for (int i = 0; i < static_cast<int>(x.size()); ++i)
        {
            const int src = i - lag;
            if (src >= 0 && src < static_cast<int>(x.size()))
                out[i] = x[src];
        }
        return out;
    }

    [[nodiscard]] static double estimateZeroCrossingTime(
        const std::vector<double>& t,
        const std::vector<double>& x)
    {
        const std::size_t n = std::min(t.size(), x.size());
        for (std::size_t i = 1; i < n; ++i)
        {
            if (x[i - 1] <= 0.0 && x[i] > 0.0)
            {
                const double x0 = x[i - 1];
                const double x1 = x[i];
                const double t0 = t[i - 1];
                const double t1 = t[i];
                const double alpha = (0.0 - x0) / (x1 - x0);
                return t0 + alpha * (t1 - t0);
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
    }

    [[nodiscard]] static std::vector<double> buildBestFitSine(
        std::size_t n,
        double fs,
        double freqHz,
        double amplitude,
        double phaseRad)
    {
        std::vector<double> y(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(i) / fs;
            y[i] = amplitude * std::cos(2.0 * PI * freqHz * t + phaseRad);
        }
        return y;
    }

    [[nodiscard]] static std::vector<double> residual(
        const std::vector<double>& x,
        const std::vector<double>& fit)
    {
        std::vector<double> r(x.size(), 0.0);
        for (std::size_t i = 0; i < x.size(); ++i)
            r[i] = x[i] - fit[i];
        return r;
    }

    [[nodiscard]] static double harmonicSimilarity(
        const std::vector<HarmonicInfo>& a,
        const std::vector<HarmonicInfo>& b)
    {
        const std::size_t n = std::min(a.size(), b.size());
        if (n <= 1) return 0.0;

        double err = 0.0;
        double denom = 0.0;

        for (std::size_t i = 1; i < n; ++i)
        {
            const double da = a[i].relToFundamental;
            const double db = b[i].relToFundamental;
            err += std::abs(da - db);
            denom += std::max({std::abs(da), std::abs(db), 1e-12});
        }

        if (denom <= 0.0) return 1.0;
        return std::max(0.0, 1.0 - err / denom);
    }

    [[nodiscard]] static ChannelStats analyzeChannel(
        const std::string& name,
        const std::vector<double>& time,
        const std::vector<double>& raw,
        double fs,
        double targetFundamentalHz,
        int maxHarmonic)
    {
        ChannelStats s;
        s.name = name;

        s.mean = mean(raw);
        s.rms = rms(raw);
        s.stddev = stddev(raw, s.mean);
        s.minv = minValue(raw);
        s.maxv = maxValue(raw);
        s.p2p = s.maxv - s.minv;
        s.crestFactor = (s.rms > 0.0) ? std::max(std::abs(s.minv), std::abs(s.maxv)) / s.rms : 0.0;

        s.detrended = detrendDc(raw, s.mean);
        s.zeroCrossingTime = estimateZeroCrossingTime(time, s.detrended);

        const auto fft = computeRfft(s.detrended, fs);

        s.fundamentalHz = targetFundamentalHz;
        s.fundamentalBin = nearestBin(fft, targetFundamentalHz);
        s.fundamentalAmplitude = binAmplitudePeak(fft, s.fundamentalBin);
        s.fundamentalPhaseRad = binPhaseRad(fft, s.fundamentalBin);
        s.fundamentalPhaseDeg = radToDeg(s.fundamentalPhaseRad);

        s.dominantBin = dominantBinIgnoringDc(fft);
        s.dominantFreqHz = binFrequency(fft, s.dominantBin);
        s.dominantAmplitude = binAmplitudePeak(fft, s.dominantBin);

        s.harmonics.resize(static_cast<std::size_t>(maxHarmonic + 1));
        double harmonicPowerWithoutFundamental = 0.0;
        double totalPower = 0.0;

        for (int k = 1; k < static_cast<int>(fft.bins.size()); ++k)
        {
            const double a = binAmplitudePeak(fft, k);
            totalPower += a * a;
        }

        for (int h = 1; h <= maxHarmonic; ++h)
        {
            const double f = targetFundamentalHz * static_cast<double>(h);
            const int bin = nearestBin(fft, f);
            HarmonicInfo hi;
            hi.harmonic = h;
            hi.freqHz = binFrequency(fft, bin);
            hi.fftBin = bin;
            hi.amplitude = binAmplitudePeak(fft, bin);
            hi.phaseRad = binPhaseRad(fft, bin);
            hi.phaseDeg = radToDeg(hi.phaseRad);
            hi.relToFundamental = (s.fundamentalAmplitude > 0.0) ? hi.amplitude / s.fundamentalAmplitude : 0.0;
            s.harmonics[static_cast<std::size_t>(h)] = hi;

            if (h >= 2)
                harmonicPowerWithoutFundamental += hi.amplitude * hi.amplitude;
        }

        s.totalSpectralEnergy = totalPower;
        s.harmonicEnergy = harmonicPowerWithoutFundamental;
        s.fundamentalEnergyRatio = (totalPower > 0.0) ? (s.fundamentalAmplitude * s.fundamentalAmplitude) / totalPower : 0.0;
        s.thd = (s.fundamentalAmplitude > 0.0) ? std::sqrt(harmonicPowerWithoutFundamental) / s.fundamentalAmplitude : 0.0;

        const auto fit = buildBestFitSine(s.detrended.size(), fs, targetFundamentalHz, s.fundamentalAmplitude, s.fundamentalPhaseRad);
        s.sineResidual = residual(s.detrended, fit);
        s.sineResidualRMS = rms(s.sineResidual);
        s.sineResidualToSignalRMS = (rms(s.detrended) > 0.0) ? s.sineResidualRMS / rms(s.detrended) : 0.0;
        s.sineResidualToSignalStd = (s.stddev > 0.0) ? stddev(s.sineResidual, mean(s.sineResidual)) / s.stddev : 0.0;

        return s;
    }

    [[nodiscard]] static PairStats analyzePair(
        const ChannelStats& a,
        const ChannelStats& b,
        double fs,
        double fundamentalHz)
    {
        PairStats p;
        p.a = a.name;
        p.b = b.name;

        p.pearsonCorrelation = pearsonCorrelation(a.detrended, b.detrended);
        p.normalizedDot = normalizedDot(a.detrended, b.detrended);
        p.amplitudeRatio = (b.rms > 0.0) ? a.rms / b.rms : 0.0;

        p.phaseDiffRad = wrapPhasePi(b.fundamentalPhaseRad - a.fundamentalPhaseRad);
        p.phaseDiffDeg = radToDeg(p.phaseDiffRad);
        p.timeShiftSec = degToTimeSec(p.phaseDiffDeg, fundamentalHz);
        p.timeShiftUs = p.timeShiftSec * 1e6;

        const int maxLag = std::max(1, static_cast<int>(std::llround(fs / fundamentalHz * 0.5)));
        auto [lag, score] = bestCrossCorrelationLag(a.detrended, b.detrended, maxLag);
        p.bestLagSamples = lag;
        p.bestLagSec = static_cast<double>(lag) / fs;
        p.bestLagUs = p.bestLagSec * 1e6;
        p.bestLagCorrelation = score;

        const auto shiftedB = shiftSignal(b.detrended, lag);
        p.alignedCorrelation = pearsonCorrelation(a.detrended, shiftedB);

        p.harmonicSimilarity = harmonicSimilarity(a.harmonics, b.harmonics);
        p.residualCorrelation = pearsonCorrelation(a.sineResidual, b.sineResidual);

        return p;
    }

    static void writeSeparator(std::ostream& os, char ch = '=', int count = 90)
    {
        for (int i = 0; i < count; ++i) os << ch;
        os << '\n';
    }

    static void writeChannelReport(
        std::ostream& os,
        const ChannelStats& s)
    {
        writeSeparator(os);
        os << "CHANNEL: " << s.name << "\n";
        writeSeparator(os, '-');

        os << std::fixed << std::setprecision(9);
        os << "Mean/DC                     : " << s.mean << "\n";
        os << "RMS                         : " << s.rms << "\n";
        os << "StdDev                      : " << s.stddev << "\n";
        os << "Min                         : " << s.minv << "\n";
        os << "Max                         : " << s.maxv << "\n";
        os << "Peak-to-peak                : " << s.p2p << "\n";
        os << "Crest factor                : " << s.crestFactor << "\n";
        os << "\n";

        os << "Dominant FFT bin            : " << s.dominantBin << "\n";
        os << "Dominant frequency [Hz]     : " << s.dominantFreqHz << "\n";
        os << "Dominant amplitude          : " << s.dominantAmplitude << "\n";
        os << "\n";

        os << "Target fundamental [Hz]     : " << s.fundamentalHz << "\n";
        os << "Fundamental bin             : " << s.fundamentalBin << "\n";
        os << "Fundamental amplitude       : " << s.fundamentalAmplitude << "\n";
        os << "Fundamental phase [rad]     : " << s.fundamentalPhaseRad << "\n";
        os << "Fundamental phase [deg]     : " << s.fundamentalPhaseDeg << "\n";
        os << "\n";

        os << "THD                         : " << s.thd << "\n";
        os << "Total spectral energy       : " << s.totalSpectralEnergy << "\n";
        os << "Harmonic energy (2..N)      : " << s.harmonicEnergy << "\n";
        os << "Fundamental energy ratio    : " << s.fundamentalEnergyRatio << "\n";
        os << "\n";

        os << "Sine residual RMS           : " << s.sineResidualRMS << "\n";
        os << "Residual / signal RMS       : " << s.sineResidualToSignalRMS << "\n";
        os << "Residual / signal StdDev    : " << s.sineResidualToSignalStd << "\n";
        os << "\n";

        if (std::isfinite(s.zeroCrossingTime))
            os << "First rising zero-cross [s] : " << s.zeroCrossingTime << "\n";
        else
            os << "First rising zero-cross [s] : n/a\n";

        os << "\n";
        os << "HARMONICS\n";
        writeSeparator(os, '.');
        os << "H  "
        << std::setw(14) << "Freq[Hz]"
        << std::setw(12) << "Bin"
        << std::setw(18) << "Amplitude"
        << std::setw(18) << "Phase[deg]"
        << std::setw(18) << "Rel/Fund"
        << "\n";

        for (std::size_t h = 1; h < s.harmonics.size(); ++h)
        {
            const auto& hi = s.harmonics[h];
            os << std::setw(2) << hi.harmonic
            << std::setw(14) << hi.freqHz
            << std::setw(12) << hi.fftBin
            << std::setw(18) << hi.amplitude
            << std::setw(18) << hi.phaseDeg
            << std::setw(18) << hi.relToFundamental
            << "\n";
        }
        os << "\n";
    }

    static void writePairReport(
        std::ostream& os,
        const PairStats& p)
    {
        writeSeparator(os);
        os << "PAIR: " << p.a << "  <->  " << p.b << "\n";
        writeSeparator(os, '-');

        os << std::fixed << std::setprecision(9);
        os << "Pearson correlation         : " << p.pearsonCorrelation << "\n";
        os << "Normalized dot              : " << p.normalizedDot << "\n";
        os << "Amplitude ratio A/B         : " << p.amplitudeRatio << "\n";
        os << "\n";

        os << "Phase diff [rad]            : " << p.phaseDiffRad << "\n";
        os << "Phase diff [deg]            : " << p.phaseDiffDeg << "\n";
        os << "Time shift [s]              : " << p.timeShiftSec << "\n";
        os << "Time shift [us]             : " << p.timeShiftUs << "\n";
        os << "\n";

        os << "Best cross-corr lag [samp]  : " << p.bestLagSamples << "\n";
        os << "Best cross-corr lag [s]     : " << p.bestLagSec << "\n";
        os << "Best cross-corr lag [us]    : " << p.bestLagUs << "\n";
        os << "Best cross-corr score       : " << p.bestLagCorrelation << "\n";
        os << "Aligned correlation         : " << p.alignedCorrelation << "\n";
        os << "\n";

        os << "Harmonic similarity         : " << p.harmonicSimilarity << "\n";
        os << "Residual correlation        : " << p.residualCorrelation << "\n";
        os << "\n";
    }

    [[nodiscard]] static std::string nowString()
    {
        return "local-time-unavailable-in-portable-std-only-build";
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc < 3)
        {
            std::cerr << "Usage: " << argv[0]
            << " <input.csv> <output.log> [fundamental_hz] [max_harmonic]\n";
            return 1;
        }

        const std::string inputCsv = argv[1];
        const std::string outputLog = argv[2];
        const double targetFundamentalHz = (argc >= 4) ? std::stod(argv[3]) : 50.0;
        const int maxHarmonic = (argc >= 5) ? std::stoi(argv[4]) : 15;

        if (targetFundamentalHz <= 0.0)
            throw std::runtime_error("fundamental_hz must be > 0");

        if (maxHarmonic < 1)
            throw std::runtime_error("max_harmonic must be >= 1");

        const CsvData csv = readCsv(inputCsv);
        if (csv.headers.size() < 2)
            throw std::runtime_error("Need at least time column + one signal column");

        const std::size_t timeIdx = findTimeColumnIndex(csv.headers);
        const auto& time = csv.columns[timeIdx];
        if (!isFiniteVector(time))
            throw std::runtime_error("Time column contains non-finite values");

        const double dt = estimateDt(time);
        const double fs = 1.0 / dt;
        const double duration = (time.back() - time.front());
        const std::size_t N = time.size();

        std::vector<ChannelStats> channels;
        for (std::size_t c = 0; c < csv.headers.size(); ++c)
        {
            if (c == timeIdx)
                continue;
            if (isAuxiliarySkipHeader(csv.headers[c]))
                continue;

            const auto& col = csv.columns[c];
            if (col.size() != time.size())
                continue;
            if (!isFiniteVector(col))
                continue;

            channels.push_back(analyzeChannel(csv.headers[c], time, col, fs, targetFundamentalHz, maxHarmonic));
        }

        if (channels.empty())
            throw std::runtime_error("No valid numeric signal channels found");

        std::vector<PairStats> pairs;
        for (std::size_t i = 0; i < channels.size(); ++i)
        {
            for (std::size_t j = i + 1; j < channels.size(); ++j)
                pairs.push_back(analyzePair(channels[i], channels[j], fs, targetFundamentalHz));
        }

        std::ofstream log(outputLog);
        if (!log)
            throw std::runtime_error("Cannot open output log: " + outputLog);

        writeSeparator(log);
        log << "OSCILLOSCOPE CSV ANALYSIS REPORT\n";
        writeSeparator(log);
        log << "Input file                  : " << inputCsv << "\n";
        log << "Output log                  : " << outputLog << "\n";
        log << "Generated                   : " << nowString() << "\n";
        log << "\n";

        log << std::fixed << std::setprecision(9);
        log << "Rows                        : " << N << "\n";
        log << "Duration [s]                : " << duration << "\n";
        log << "Estimated dt [s]            : " << dt << "\n";
        log << "Estimated Fs [Hz]           : " << fs << "\n";
        log << "Target fundamental [Hz]     : " << targetFundamentalHz << "\n";
        log << "Max harmonic                : " << maxHarmonic << "\n";
        log << "Time column                 : " << csv.headers[timeIdx] << " (column " << timeIdx << ")\n";
        log << "Signal channels             : " << channels.size() << "\n";
        log << "\n";

        writeSeparator(log);
        log << "CHANNEL LIST\n";
        writeSeparator(log, '-');
        for (const auto& ch : channels)
            log << " - " << ch.name << "\n";
        log << "\n";

        for (const auto& ch : channels)
            writeChannelReport(log, ch);

        writeSeparator(log);
        log << "PAIRWISE ANALYSIS\n";
        writeSeparator(log);
        for (const auto& p : pairs)
            writePairReport(log, p);

        writeSeparator(log);
        log << "SUMMARY HINTS\n";
        writeSeparator(log, '-');

        for (const auto& p : pairs)
        {
            log << p.a << " vs " << p.b << ":\n";
            log << "  Phase diff [deg]          : " << p.phaseDiffDeg << "\n";
            log << "  Time shift [us]           : " << p.timeShiftUs << "\n";
            log << "  Pearson corr              : " << p.pearsonCorrelation << "\n";
            log << "  Aligned corr              : " << p.alignedCorrelation << "\n";
            log << "  Harmonic similarity       : " << p.harmonicSimilarity << "\n";
            log << "  Residual correlation      : " << p.residualCorrelation << "\n";

            if (std::abs(p.phaseDiffDeg) < 2.0 &&
                p.alignedCorrelation > 0.98 &&
                p.harmonicSimilarity > 0.9)
            {
                log << "  Verdict                   : very similar waveform and nearly phase aligned\n";
            }
            else if (p.alignedCorrelation > 0.9 && p.harmonicSimilarity > 0.8)
            {
                log << "  Verdict                   : similar shape, possible moderate phase/amplitude offset\n";
            }
            else
            {
                log << "  Verdict                   : waveform family likely different or strongly distorted\n";
            }
            log << "\n";
        }

        std::cout << "Analysis complete. Log written to: " << outputLog << "\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
