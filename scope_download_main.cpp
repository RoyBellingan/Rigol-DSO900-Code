#include "scope_download.hpp"

#include <iostream>
#include <string_view>

/*
g++ -std=c++20 -O2 -Wall -Wextra -o scope_download \
    scope_download.cpp scope_download_main.cpp scope_analyzer.cpp -lfftw3 -pthread
    
./scope_download
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
        std::cout << "Done. Output: " << result.outDir;
        if (!result.analysisLog.empty())
            std::cout << "  (report: " << result.analysisLog.filename() << ")";
        std::cout << "\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
