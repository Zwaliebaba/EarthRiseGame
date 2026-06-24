// JsonParseTests.cpp — coverage for NeuronCore/Json.h, the parser the server and
// client config loaders sit on (env-var → JSON config migration). Runs in the
// platform-independent Linux testrunner (masterplan §16.2).

#include "TestRunner.h"

#include "Json.h"

using Neuron::Json::Value;

namespace
{
Value parseOk(std::string_view text)
{
    Value v;
    std::string err;
    if (!Neuron::Json::Parse(text, v, &err))
        ::ertest::fail("expected parse to succeed but failed: " + err);
    return v;
}

bool parseFails(std::string_view text)
{
    Value v;
    std::string err;
    return !Neuron::Json::Parse(text, v, &err);
}
} // namespace

ER_TEST(Json, ParsesFlatObjectScalars)
{
    const Value v = parseOk(R"({
        "port": 7777,
        "devAuthStub": false,
        "name": "earthrise",
        "missing": null
    })");
    ER_CHECK(v.isObject());
    ER_CHECK_EQ(v.getUint32("port", 0), 7777u);
    ER_CHECK_EQ(v.getBool("devAuthStub", true), false);
    ER_CHECK_EQ(v.getString("name", "x"), std::string("earthrise"));
    ER_CHECK(v.has("missing"));
    ER_CHECK(v["missing"].isNull());
}

ER_TEST(Json, MissingKeyReturnsFallback)
{
    const Value v = parseOk(R"({"a": 1})");
    ER_CHECK_EQ(v.getUint32("nope", 42u), 42u);
    ER_CHECK_EQ(v.getString("nope", "def"), std::string("def"));
    ER_CHECK(!v.has("nope"));
    ER_CHECK(v["nope"].isNull()); // chaining off a missing key is safe
}

ER_TEST(Json, NestedObjectChaining)
{
    const Value v = parseOk(R"({
        "database": { "user": "er_app", "password": "S3cret!" },
        "pool": { "min": 2, "max": 8 }
    })");
    ER_CHECK_EQ(v["database"].getString("user", ""), std::string("er_app"));
    ER_CHECK_EQ(v["database"].getString("password", ""), std::string("S3cret!"));
    ER_CHECK_EQ(v["pool"].getInt("min", 0), 2);
    ER_CHECK_EQ(v["pool"].getInt("max", 0), 8);
    // Reading a sub-object that does not exist yields a Null value, not a crash.
    ER_CHECK(v["auth"].isNull());
    ER_CHECK_EQ(v["auth"].getString("pepper", "fallback"), std::string("fallback"));
}

ER_TEST(Json, Arrays)
{
    const Value v = parseOk(R"({ "hosts": ["127.0.0.1", "10.0.0.2"], "ports": [1, 2, 3] })");
    const Value& hosts = v["hosts"];
    ER_CHECK(hosts.isArray());
    ER_CHECK_EQ(hosts.size(), size_t(2));
    ER_CHECK_EQ(hosts.at(0).asString(), std::string("127.0.0.1"));
    ER_CHECK_EQ(hosts.at(1).asString(), std::string("10.0.0.2"));
    ER_CHECK_EQ(v["ports"].size(), size_t(3));
    ER_CHECK_EQ(v["ports"].at(2).asInt(), int64_t(3));
    ER_CHECK(v["ports"].at(99).isNull()); // out-of-range index is safe
}

ER_TEST(Json, NumberForms)
{
    const Value v = parseOk(R"({ "neg": -5, "frac": 3.5, "exp": 2.1e3, "big": 210000 })");
    ER_CHECK_EQ(v["neg"].asInt(), int64_t(-5));
    ER_CHECK(v["frac"].asNumber() > 3.49 && v["frac"].asNumber() < 3.51);
    ER_CHECK_EQ(v["exp"].asInt(), int64_t(2100));
    ER_CHECK_EQ(v["big"].asUint(), uint64_t(210000));
}

ER_TEST(Json, StringEscapes)
{
    const Value v = parseOk(R"({ "s": "a\tb\n\"c\"\\dAé" })");
    // A = 'A'; é = 'é' (UTF-8 0xC3 0xA9)
    const std::string expected = std::string("a\tb\n\"c\"\\dA") + "\xc3\xa9";
    ER_CHECK_EQ(v["s"].asString(), expected);
}

ER_TEST(Json, SurrogatePair)
{
    // U+1F680 ROCKET, encoded as a UTF-16 surrogate pair.
    const Value v = parseOk(R"({ "rocket": "🚀" })");
    const std::string expected = "\xF0\x9F\x9A\x80";
    ER_CHECK_EQ(v["rocket"].asString(), expected);
}

ER_TEST(Json, WrongTypeYieldsFallback)
{
    const Value v = parseOk(R"({ "port": "not-a-number", "flag": 3 })");
    // Type mismatch falls back rather than throwing or coercing.
    ER_CHECK_EQ(v.getUint32("port", 9000u), 9000u);
    ER_CHECK_EQ(v.getBool("flag", true), true);
}

ER_TEST(Json, TopLevelArrayAndScalar)
{
    ER_CHECK(parseOk("[1, 2, 3]").isArray());
    ER_CHECK(parseOk("true").asBool());
    ER_CHECK_EQ(parseOk("  42  ").asInt(), int64_t(42));
    ER_CHECK(parseOk("\"hi\"").asString() == std::string("hi"));
}

ER_TEST(Json, RejectsMalformed)
{
    ER_CHECK(parseFails("{"));
    ER_CHECK(parseFails("{\"a\": }"));
    ER_CHECK(parseFails("{\"a\": 1,}"));   // trailing comma
    ER_CHECK(parseFails("[1 2]"));         // missing comma
    ER_CHECK(parseFails("{\"a\" 1}"));     // missing colon
    ER_CHECK(parseFails("nul"));
    ER_CHECK(parseFails("\"unterminated"));
    ER_CHECK(parseFails("1 2"));           // trailing content after top-level value
    ER_CHECK(parseFails(""));              // empty input
}

ER_TEST(Json, EmptyContainers)
{
    ER_CHECK(parseOk("{}").isObject());
    ER_CHECK_EQ(parseOk("{}").size(), size_t(0)); // size() is array-only → 0
    ER_CHECK(parseOk("[]").isArray());
    ER_CHECK_EQ(parseOk("[]").size(), size_t(0));
}
