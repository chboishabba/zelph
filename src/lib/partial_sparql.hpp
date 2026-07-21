/*
Copyright (c) 2026 acrion innovations GmbH

Partial-graph SPARQL semantic classification and graph-preservation support.
*/
#pragma once

#include "network/reasoning.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace zelph::console::partial_sparql
{
    enum class QueryClass
    {
        positive_monotone,
        recursive_path,
        non_monotone,
        aggregate,
        ordered_window,
        global_scan,
        representation_unknown,
    };

    enum class ResultContract
    {
        sound_lower_bound,
        potentially_unstable,
        complete_under_coverage_certificate,
    };

    enum class CoverageState
    {
        complete,
        incomplete,
        unknown,
    };

    struct QueryAssessment
    {
        QueryClass      query_class     = QueryClass::positive_monotone;
        ResultContract  result_contract = ResultContract::sound_lower_bound;
        CoverageState   coverage        = CoverageState::unknown;
        bool            allowed_in_resident_slice = true;
        bool            recursive_path           = false;
        bool            requires_qualifier_layer = false;
        bool            has_non_monotone_operator = false;
        bool            has_aggregate             = false;
        bool            has_order_or_limit        = false;
        bool            has_global_scan           = false;
        std::string     reason;
    };

    inline const char* query_class_name(const QueryClass value)
    {
        switch (value)
        {
        case QueryClass::positive_monotone:
            return "positive-monotone";
        case QueryClass::recursive_path:
            return "recursive-path";
        case QueryClass::non_monotone:
            return "non-monotone";
        case QueryClass::aggregate:
            return "aggregate";
        case QueryClass::ordered_window:
            return "ordered-window";
        case QueryClass::global_scan:
            return "global-scan";
        case QueryClass::representation_unknown:
            return "representation-coverage-unknown";
        }
        return "unknown";
    }

    inline const char* result_contract_name(const ResultContract value)
    {
        switch (value)
        {
        case ResultContract::sound_lower_bound:
            return "sound-lower-bound";
        case ResultContract::potentially_unstable:
            return "potentially-unstable";
        case ResultContract::complete_under_coverage_certificate:
            return "complete-under-coverage-certificate";
        }
        return "unknown";
    }

    inline std::string lexical_upper(const std::string& query)
    {
        std::string out;
        out.reserve(query.size());
        bool in_single = false;
        bool in_double = false;
        bool in_iri    = false;
        bool in_comment = false;
        bool escaped = false;

        for (const unsigned char raw : query)
        {
            const char c = static_cast<char>(raw);
            if (in_comment)
            {
                if (c == '\n')
                {
                    in_comment = false;
                    out.push_back('\n');
                }
                else
                {
                    out.push_back(' ');
                }
                continue;
            }
            if (!in_single && !in_double && !in_iri && c == '#')
            {
                in_comment = true;
                out.push_back(' ');
                continue;
            }
            if (escaped)
            {
                escaped = false;
                out.push_back(' ');
                continue;
            }
            if ((in_single || in_double) && c == '\\')
            {
                escaped = true;
                out.push_back(' ');
                continue;
            }
            if (!in_double && !in_iri && c == '\'')
            {
                in_single = !in_single;
                out.push_back(' ');
                continue;
            }
            if (!in_single && !in_iri && c == '"')
            {
                in_double = !in_double;
                out.push_back(' ');
                continue;
            }
            if (!in_single && !in_double && c == '<')
            {
                in_iri = true;
                out.push_back(' ');
                continue;
            }
            if (in_iri && c == '>')
            {
                in_iri = false;
                out.push_back(' ');
                continue;
            }
            if (in_single || in_double || in_iri)
            {
                out.push_back(' ');
                continue;
            }
            out.push_back(static_cast<char>(std::toupper(raw)));
        }
        return out;
    }

    inline bool contains_word(const std::string& text, const std::string& word)
    {
        size_t pos = text.find(word);
        while (pos != std::string::npos)
        {
            const bool left_ok = pos == 0 || !(std::isalnum(static_cast<unsigned char>(text[pos - 1])) || text[pos - 1] == '_' || text[pos - 1] == '-');
            const size_t end = pos + word.size();
            const bool right_ok = end >= text.size() || !(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' || text[end] == '-');
            if (left_ok && right_ok) return true;
            pos = text.find(word, pos + 1);
        }
        return false;
    }

    inline QueryAssessment classify(const std::string& query)
    {
        const std::string upper = lexical_upper(query);
        QueryAssessment assessment;

        assessment.has_non_monotone_operator = contains_word(upper, "MINUS")
                                             || contains_word(upper, "OPTIONAL")
                                             || upper.find("NOT EXISTS") != std::string::npos;
        assessment.has_aggregate = contains_word(upper, "COUNT")
                                || upper.find("GROUP BY") != std::string::npos;
        assessment.has_order_or_limit = upper.find("ORDER BY") != std::string::npos
                                     || contains_word(upper, "LIMIT")
                                     || contains_word(upper, "OFFSET");
        assessment.recursive_path = std::regex_search(upper, std::regex(R"(((?:[A-Z][A-Z0-9_-]*:)?[A-Z][A-Z0-9_-]*[+*])(?=\s|/|\?|\.|\}))"));
        assessment.requires_qualifier_layer = std::regex_search(upper, std::regex(R"((^|[^A-Z0-9_-])(P|PS|PQ|PR):)"))
                                           || upper.find("WIKIBASE:RANK") != std::string::npos;

        // The first graph pattern must be anchorable by a concrete subject or
        // object. Later patterns may have two syntactic variables because an
        // earlier pattern can bind one of them before the join is evaluated.
        const std::regex unanchored_initial(
            R"(\{\s*(\?[A-Z_][A-Z0-9_-]*)\s+([^\s{}]+)\s+(\?[A-Z_][A-Z0-9_-]*))");
        assessment.has_global_scan = std::regex_search(upper, unanchored_initial);

        if (assessment.requires_qualifier_layer)
        {
            assessment.query_class = QueryClass::representation_unknown;
            assessment.result_contract = ResultContract::potentially_unstable;
            assessment.allowed_in_resident_slice = false;
            assessment.reason = "The loaded manifest does not yet certify p:/ps:/pq:/pr:/rank representation coverage.";
        }
        else if (assessment.has_non_monotone_operator)
        {
            assessment.query_class = QueryClass::non_monotone;
            assessment.result_contract = ResultContract::potentially_unstable;
            assessment.allowed_in_resident_slice = false;
            assessment.reason = "OPTIONAL, MINUS, and absence-sensitive operators require complete routed coverage before finalisation.";
        }
        else if (assessment.has_aggregate)
        {
            assessment.query_class = QueryClass::aggregate;
            assessment.result_contract = ResultContract::potentially_unstable;
            assessment.allowed_in_resident_slice = false;
            assessment.reason = "Aggregates are not exact until every relevant routing obligation is discharged.";
        }
        else if (assessment.has_order_or_limit)
        {
            assessment.query_class = QueryClass::ordered_window;
            assessment.result_contract = ResultContract::potentially_unstable;
            assessment.allowed_in_resident_slice = false;
            assessment.reason = "ORDER BY/LIMIT/OFFSET results are unstable while unseen shards may contain superior rows.";
        }
        else if (assessment.has_global_scan)
        {
            assessment.query_class = QueryClass::global_scan;
            assessment.result_contract = ResultContract::sound_lower_bound;
            assessment.allowed_in_resident_slice = false;
            assessment.reason = "The query has an unanchored triple pattern; completeness requires an exhaustive predicate/global index.";
        }
        else if (assessment.recursive_path)
        {
            assessment.query_class = QueryClass::recursive_path;
            assessment.result_contract = ResultContract::sound_lower_bound;
            assessment.reason = "Returned path rows are valid over the resident graph, but routed closure has not yet been certified.";
        }
        else
        {
            assessment.query_class = QueryClass::positive_monotone;
            assessment.result_contract = ResultContract::sound_lower_bound;
            assessment.reason = "Returned rows remain valid under graph extension; additional rows may exist.";
        }
        return assessment;
    }

    struct GraphFingerprint
    {
        uint64_t hash       = 1469598103934665603ULL;
        uint64_t nodes      = 0;
        uint64_t left_edges = 0;
        uint64_t right_edges = 0;
        uint64_t names      = 0;
        uint64_t rules      = 0;

        friend bool operator==(const GraphFingerprint&, const GraphFingerprint&) = default;
    };

    inline void hash_bytes(uint64_t& hash, const void* data, const size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    }

    template <typename T>
    inline void hash_value(uint64_t& hash, const T& value)
    {
        hash_bytes(hash, &value, sizeof(value));
    }

    inline void hash_string(uint64_t& hash, const std::string& value)
    {
        hash_bytes(hash, value.data(), value.size());
        const unsigned char separator = 0xff;
        hash_bytes(hash, &separator, 1);
    }

    inline GraphFingerprint fingerprint(const network::Reasoning& graph)
    {
        GraphFingerprint result;
        std::vector<network::Node> nodes;
        const auto view = graph.get_all_nodes_view();
        for (auto it = view.begin(); it != view.end(); ++it)
            nodes.push_back(it->first);
        std::sort(nodes.begin(), nodes.end());
        result.nodes = nodes.size();

        for (const auto node : nodes)
        {
            hash_value(result.hash, node);
            auto left = graph.get_left(node);
            std::vector<network::Node> left_sorted(left.begin(), left.end());
            std::sort(left_sorted.begin(), left_sorted.end());
            result.left_edges += left_sorted.size();
            for (const auto neighbor : left_sorted)
            {
                hash_value(result.hash, neighbor);
                const double weight = graph.edge_weight(neighbor, node, 1.0);
                uint64_t bits = 0;
                static_assert(sizeof(bits) == sizeof(weight));
                std::memcpy(&bits, &weight, sizeof(bits));
                hash_value(result.hash, bits);
            }

            auto right = graph.get_right(node);
            std::vector<network::Node> right_sorted(right.begin(), right.end());
            std::sort(right_sorted.begin(), right_sorted.end());
            result.right_edges += right_sorted.size();
            for (const auto neighbor : right_sorted)
            {
                hash_value(result.hash, neighbor);
                const double weight = graph.edge_weight(node, neighbor, 1.0);
                uint64_t bits = 0;
                std::memcpy(&bits, &weight, sizeof(bits));
                hash_value(result.hash, bits);
            }
        }

        auto languages = graph.get_languages();
        std::sort(languages.begin(), languages.end());
        for (const auto& language : languages)
        {
            hash_string(result.hash, language);
            for (const auto node : nodes)
            {
                const auto name = graph.get_name(node, language, false);
                if (!name.empty())
                {
                    ++result.names;
                    hash_value(result.hash, node);
                    hash_string(result.hash, name);
                }
            }
        }
        result.rules = graph.rule_count();
        hash_value(result.hash, result.rules);
        return result;
    }

    inline std::string describe(const GraphFingerprint& value)
    {
        std::ostringstream stream;
        stream << "hash=" << value.hash
               << ", nodes=" << value.nodes
               << ", left_edges=" << value.left_edges
               << ", right_edges=" << value.right_edges
               << ", names=" << value.names
               << ", rules=" << value.rules;
        return stream.str();
    }
} // namespace zelph::console::partial_sparql
