/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch

This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org

zelph is offered under a commercial and under the AGPL license.
For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.

AGPL licensing:

zelph is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zelph is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with zelph. If not, see <https://www.gnu.org/licenses/>.
*/

#include "zelph.hpp"

#include "partial_query_manifest.hpp"
#include "verified_object_store.hpp"
#include "zelph_impl.hpp"

#include <filesystem>

using namespace zelph::network;

namespace
{
#ifndef __EMSCRIPTEN__
    std::string resolve_contract_uri(const PartialQueryManifest& manifest, const std::string& uri)
    {
        if (uri.empty() || detail::is_hf_uri(uri) || uri.rfind("file://", 0) == 0
            || std::filesystem::path(uri).is_absolute()) return uri;
        return (std::filesystem::path(manifest.local_path()).parent_path() / uri).string();
    }

    void restore_cluster(const Zelph& graph, const std::string& previous)
    {
        if (previous.empty() || previous == "default") graph.deactivate_cluster();
        else graph.set_active_cluster(previous);
    }
#endif
}

void Zelph::cleanup_isolated(size_t& removed_count) const
{
    removed_count = 0;
    invalidate_fact_structures_cache();
    _pImpl->remove_isolated_nodes(removed_count);
}

size_t Zelph::cleanup_names() const { return _pImpl->cleanup_dangling_names(); }

void Zelph::remove_node(Node node) const
{
    if (!_pImpl->exists(node)) throw std::runtime_error("Cannot remove non-existent node " + std::to_string(node));
    invalidate_fact_structures_cache();
    _pImpl->remove(node);
    _pImpl->remove_node_names(node);
}

adjacency_set Zelph::get_rules() const
{
    const adjacency_set& rule_candidates = _pImpl->get_left(core.Causes);
    adjacency_set rules;
    for (Node rule_candidate : rule_candidates)
    {
        if (rule_candidate)
        {
            adjacency_set deductions;
            Node condition = parse_fact(rule_candidate, deductions);
            if (condition && condition != core.Causes && !deductions.empty()) rules.insert(rule_candidate);
        }
    }
    return rules;
}

void Zelph::remove_rules() const
{
    adjacency_set rules = get_rules();
    for (Node rule : rules)
    {
        invalidate_fact_structures_cache();
        _pImpl->remove(rule);
        for (auto& lang_map : _pImpl->_name_of_node) lang_map.second.erase(rule);
        for (auto& lang_map : _pImpl->_node_of_name)
        {
            for (auto it = lang_map.second.begin(); it != lang_map.second.end();)
            {
                if (it->second == rule) it = lang_map.second.erase(it);
                else ++it;
            }
        }
    }
}

size_t Zelph::rule_count() const { return get_rules().size(); }

#ifndef __EMSCRIPTEN__
void Zelph::save_to_file(const std::string& filename) const { _pImpl->saveToFile(filename); }

void Zelph::load_from_file(const std::string& filename) const
{
    clear_partial_query_session();
    invalidate_fact_structures_cache();
    _pImpl->loadFromFile(filename);
}

void Zelph::load_from_file(const std::string& filename,
                           const BinChunkSelection& selection,
                           const bool skip_payload) const
{
    clear_partial_query_session();
    invalidate_fact_structures_cache();
    _pImpl->loadFromFile(filename, selection, skip_payload);
}

void Zelph::load_from_manifest(const std::string& manifest_path,
                               const BinChunkSelection& selection,
                               const std::string& shard_root,
                               const std::string& bin_path_override,
                               const bool skip_payload) const
{
    clear_partial_query_session();
    invalidate_fact_structures_cache();

    const auto contract = PartialQueryManifest::load(manifest_path, shard_root);
    std::string effective_bin_override = bin_path_override;
    if (contract.canonical())
    {
        VerifiedObjectStore objects;
        ObjectRequest request;
        request.dataset_version = contract.dataset_version();
        request.uri = resolve_contract_uri(contract, contract.source_uri());
        request.expected_sha256 = contract.source_sha256();
        request.section_hint = "graph-header";
        request.length = contract.source_byte_size();
        const auto verified = objects.materialize(request);
        effective_bin_override = verified.local_path.string();
        diagnostic("Verified canonical graph header: " + effective_bin_override, true);
    }

    _pImpl->loadFromManifest(manifest_path, selection, shard_root, effective_bin_override, skip_payload);

    if (!contract.legacy().node_route_supported)
    {
        diagnostic("Manifest has no exhaustive node-route sidecar; partial graph remains resident-slice only.", true);
        return;
    }

    try
    {
        configure_partial_query_session(manifest_path, selection, shard_root,
                                        effective_bin_override, !skip_payload);
    }
    catch (const std::exception& error)
    {
        clear_partial_query_session();
        diagnostic(std::string("Routed partial-query session unavailable: ") + error.what(), true);
    }
}

void Zelph::append_partial_chunk(const PartialChunkSection section,
                                 const std::string& verified_path,
                                 const uint64_t source_offset,
                                 const uint32_t chunk_index,
                                 const uint32_t section_count) const
{
    // Query plans use a temporary cluster for their unification scaffolding.
    // Shards are durable dataset state and must never inherit that cluster,
    // otherwise query cleanup would delete freshly loaded graph nodes.
    const std::string previous_cluster = active_cluster_name();
    deactivate_cluster();

    try
    {
        detail::chunk_selector selection{chunk_index};
        invalidate_fact_structures_cache();
        switch (section)
        {
        case PartialChunkSection::left:
            _pImpl->loadLeftRightChunkFromPath(verified_path, source_offset, &selection, "left", section_count);
            break;
        case PartialChunkSection::right:
            _pImpl->loadLeftRightChunkFromPath(verified_path, source_offset, &selection, "right", section_count);
            break;
        case PartialChunkSection::name_of_node:
            _pImpl->loadNameOfNodeChunkFromPath(verified_path, source_offset, &selection);
            break;
        case PartialChunkSection::node_of_name:
            _pImpl->loadNodeOfNameChunkFromPath(verified_path, source_offset, &selection);
            break;
        }
    }
    catch (...)
    {
        restore_cluster(*this, previous_cluster);
        throw;
    }

    restore_cluster(*this, previous_cluster);
}
#endif

void        Zelph::set_active_cluster(const std::string& name) const { _pImpl->set_active_cluster(name); }
void        Zelph::deactivate_cluster() const { _pImpl->deactivate_cluster(); }
std::string Zelph::active_cluster_name() const { return _pImpl->active_cluster_name(); }
std::vector<std::pair<std::string, size_t>> Zelph::list_clusters() const { return _pImpl->list_clusters(); }
bool Zelph::merge_cluster(const std::string& from, const std::string& to) const { return _pImpl->merge_cluster(from, to); }

size_t Zelph::drop_cluster(const std::string& name) const
{
    const std::vector<Node> nodes = _pImpl->take_cluster(name);
    if (nodes.empty()) return 0;
    invalidate_fact_structures_cache();
    size_t removed = 0;
    for (const Node n : nodes)
    {
        if (_pImpl->exists(n))
        {
            remove_node(n);
            ++removed;
        }
    }
    return removed;
}
