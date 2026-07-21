/*
Copyright (c) 2026 acrion innovations GmbH
*/

#include <doctest/doctest.h>

#include "import_policy.hpp"
#include "network/partial_query_runtime.hpp"
#include "partial_sparql.hpp"
#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace zelph::test;

namespace
{
    void run_sparql(const zelph::console::Interactive& interactive, const std::string& query)
    {
        interactive.process("sparql");
        std::istringstream input(query);
        std::string line;
        while (std::getline(input, line)) interactive.process(line);
        interactive.process("");
    }

    std::filesystem::path temporary_path(const std::string& suffix)
    {
        static uint64_t serial = 0;
        return std::filesystem::temp_directory_path()
             / ("zelph-partial-sparql-" + std::to_string(++serial) + suffix);
    }

    void create_partial_fixture(zelph::console::Interactive& interactive, const std::filesystem::path& path)
    {
        process_lines(interactive, R"(
.lang wikidata
Q1 P31 Q5
Q2 P31 Q5
Q1 P19 Q100
Q2 P19 Q200
Q100 P17 Q408
Q200 P17 Q30
Q5 P279 Q50
Q50 P279 Q500
)");
        interactive.process(".save " + path.string());
        interactive.process(".new");
        interactive.process(".load-partial " + path.string());
    }
}

TEST_CASE("partial SPARQL classifier separates stable and unstable query classes")
{
    using namespace zelph::console::partial_sparql;

    auto positive = classify("SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }");
    CHECK(positive.query_class == QueryClass::positive_monotone);
    CHECK(positive.allowed_in_resident_slice);
    CHECK(positive.result_contract == ResultContract::sound_lower_bound);

    auto join = classify(R"(SELECT ?place WHERE {
      ?person wdt:P31 wd:Q5 .
      ?person wdt:P19 ?place .
    })");
    CHECK(join.query_class == QueryClass::positive_monotone);
    CHECK(join.allowed_in_resident_slice);

    auto path = classify("SELECT ?x WHERE { wd:Q5 wdt:P279* ?x . }");
    CHECK(path.query_class == QueryClass::recursive_path);
    CHECK(path.allowed_in_resident_slice);

    auto minus = classify("SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . MINUS { ?x wdt:P570 ?d . } }");
    CHECK(minus.query_class == QueryClass::non_monotone);
    CHECK_FALSE(minus.allowed_in_resident_slice);

    auto count = classify("SELECT (COUNT(?x) AS ?n) WHERE { ?x wdt:P31 wd:Q5 . }");
    CHECK(count.query_class == QueryClass::aggregate);
    CHECK_FALSE(count.allowed_in_resident_slice);

    auto ordered = classify("SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . } ORDER BY ?x LIMIT 1");
    CHECK(ordered.query_class == QueryClass::ordered_window);
    CHECK_FALSE(ordered.allowed_in_resident_slice);

    auto qualifier = classify("SELECT ?s WHERE { wd:Q1 p:P39 ?s . ?s pq:P580 ?date . }");
    CHECK(qualifier.query_class == QueryClass::representation_unknown);
    CHECK_FALSE(qualifier.allowed_in_resident_slice);

    auto global = classify("SELECT ?s ?o WHERE { ?s wdt:P31 ?o . }");
    CHECK(global.query_class == QueryClass::global_scan);
    CHECK_FALSE(global.allowed_in_resident_slice);

    auto projection_star = classify("SELECT * WHERE { ?x wdt:P31 wd:Q5 . }");
    CHECK(projection_star.query_class == QueryClass::positive_monotone);
}

TEST_CASE("progressive routing consumes useful shards without a global barrier")
{
    using namespace zelph::network::partial_query;

    Runtime runtime;
    runtime.set_manifest_exhaustive(true);
    runtime.set_representation_coverage(true);

    runtime.add_obligation({"person:P19", 1, 19, Direction::outgoing, RepresentationLayer::direct_claim},
                           {{"people-a", 1000, false, 1}, {"people-b", 200, true, 1}});
    runtime.add_obligation({"place:P17", 100, 17, Direction::outgoing, RepresentationLayer::direct_claim},
                           {{"people-b", 200, true, 2}});

    const auto first = runtime.next_batch({1, 1000});
    REQUIRE(first.size() == 1);
    CHECK(first.front() == "people-b");

    runtime.mark_in_flight("people-b");
    runtime.accept_shard("people-b", {"person:P19", "place:P17"});
    runtime.set_evaluation_fixed_point(true);

    const auto certificate = runtime.completion();
    CHECK(certificate.state == CompletionState::complete);
    CHECK(certificate.routing_fixed_point);
    CHECK(certificate.unresolved_obligations == 0);
    CHECK(certificate.relevant_unloaded_shards == 0);
    CHECK(certificate.loaded_shards == 1);
}

