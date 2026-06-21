#pragma once
// UniverseSource.h — text → UniverseDataset parser for the universe layout
// (docs/design/universe-worldgen.md §4). Build-time only: lives in NeuronTools
// (a leaf), shared by `datacook`, `datacheck`, and the Linux testrunner. The
// runtime (NeuronCore) never parses text — it loads the cooked binary.
//
// Grammar (whitespace-insensitive; `#` starts a comment to end-of-line):
//
//   region NAME {
//     security   = high | low | null
//     bounds     = x0 x1 y0 y1 z0 z1        # inclusive SectorId ranges
//     yield_mult = <float>
//   }
//   beacon NAME {
//     region = REGION_NAME
//     pos    = x y z                        # UniversePos, int64 metres
//     links  = BEACON_NAME ...              # reciprocal jump edges (may be empty)
//     kind   = public | claimable
//   }
//   field NAME {
//     region  = REGION_NAME
//     center  = x y z
//     radius  = <float>
//     nodes   = Ore:0.6 Ice:0.3 Gas:0.1     # composition weights (sum ≈ 1.0)
//     count   = min max
//     yield   = min max
//     respawn = <seconds>
//   }
//
// `{`, `}`, `=` are tokens; everything else is a bare token (so single-line and
// multi-line layouts both parse). Reports *syntax* errors (line-numbered);
// semantic/referential rules are ValidateUniverseDataset's job.

#include "UniverseData.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace Neuron::Tools
{

namespace detail
{
    struct Token { std::string text; size_t line; };

    inline std::vector<Token> Lex(std::string_view s)
    {
        std::vector<Token> out;
        size_t line = 1, i = 0;
        auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\n')              { ++line; ++i; continue; }
            if (isSpace(c))             { ++i; continue; }
            if (c == '#')               { while (i < s.size() && s[i] != '\n') ++i; continue; }
            if (c == '{' || c == '}' || c == '=') { out.push_back({ std::string(1, c), line }); ++i; continue; }
            const size_t start = i;
            while (i < s.size()) {
                const char d = s[i];
                if (d == '\n' || isSpace(d) || d == '#' || d == '{' || d == '}' || d == '=') break;
                ++i;
            }
            out.push_back({ std::string(s.substr(start, i - start)), line });
        }
        return out;
    }

    inline bool ParseI64(const std::string& t, int64_t& out)
    {
        char* end = nullptr;
        const long long v = std::strtoll(t.c_str(), &end, 10);
        if (end == t.c_str() || *end != '\0') return false;
        out = static_cast<int64_t>(v);
        return true;
    }
    inline bool ParseFloat(const std::string& t, float& out)
    {
        char* end = nullptr;
        const float v = std::strtof(t.c_str(), &end);
        if (end == t.c_str() || *end != '\0') return false;
        out = v;
        return true;
    }
} // namespace detail

