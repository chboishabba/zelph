/*
Copyright (c) 2026 acrion innovations GmbH

Public graph API wrappers that preserve the resident implementation for full
loads and discharge hosted routing obligations for manifest-backed partial
loads.
*/

#include "zelph.hpp"

#include <utility>
#include <vector>

namespace zelph::network
{
    uintptr_t Zelph::partial_query_instance_token() const
    {
        return reinterpret_cast<uintptr_t>(_pImpl);
    }

    bool Zelph::partial_query_certifiable() const
    {
        const auto status = partial_query_status_json();
        return status.find("\"manifest_exhaustive\":true") != std::string::npos;
    }

    Node Zelph::node(const std::string& name, std::string lang)
    {
        if (lang.empty()) lang = _lang;
        if (const Node existing = get_node_resident(name, lang)) return existing;
        if (partial_query_active())
        {
            ensure_partial_name(name, lang);
            if (const Node routed = get_node_resident(name, lang)) return routed;
        }
        return node_resident(name, std::move(lang));
    }

    Node Zelph::get_node(const std::string& name, std::string lang) const
    {
        if (lang.empty()) lang = _lang;
        if (const Node existing = get_node_resident(name, lang)) return existing;
        if (partial_query_active())
        {
            ensure_partial_name(name, lang);
            return get_node_resident(name, lang);
        }
        return 0;
    }

    std::string Zelph::get_name(const Node node, std::string lang, const bool fallback) const
    {
        if (lang.empty()) lang = _lang;
        auto value = get_name_resident(node, lang, fallback);
        if (value.empty() && partial_query_active())
        {
            ensure_partial_node_names(node);
            value = get_name_resident(node, lang, fallback);
        }
        return value;
    }

    adjacency_set Zelph::get_fact_objects(const Node subject, const Node predicate) const
    {
        if (partial_query_active()) ensure_partial_outgoing(subject, predicate, 0);
        return get_fact_objects_resident(subject, predicate);
    }

    adjacency_set Zelph::get_fact_subjects(const Node predicate, const Node object) const
    {
        if (partial_query_active()) ensure_partial_incoming(predicate, object, 0);
        return get_fact_subjects_resident(predicate, object);
    }

    adjacency_set Zelph::transitive_targets(const Node start, const Node predicate, const bool include_start) const
    {
        if (!partial_query_active()) return transitive_targets_resident(start, predicate, include_start);

        adjacency_set result;
        ankerl::unordered_dense::set<Node> seen;
        std::vector<std::pair<Node, uint64_t>> frontier{{start, 0}};
        if (include_start)
        {
            seen.insert(start);
            result.insert(start);
        }
        while (!frontier.empty())
        {
            std::vector<std::pair<Node, uint64_t>> next;
            for (const auto& [node_value, depth] : frontier)
            {
                ensure_partial_outgoing(node_value, predicate, depth);
                for (const Node target : get_fact_objects_resident(node_value, predicate))
                {
                    if (seen.insert(target).second)
                    {
                        result.insert(target);
                        next.emplace_back(target, depth + 1);
                    }
                }
            }
            frontier = std::move(next);
        }
        return result;
    }

    adjacency_set Zelph::transitive_sources(const Node target, const Node predicate, const bool include_target) const
    {
        if (!partial_query_active()) return transitive_sources_resident(target, predicate, include_target);

        adjacency_set result;
        ankerl::unordered_dense::set<Node> seen;
        std::vector<std::pair<Node, uint64_t>> frontier{{target, 0}};
        if (include_target)
        {
            seen.insert(target);
            result.insert(target);
        }
        while (!frontier.empty())
        {
            std::vector<std::pair<Node, uint64_t>> next;
            for (const auto& [node_value, depth] : frontier)
            {
                ensure_partial_incoming(predicate, node_value, depth);
                for (const Node source : get_fact_subjects_resident(predicate, node_value))
                {
                    if (seen.insert(source).second)
                    {
                        result.insert(source);
                        next.emplace_back(source, depth + 1);
                    }
                }
            }
            frontier = std::move(next);
        }
        return result;
    }
} // namespace zelph::network
