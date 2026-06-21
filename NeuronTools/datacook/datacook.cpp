// datacook — text universe source → packed binary (masterplan §12.6).
//
//   datacook <in.universe> <out.bin>
//
// Parses the §4 schema, runs the same referential-integrity checks as datacheck
// (a cook never emits an invalid blob), encodes via the shared NeuronCore codec,
// and self-checks by decoding the result. Exit: 0 ok · 1 parse/validate error ·
// 2 usage/IO · 3 internal.

#include "ToolCommon.h"
#include "UniverseSource.h"

#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: datacook <in.universe> <out.bin>\n";
        return 2;
    }
    const std::string inPath = argv[1];
    const std::string outPath = argv[2];

    std::string text;
    if (!Neuron::Tools::ReadFileText(inPath, text)) {
        std::cerr << "datacook: cannot read " << inPath << "\n";
        return 2;
    }

    Neuron::Sim::UniverseDataset ds;
    std::vector<std::string> errors;
    if (!Neuron::Tools::ParseUniverseSource(text, ds, errors)) {
        Neuron::Tools::PrintErrors(inPath, errors);
        return 1;
    }
    if (!Neuron::Sim::ValidateUniverseDataset(ds, errors)) {
        Neuron::Tools::PrintErrors(inPath, errors);
        return 1;
    }

    const std::vector<uint8_t> bytes = Neuron::Sim::EncodeUniverseDataset(ds);

    // Self-check: the cooked blob must decode cleanly.
    if (!Neuron::Sim::DecodeUniverseDataset(bytes)) {
        std::cerr << "datacook: internal error — cooked blob failed to decode\n";
        return 3;
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::cerr << "datacook: cannot write " << outPath << "\n";
        return 2;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    std::cout << "datacook: " << inPath << " -> " << outPath << "  ("
              << ds.regions.size() << " regions, " << ds.beacons.size() << " beacons, "
              << ds.fields.size() << " fields, " << bytes.size() << " bytes)\n";
    return 0;
}
