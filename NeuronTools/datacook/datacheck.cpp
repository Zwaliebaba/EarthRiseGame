// datacheck — referential-integrity gate for universe source (masterplan §12.6,
// §16). Validates one or more authored datasets WITHOUT cooking; meant to run in
// CI. Exit: 0 all clean · 1 a dataset failed · 2 usage/IO.
//
//   datacheck <in.universe> [more.universe ...]

#include "ToolCommon.h"
#include "UniverseSource.h"

#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: datacheck <in.universe> [more.universe ...]\n";
        return 2;
    }

    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];

        std::string text;
        if (!Neuron::Tools::ReadFileText(path, text)) {
            std::cerr << "datacheck: cannot read " << path << "\n";
            rc = 2;
            continue;
        }

        Neuron::Sim::UniverseDataset ds;
        std::vector<std::string> errors;
        if (!Neuron::Tools::ParseUniverseSource(text, ds, errors)) {
            Neuron::Tools::PrintErrors(path, errors); // syntax errors
            rc = 1;
            continue;
        }
        if (!Neuron::Sim::ValidateUniverseDataset(ds, errors)) {
            Neuron::Tools::PrintErrors(path, errors); // referential errors
            rc = 1;
            continue;
        }

        std::cout << "datacheck: OK  " << path << "  ("
                  << ds.regions.size() << " regions, " << ds.beacons.size() << " beacons, "
                  << ds.fields.size() << " fields)\n";
    }
    return rc;
}