// Parse `text` into `out`. Returns true iff there were no *syntax* errors;
// appends "line N: ..." messages to `errors` otherwise.
[[nodiscard]] inline bool ParseUniverseSource(std::string_view text,
                                              Neuron::Sim::UniverseDataset& out,
                                              std::vector<std::string>& errors)
{
    using namespace Neuron::Sim;
    const std::vector<detail::Token> toks = detail::Lex(text);
    const size_t before = errors.size();

    size_t i = 0;
    auto err  = [&](size_t line, const std::string& m) { errors.push_back("line " + std::to_string(line) + ": " + m); };
    auto peek = [&](size_t off = 0) -> const detail::Token* { return (i + off < toks.size()) ? &toks[i + off] : nullptr; };

    // Apply one `key = values` assignment to whichever block is open.
    auto apply = [&](std::string_view kind, const std::string& key, size_t line,
                     const std::vector<std::string>& v,
                     RegionDef& region, BeaconDef& beacon, ResourceFieldDef& field)
    {
        auto needN = [&](size_t n, const char* what) -> bool {
            if (v.size() != n) { err(line, "'" + key + "' expects " + std::to_string(n) + " " + what); return false; }
            return true;
        };
        if (kind == "region") {
            if (key == "security") {
                if (!needN(1, "value")) return;
                if      (v[0] == "high") region.security = SecurityTier::High;
                else if (v[0] == "low")  region.security = SecurityTier::Low;
                else if (v[0] == "null") region.security = SecurityTier::Null;
                else err(line, "unknown security tier '" + v[0] + "'");
            } else if (key == "bounds") {
                if (!needN(6, "integers")) return;
                int64_t b[6]; bool ok = true;
                for (int k = 0; k < 6; ++k) ok = detail::ParseI64(v[static_cast<size_t>(k)], b[k]) && ok;
                if (ok) region.bounds = { b[0], b[1], b[2], b[3], b[4], b[5] };
                else err(line, "bounds has a non-integer value");
            } else if (key == "yield_mult") {
                if (needN(1, "float") && !detail::ParseFloat(v[0], region.yieldMult)) err(line, "yield_mult is not a number");
            } else err(line, "unknown region key '" + key + "'");
        }
        else if (kind == "beacon") {
            if (key == "region") {
                if (needN(1, "name")) beacon.region = v[0];
            } else if (key == "pos") {
                if (!needN(3, "integers")) return;
                int64_t p[3]; bool ok = true;
                for (int k = 0; k < 3; ++k) ok = detail::ParseI64(v[static_cast<size_t>(k)], p[k]) && ok;
                if (ok) beacon.pos = { p[0], p[1], p[2] }; else err(line, "pos has a non-integer value");
            } else if (key == "links") {
                for (const auto& l : v) beacon.links.push_back(l); // may be empty
            } else if (key == "kind") {
                if (!needN(1, "value")) return;
                if      (v[0] == "public")    beacon.kind = BeaconKind::Public;
                else if (v[0] == "claimable") beacon.kind = BeaconKind::Claimable;
                else err(line, "unknown beacon kind '" + v[0] + "'");
            } else err(line, "unknown beacon key '" + key + "'");
        }
        else { // field
            if (key == "region") {
                if (needN(1, "name")) field.region = v[0];
            } else if (key == "center") {
                if (!needN(3, "integers")) return;
                int64_t c[3]; bool ok = true;
                for (int k = 0; k < 3; ++k) ok = detail::ParseI64(v[static_cast<size_t>(k)], c[k]) && ok;
                if (ok) field.center = { c[0], c[1], c[2] }; else err(line, "center has a non-integer value");
            } else if (key == "radius") {
                if (needN(1, "float") && !detail::ParseFloat(v[0], field.radius)) err(line, "radius is not a number");
            } else if (key == "nodes") {
                for (const auto& n : v) {
                    const size_t colon = n.find(':');
                    if (colon == std::string::npos) { err(line, "node '" + n + "' must be Type:weight"); continue; }
                    const std::string type = n.substr(0, colon);
                    ResourceWeight w;
                    if      (type == "Ore") w.type = ResourceType::Ore;
                    else if (type == "Ice") w.type = ResourceType::Ice;
                    else if (type == "Gas") w.type = ResourceType::Gas;
                    else { err(line, "unknown resource type '" + type + "'"); continue; }
                    if (!detail::ParseFloat(n.substr(colon + 1), w.weight)) { err(line, "node '" + n + "' has a bad weight"); continue; }
                    field.nodes.push_back(w);
                }
            } else if (key == "count") {
                if (!needN(2, "integers")) return;
                int64_t c[2];
                if (detail::ParseI64(v[0], c[0]) && detail::ParseI64(v[1], c[1])) {
                    field.countMin = static_cast<uint16_t>(c[0]); field.countMax = static_cast<uint16_t>(c[1]);
                } else err(line, "count has a non-integer value");
            } else if (key == "yield") {
                if (!needN(2, "integers")) return;
                int64_t y[2];
                if (detail::ParseI64(v[0], y[0]) && detail::ParseI64(v[1], y[1])) {
                    field.yieldMin = static_cast<int32_t>(y[0]); field.yieldMax = static_cast<int32_t>(y[1]);
                } else err(line, "yield has a non-integer value");
            } else if (key == "respawn") {
                int64_t r;
                if (needN(1, "integer") && detail::ParseI64(v[0], r)) field.respawnSeconds = static_cast<uint32_t>(r);
                else if (v.size() == 1) err(line, "respawn is not an integer");
            } else err(line, "unknown field key '" + key + "'");
        }
    };

    while (i < toks.size()) {
        const std::string kind = toks[i].text;
        const size_t kindLine = toks[i].line;
        if (kind != "region" && kind != "beacon" && kind != "field") {
            err(kindLine, "expected 'region|beacon|field', got '" + kind + "'");
            ++i; continue;
        }
        ++i;
        const detail::Token* nameTok = peek();
        if (!nameTok || nameTok->text == "{" || nameTok->text == "}" || nameTok->text == "=") {
            err(kindLine, kind + " is missing a name"); continue;
        }
        const std::string name = nameTok->text;
        ++i;
        const detail::Token* brace = peek();
        if (!brace || brace->text != "{") { err(nameTok->line, "expected '{' after " + kind + " " + name); continue; }
        ++i;

        RegionDef region; BeaconDef beacon; ResourceFieldDef field;
        if      (kind == "region") region.name = name;
        else if (kind == "beacon") beacon.name = name;
        else                       field.name  = name;

        bool closed = false;
        while (i < toks.size()) {
            if (toks[i].text == "}") { ++i; closed = true; break; }
            const std::string key = toks[i].text;
            const size_t keyLine = toks[i].line;
            if (key == "{" || key == "=") { err(keyLine, "expected a key or '}'"); ++i; continue; }
            ++i;
            const detail::Token* eq = peek();
            if (!eq || eq->text != "=") { err(keyLine, "expected '=' after key '" + key + "'"); continue; }
            ++i;
            std::vector<std::string> values;
            while (i < toks.size()) {
                if (toks[i].text == "}") break;
                const detail::Token* nxt = peek(1);
                if (nxt && nxt->text == "=") break; // current token starts the next assignment
                values.push_back(toks[i].text);
                ++i;
            }
            apply(kind, key, keyLine, values, region, beacon, field);
        }

        if (!closed) { err(kindLine, kind + " " + name + " is missing a closing '}'"); continue; }
        if      (kind == "region") out.regions.push_back(std::move(region));
        else if (kind == "beacon") out.beacons.push_back(std::move(beacon));
        else                       out.fields.push_back(std::move(field));
    }

    return errors.size() == before;
}

} // namespace Neuron::Tools
