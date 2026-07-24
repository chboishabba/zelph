/*
Copyright (c) 2026 acrion innovations GmbH

Live bridge between hosted manifests and ordinary Zelph graph lookups.
*/
#pragma once

#ifndef __EMSCRIPTEN__
#include "partial_query_manifest.hpp"
#include "partial_query_runtime.hpp"
#include "verified_object_store.hpp"
#include "zelph.hpp"

#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace zelph::network
{
    class PartialQuerySession
    {
    public:
        PartialQuerySession(Zelph& owner,
                            std::string manifest_source,
                            const Zelph::BinChunkSelection& initial_selection,
                            std::string shard_root,
                            std::string source_bin_override,
                            bool initial_payload_loaded);

        bool ensure_name(const std::string& name, const std::string& language);
        void ensure_node_names(Node node);
        void ensure_outgoing(Node subject, Node predicate, uint64_t depth = 0);
        void ensure_incoming(Node predicate, Node object, uint64_t depth = 0);

        bool supports_layers(const std::vector<std::string>& layers) const;
        bool certifiable() const { return _manifest.certifiable(); }
        void begin_query(const std::string& normalized_query,
                         const std::vector<std::string>& required_layers,
                         partial_query::RowContract contract);
        std::string finish_query(bool evaluation_fixed_point, uint64_t result_rows = 0);
        std::string status_json() const;
        bool active_query() const { return _query_active; }
        const PartialQueryManifest& manifest() const { return _manifest; }

    private:
        using ChunkSet = std::set<uint32_t>;

        void mark_initially_loaded(const Zelph::BinChunkSelection& selection, bool payload_loaded);
        detail::RouteSelectionResolution route_nodes(const std::vector<uint64_t>& nodes);
        detail::RouteSelectionResolution route_name(const std::string& name, const std::string& language);
        void load_resolution(const detail::RouteSelectionResolution& resolution,
                             const std::string& obligation_prefix,
                             partial_query::Direction direction,
                             partial_query::RepresentationLayer layer,
                             uint64_t depth);
        void load_chunk(PartialChunkSection section, uint32_t index, const std::string& obligation_id);
        ObjectRequest object_request(const PartialChunkDescriptor& descriptor) const;
        std::string resolve_uri(const std::string& uri) const;
        bool loaded(PartialChunkSection section, uint32_t index) const;
        void mark_loaded(PartialChunkSection section, uint32_t index);
        void emit_new_events();
        partial_query::Budget environment_budget() const;
        partial_query::RepresentationLayer layer_for_predicate(Node predicate) const;
        std::string layer_name(partial_query::RepresentationLayer layer) const;

        Zelph& _owner;
        PartialQueryManifest _manifest;
        VerifiedObjectStore _objects;
        std::string _shard_root;
        std::string _source_bin_override;
        mutable std::recursive_mutex _mutex;
        ChunkSet _loaded_left;
        ChunkSet _loaded_right;
        ChunkSet _loaded_name_of_node;
        ChunkSet _loaded_node_of_name;
        partial_query::Runtime _runtime;
        partial_query::RowContract _row_contract = partial_query::RowContract::sound_lower_bound;
        bool _query_active = false;
        size_t _emitted_events = 0;
    };
} // namespace zelph::network
#endif