TEST_CASE("progressive routing reports resource-limited incompleteness")
{
    using namespace zelph::network::partial_query;

    Runtime runtime;
    runtime.set_manifest_exhaustive(true);
    runtime.set_representation_coverage(true);
    runtime.add_obligation({"frontier", 5, 279, Direction::outgoing, RepresentationLayer::direct_claim},
                           {{"closure-next", 4096, false, 1}});
    runtime.stop("shard byte budget exhausted");

    const auto certificate = runtime.completion();
    CHECK(certificate.state == CompletionState::incomplete_known);
    CHECK(certificate.reason == "shard byte budget exhausted");
    CHECK(certificate.unresolved_obligations == 1);
    CHECK(certificate.relevant_unloaded_shards == 1);
}

#ifndef __EMSCRIPTEN__
TEST_CASE("partial mode imports trusted SPARQL and preserves the extensional graph")
{
    const auto bin_path = temporary_path(".bin");
    std::error_code ignored;
    std::filesystem::remove(bin_path, ignored);

    zelph::io::OutputCollector collector;
    zelph::console::Interactive interactive(collector.sink());
    create_partial_fixture(interactive, bin_path);
    collector.clear();

    interactive.process(".import sparql");
    CHECK(any_output_contains(collector, "SPARQL subset loaded"));
    CHECK(any_event_contains(collector, "Imported partial-safe language extension"));

    collector.clear();
    run_sparql(interactive, R"(SELECT ?place WHERE {
      ?person wdt:P31 wd:Q5 .
      ?person wdt:P19 ?place .
    })");
    CHECK(any_output_contains(collector, "Q100"));
    CHECK(any_output_contains(collector, "Q200"));
    CHECK(any_output_contains(collector, "dataset_mode: partial"));
    CHECK(any_output_contains(collector, "row_contract: sound-lower-bound"));
    CHECK(any_output_contains(collector, "result_set_status: incomplete"));
    CHECK(any_output_contains(collector, "ephemeral_graph_nodes_rolled_back:"));

    collector.clear();
    run_sparql(interactive, "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }");
    CHECK(any_output_contains(collector, "-- 2 result(s) --"));

    std::filesystem::remove(bin_path, ignored);
}

TEST_CASE("partial mode refuses unstable SPARQL and mutation bypasses")
{
    const auto bin_path = temporary_path(".bin");
    const auto script_path = temporary_path(".zph");
    std::error_code ignored;

    zelph::io::OutputCollector collector;
    zelph::console::Interactive interactive(collector.sink());
    create_partial_fixture(interactive, bin_path);
    interactive.process(".import sparql");

    CHECK_THROWS(run_sparql(interactive,
                           "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . MINUS { ?x wdt:P570 ?d . } }"));
    CHECK_THROWS(run_sparql(interactive,
                           "SELECT (COUNT(?x) AS ?n) WHERE { ?x wdt:P31 wd:Q5 . }"));
    CHECK_THROWS(interactive.process("%(zelph/fact \"Q9\" \"P31\" \"Q5\")"));
    CHECK_THROWS(interactive.process("Q9 P31 Q5"));
    CHECK_THROWS(interactive.process(".auto-run"));

    {
        std::ofstream script(script_path);
        script << "# zelph-import-capability: language-extension\n"
               << "%\n"
               << "(zelph/register-keyword \"unsafe-test\" (fn [text] text))\n"
               << "%\n";
    }
    CHECK_THROWS(interactive.process(".import " + script_path.string()));

    std::filesystem::remove(bin_path, ignored);
    std::filesystem::remove(script_path, ignored);
}
#endif
