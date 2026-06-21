#pragma once
// Shared helpers for the datacook / datacheck CLIs (file IO + error printing).
// Build-time only; not part of the runtime.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace Neuron::Tools
{

[[nodiscard]] inline bool ReadFileText(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline void PrintErrors(const std::string& path, const std::vector<std::string>& errs)
{
    for (const auto& e : errs) std::cerr << path << ": " << e << "\n";
}

} // namespace Neuron::Tools
