#include "scope_analyzer.hpp"

#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    try
    {
        scope_analyzer::RunPaths paths;
        try
        {
            paths = scope_analyzer::parseRunPaths(argc, argv);
        }
        catch (const std::runtime_error& e)
        {
            if (std::string_view(e.what()) == "missing argument" ||
                std::string_view(e.what()) == "missing output log path")
            {
                scope_analyzer::printUsage(argv[0]);
                return 1;
            }
            throw;
        }

        scope_analyzer::AnalysisOptions options;
        options.targetFundamentalHz = paths.targetFundamentalHz;
        options.maxHarmonic = paths.maxHarmonic;

        const auto result = scope_analyzer::analyzeCsvFile(paths.inputCsv, options);

        std::ofstream log(paths.outputLog);
        if (!log)
            throw std::runtime_error("Cannot open output log: " + paths.outputLog);

        scope_analyzer::ReportOptions report;
        report.inputFile = paths.inputCsv;
        report.outputLog = paths.outputLog;
        report.targetFundamentalHz = paths.targetFundamentalHz;
        report.maxHarmonic = paths.maxHarmonic;

        scope_analyzer::writeAnalysisReport(log, result, report);

        std::cout << "Analysis complete. Log written to: " << paths.outputLog << "\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
