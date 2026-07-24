/*
Copyright (c) 2026 acrion innovations GmbH
*/

#include <doctest/doctest.h>

#include "network/partial_query_runtime.hpp"
#include "network/sha256.hpp"
#include "network/verified_object_store.hpp"
#include "partial_sparql.hpp"
#include "test_helpers.hpp"

#include <algorithm>
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

TEST_CASE("partial SPARQL classifier separates streaming and finalised query classes")
{
    using namespace zelph::console::partial_sparql;

    const auto positive = classify("SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }");
    CHECK(positive.query_class == QueryClass::positive_monotone);
    CHECK(positive.allowed_in_resident_slice);
    CHECK(positive.result_contract == ResultContract::sound_lower_bound);
    CHECK(std::find(positive.required_layers.begin(), positive.required_layers.end(), "directClaim") != positive.required_layers.end());

    const auto join = classify(R"(SELECT ?place WHERE {
      ?person wdt:P31 wd:Q5 .
      ?person wdt:P19 ?place .
    })");
    CHECK(join.query_class == QueryClass::positive_monotone);
    CHECK(join.allowed_with_routed_coverage);

    const auto path = classify("SELECT ?x WHERE { wd:Q5 wdt:P279* ?x . }");
    CHECK(path.query_class == QueryClass::recursive_path);
    CHECK(path.allowed_in_resident_slice);

    const auto minus = classify("SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . MINUS { ?x wdt:P570 ?d . } }");
    CHECK(minus.query_class == QueryClass::non_monotone);
    CHECK_FALSE(minus.allowed_in_resident_slice);
    CHECK(minus.allowed_with_routed_coverage);
    CHECK(minus.requires_complete_coverage);

    const auto count = classify("SELECT (COUNT(?x) AS ?n) WHERE { ?x wdt:P31 wd:Q5 . }");
    CHECK(count.query_class == QueryClass::aggregate);
    CHECK(count.result_contract == ResultContract::withheld_until_complete);

    const auto ordered = classify("SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . } ORDER BY ?x LIMIT 1");
    CHECK(ordered.query_class == QueryClass::ordered_window);
    CHECK(ordered.requires_complete_coverage);

    const auto qualifier = classify("SELECT ?s WHERE { wd:Q1 p:P39 ?s . ?s pq:P580 ?date . }");
    CHECK(std::find(qualifier.required_layers.begin(), qualifier.required_layers.end(), "statement") != qualifier.required_layers.end());
    CHECK(std::find(qualifier.required_layers.begin(), qualifier.required_layers.end(), "qualifier") != qualifier.required_layers.end());

    const auto global = classify("SELECT ?s ?o WHERE { ?s wdt:P31 ?o . }");
    CHECK(global.query_class == QueryClass::global_scan);
    CHECK_FALSE(global.allowed_with_routed_coverage);
}

TEST_CASE("progressive routing consumes a useful shard without a global barrier")
{
    using namespace zelph::network::partial_query;

    Runtime runtime(Budget{.max_shards = 4, .max_bytes = 4096});
    runtime.set_query_id("test-query");
    runtime.set_manifest_exhaustive(true);
    runtime.set_representation_coverage(true);

    RoutingObligation first;
    first.id = "person:P19";
    first.subject = 1;
    first.predicate = 19;
    first.direction = Direction::outgoing;
    CHECK(runtime.add_obligation(first, {{"people-a", 1000, false, 1}, {"people-b", 200, true, 2}}));

    RoutingObligation second;
    second.id = "place:P17";
    second.subject = 100;
    second.predicate = 17;
    second.direction = Direction::outgoing;
    CHECK(runtime.add_obligation(second, {{"people-b", 200, true, 2}}));

    const auto first_batch = runtime.next_batch(1);
    REQUIRE(first_batch.size() == 1);
    CHECK(first_batch.front() == "people-b");

    runtime.mark_in_flight("people-b");
    runtime.accept_shard("people-b", {"person:P19", "place:P17"}, 200);
    runtime.set_evaluation_fixed_point(true);

    const auto certificate = runtime.completion();
    CHECK(certificate.state == CompletionState::complete);
    CHECK(certificate.routing_fixed_point);
    CHECK(certificate.unresolved_obligations == 0);
    CHECK(certificate.relevant_unloaded_shards == 0);
    CHECK(certificate.loaded_shards == 1);
    CHECK(runtime.events().front().type == EventType::routing_obligations_added);
}

TEST_CASE("progressive routing reports resource-limited incompleteness")
{
    using namespace zelph::network::partial_query;

    Runtime runtime(Budget{.max_shards = 1, .max_bytes = 100});
    runtime.set_manifest_exhaustive(true);
    runtime.set_representation_coverage(true);
    RoutingObligation frontier;
    frontier.id = "frontier";
    frontier.subject = 5;
    frontier.predicate = 279;
    frontier.depth = 1;
    CHECK(runtime.add_obligation(frontier, {{"closure-next", 4096, false, 1}}));
    CHECK(runtime.next_batch(1).empty());
    runtime.stop("shard-or-byte-budget-exhausted");

    const auto certificate = runtime.completion();
    CHECK(certificate.state == CompletionState::incomplete_known);
    CHECK(certificate.reason == "shard-or-byte-budget-exhausted");
    CHECK(certificate.unresolved_obligations == 1);
    CHECK(certificate.relevant_unloaded_shards == 1);
}

#ifndef __EMSCRIPTEN__
TEST_CASE("verified object store rejects corruption and reuses verified content")
{
    using namespace zelph::network;
    const auto source = temporary_path(".object");
    {
        std::ofstream output(source, std::ios::binary);
        output << "verified partial object";
    }

    ObjectRequest request;
    request.dataset_version = "test-v1";
    request.uri = source.string();
    request.expected_sha256 = sha256::file(source);
    request.length = std::filesystem::file_size(source);
    request.section_hint = "test";

    VerifiedObjectStore store;
    const auto first = store.materialize(request);
    CHECK(first.sha256 == request.expected_sha256);
    CHECK(std::filesystem::is_regular_file(first.local_path));

    const auto second = store.materialize(request);
    CHECK(second.sha256 == request.expected_sha256);
    CHECK(second.cache_disposition == CacheDisposition::verified_hit);

    request.expected_sha256.assign(64, '0');
    CHECK_THROWS(store.materialize(request));
    std::error_code ignored;
    std::filesystem::remove(source, ignored);
}

TEST_CASE("partial mode imports composed SPARQL extension and preserves resident graph")
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
    CHECK(any_output_contains(collector, "Routed SPARQL join strategy loaded"));
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

    collector.clear();
    run_sparql(interactive, "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }");
    CHECK(any_output_contains(collector, "-- 2 result(s) --"));

    std::filesystem::remove(bin_path, ignored);
}

TEST_CASE("resident partial mode refuses unstable SPARQL and mutation bypasses")
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
