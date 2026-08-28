#include "scope_download.hpp"

#include <iostream>
#include <string_view>

/*
g++ -std=c++20 -O2 -Wall -Wextra -o scope_download \
    scope_download.cpp scope_download_main.cpp scope_analyzer.cpp -lfftw3 -pthread

./scope_download                 # 12-bit WORD transfer; writes _CHAN1.bin + .json
./scope_download 1 --csv         # also write the old full-rate _CHAN1.csv
./scope_download 1 --format byte # 8-bit, the old transfer width

Exit status: 0 on success, 1 if a channel clips beyond --clip-tolerance, 2 on error.
*/

int main(int argc, char** argv)
{
    try
    {
        scope_download::DownloadConfig config;
        try
        {
            config = scope_download::parseArgs(argc, argv);
        }
        catch (const std::runtime_error& e)
        {
            if (std::string_view(e.what()) == "help")
            {
                scope_download::printUsage(argv[0]);
                return 0;
            }
            if (std::string_view(e.what()) == "usage")
            {
                scope_download::printUsage(argv[0]);
                return 1;
            }
            throw;
        }

        const auto result = scope_download::runDownload(config);

        // Resolution and clipping decide whether the capture is usable at all, so
        // repeat them here where they are not buried under the per-chunk progress.
        bool anyClipped = false;
        for (const auto& wf : result.waveforms)
        {
            std::cout << wf.channel << ": " << wf.effectiveBits << " effective bits, LSB "
                      << wf.effectiveLsbVolts * 1e3 << " mV, codes " << wf.codeMin << ".."
                      << wf.codeMax << " of 0.." << wf.codeFull;
            if (wf.clipped)
            {
                std::cout << "  (" << wf.clippedPercent << " % on a rail)";
                if (wf.clippedPercent > config.clipTolerancePercent)
                {
                    std::cout << "  <-- CLIPPING";
                    anyClipped = true;
                }
            }
            std::cout << "\n";
        }

        std::cout << "Done. Output: " << result.outDir;
        if (!result.analysisLog.empty())
            std::cout << "  (report: " << result.analysisLog.filename() << ")";
        std::cout << "\n";

        if (anyClipped)
        {
            std::cerr << "\nWARNING: at least one channel clips beyond the "
                      << config.clipTolerancePercent
                      << " % tolerance. Raise V/div, or raise\n"
                         "         the tolerance with --clip-tolerance if the loss is "
                         "acceptable for this measurement.\n";
            return 1;
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
