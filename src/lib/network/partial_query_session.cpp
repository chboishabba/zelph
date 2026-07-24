/*
Copyright (c) 2026 acrion innovations GmbH
*/

#include "partial_query_session.hpp"

#ifndef __EMSCRIPTEN__
#include "chrono/stopwatch.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>

namespace zelph::network
{
    namespace
    {
        std::mutex registry_mutex;
        std::map<const Zelph*, std::unique_ptr<PartialQuerySession>> sessions;
        std::atomic<uint64_t> global_query_serial{0};

        uint64_t env_u64(const char* name)
        {
            const char* value = std::getenv(name);
            if (!value || !*value) return 0;
            try { return std::stoull(value); }
            catch (...) { return 0; }
        }

        std::string completion_name(partial_query::CompletionState state)
        {
            switch (state)
            {
            case partial_query::CompletionState::running: return "running";
            case partial_query::CompletionState::complete: return "complete";
            case partial_query::CompletionState::incomplete_known: return "incomplete-known";
            case partial_query::CompletionState::unknown: return "unknown";
            }
            return "unknown";
        }

        PartialQuerySession* find_session(const Zelph* owner)
        {
            std::lock_guard lock(registry_mutex);
            const auto it = sessions.find(owner);
            return it == sessions.end() ? nullptr : it->second.get();
        }
    }

    PartialQuerySession::PartialQuerySession(Zelph& owner,
                                             std::string manifest_source,
                                             const Zelph::BinChunkSelection& initial_selection,
                                             std::string shard_root,
                                             std::string source_bin_override,
                                             const bool initial_payload_loaded)
        : _owner(owner)
        , _manifest(PartialQueryManifest::load(manifest_source, shard_root))
        , _shard_root(std::move(shard_root))
        , _source_bin_override(std::move(source_bin_override))
        , _runtime(environment_budget())
    {
        if (_manifest.canonical())
        {
            const auto& route = _manifest.node_routing_index();
            ObjectRequest request;
            request.dataset_version = _manifest.dataset_version();
            request.uri = resolve_uri(route.uri);
            request.expected_sha256 = route.sha256;
            request.section_hint = "routing-index";
            const auto verified = _objects.materialize(request);
            _manifest.legacy().node_route_index_local_path = verified.local_path.string();
            _manifest.legacy().node_route_supported = route.exhaustive;
        }
        mark_initially_loaded(initial_selection, initial_payload_loaded);
        _runtime.set_manifest_exhaustive(_manifest.certifiable());
        _owner.diagnostic("Partial-query session: " + _manifest.coverage_summary(), true);
    }

    partial_query::Budget PartialQuerySession::environment_budget() const
    {
        partial_query::Budget budget;
        budget.max_shards = env_u64("ZELPH_PARTIAL_MAX_SHARDS");
        budget.max_bytes = env_u64("ZELPH_PARTIAL_MAX_BYTES");
        budget.max_elapsed_milliseconds = env_u64("ZELPH_PARTIAL_MAX_MILLISECONDS");
        budget.max_path_depth = env_u64("ZELPH_PARTIAL_MAX_PATH_DEPTH");
        budget.max_result_rows = env_u64("ZELPH_PARTIAL_MAX_RESULT_ROWS");
        return budget;
    }

    void PartialQuerySession::mark_initially_loaded(const Zelph::BinChunkSelection& selection, const bool payload_loaded)
    {
        if (!payload_loaded) return;
        const bool route_requested = selection.route_nodes_explicit || selection.route_name_explicit;
        const bool any_explicit = selection.left_explicit || selection.right_explicit
                               || selection.name_of_node_explicit || selection.node_of_name_explicit || route_requested;
        if (!any_explicit)
        {
            for (const auto& chunk : _manifest.chunks(PartialChunkSection::left)) _loaded_left.insert(chunk.chunk_index);
            for (const auto& chunk : _manifest.chunks(PartialChunkSection::right)) _loaded_right.insert(chunk.chunk_index);
            for (const auto& chunk : _manifest.chunks(PartialChunkSection::name_of_node)) _loaded_name_of_node.insert(chunk.chunk_index);
            for (const auto& chunk : _manifest.chunks(PartialChunkSection::node_of_name)) _loaded_node_of_name.insert(chunk.chunk_index);
            return;
        }
        _loaded_left.insert(selection.left.begin(), selection.left.end());
        _loaded_right.insert(selection.right.begin(), selection.right.end());
        _loaded_name_of_node.insert(selection.nameOfNode.begin(), selection.nameOfNode.end());
        _loaded_node_of_name.insert(selection.nodeOfName.begin(), selection.nodeOfName.end());
        try
        {
            if (selection.route_nodes_explicit)
            {
                const auto resolution = route_nodes(selection.route_nodes);
                _loaded_left.insert(resolution.left.begin(), resolution.left.end());
                _loaded_right.insert(resolution.right.begin(), resolution.right.end());
                _loaded_name_of_node.insert(resolution.name_of_node.begin(), resolution.name_of_node.end());
                _loaded_node_of_name.insert(resolution.node_of_name.begin(), resolution.node_of_name.end());
            }
            if (selection.route_name_explicit)
            {
                const auto resolution = route_name(selection.route_name, selection.route_lang);
                _loaded_node_of_name.insert(resolution.node_of_name.begin(), resolution.node_of_name.end());
            }
        }
        catch (const std::exception& error)
        {
            _owner.diagnostic(std::string("Could not reconstruct initial routed chunk set: ") + error.what(), true);
        }
    }

    detail::RouteSelectionResolution PartialQuerySession::route_nodes(const std::vector<uint64_t>& nodes)
    {
        const auto started = std::chrono::steady_clock::now();
        Zelph::BinChunkSelection selection;
        selection.route_nodes = nodes;
        selection.route_nodes_explicit = true;
        const auto result = detail::resolve_route_selection(
            _manifest.local_path(), _manifest.legacy(), selection, _shard_root);
        _runtime.metrics().routing_lookup_milliseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        return result;
    }

    detail::RouteSelectionResolution PartialQuerySession::route_name(const std::string& name, const std::string& language)
    {
        const auto started = std::chrono::steady_clock::now();
        Zelph::BinChunkSelection selection;
        selection.route_name = name;
        selection.route_lang = language;
        selection.route_name_explicit = true;
        const auto result = detail::resolve_route_selection(
            _manifest.local_path(), _manifest.legacy(), selection, _shard_root);
        _runtime.metrics().routing_lookup_milliseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        return result;
    }

    bool PartialQuerySession::loaded(const PartialChunkSection section, const uint32_t index) const
    {
        switch (section)
        {
        case PartialChunkSection::left: return _loaded_left.contains(index);
        case PartialChunkSection::right: return _loaded_right.contains(index);
        case PartialChunkSection::name_of_node: return _loaded_name_of_node.contains(index);
        case PartialChunkSection::node_of_name: return _loaded_node_of_name.contains(index);
        }
        return false;
    }

    void PartialQuerySession::mark_loaded(const PartialChunkSection section, const uint32_t index)
    {
        switch (section)
        {
        case PartialChunkSection::left: _loaded_left.insert(index); break;
        case PartialChunkSection::right: _loaded_right.insert(index); break;
        case PartialChunkSection::name_of_node: _loaded_name_of_node.insert(index); break;
        case PartialChunkSection::node_of_name: _loaded_node_of_name.insert(index); break;
        }
    }

    std::string PartialQuerySession::resolve_uri(const std::string& uri) const
    {
        if (uri.empty()) return {};
        if (detail::is_hf_uri(uri) || uri.rfind("file://", 0) == 0
            || uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0
            || std::filesystem::path(uri).is_absolute()) return uri;
        const auto manifest_relative = std::filesystem::path(_manifest.local_path()).parent_path() / uri;
        if (std::filesystem::exists(manifest_relative)) return manifest_relative.string();
        if (!_shard_root.empty())
        {
            const auto rooted = std::filesystem::path(_shard_root) / std::filesystem::path(uri).filename();
            if (std::filesystem::exists(rooted)) return rooted.string();
        }
        return manifest_relative.string();
    }

    ObjectRequest PartialQuerySession::object_request(const PartialChunkDescriptor& descriptor) const
    {
        ObjectRequest request;
        request.dataset_version = _manifest.dataset_version();
        request.expected_sha256 = descriptor.sha256;
        request.section_hint = std::string(PartialQueryManifest::section_name(descriptor.section))
                             + "-" + std::to_string(descriptor.chunk_index);
        request.length = descriptor.byte_size;
        if (!descriptor.uri.empty())
        {
            request.uri = resolve_uri(descriptor.uri);
            request.offset = descriptor.has_source_offset ? descriptor.source_offset : 0;
        }
        else
        {
            request.uri = resolve_uri(_source_bin_override.empty()
                                      ? _manifest.legacy().source_bin_path : _source_bin_override);
            request.offset = descriptor.source_offset;
        }
        return request;
    }

    void PartialQuerySession::load_chunk(const PartialChunkSection section,
                                         const uint32_t index,
                                         const std::string& obligation_id)
    {
        if (loaded(section, index))
        {
            _runtime.resolve_obligation(obligation_id);
            return;
        }
        const auto* descriptor = _manifest.chunk(section, index);
        if (!descriptor) throw std::runtime_error("Routing index selected an undeclared chunk");
        const std::string shard_id = std::string(PartialQueryManifest::section_name(section)) + ":" + std::to_string(index);
        _runtime.mark_in_flight(shard_id);
        emit_new_events();
        try
        {
            const auto materialized = _objects.materialize(object_request(*descriptor));
            _runtime.metrics().checksum_verify_milliseconds += materialized.verify_milliseconds;
            if (materialized.cache_disposition == CacheDisposition::verified_hit) ++_runtime.metrics().cache_hits;
            else if (materialized.cache_disposition == CacheDisposition::fetched)
                _runtime.metrics().network_fetch_milliseconds += materialized.materialize_milliseconds;

            const auto integrate_started = std::chrono::steady_clock::now();
            _owner.append_partial_chunk(section, materialized.local_path.string(), 0, index,
                                        static_cast<uint32_t>(_manifest.chunks(section).size()));
            _runtime.metrics().graph_integration_milliseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - integrate_started).count());
            mark_loaded(section, index);
            _runtime.accept_shard(shard_id, {obligation_id}, materialized.byte_size);
            emit_new_events();
        }
        catch (const std::exception& error)
        {
            _runtime.fail_shard(shard_id, error.what());
            emit_new_events();
            throw;
        }
    }

    void PartialQuerySession::load_resolution(const detail::RouteSelectionResolution& resolution,
                                              const std::string& obligation_prefix,
                                              const partial_query::Direction direction,
                                              const partial_query::RepresentationLayer layer,
                                              const uint64_t depth)
    {
        struct Routed { PartialChunkSection section; uint32_t index; };
        std::map<std::string, Routed> routed;
        auto add = [&](PartialChunkSection section, const detail::chunk_selector& indexes)
        {
            for (const uint32_t index : indexes)
            {
                if (loaded(section, index)) continue;
                const auto* descriptor = _manifest.chunk(section, index);
                if (!descriptor) throw std::runtime_error("Routing sidecar references an undeclared chunk");
                const std::string shard_id = std::string(PartialQueryManifest::section_name(section)) + ":" + std::to_string(index);
                const std::string obligation_id = obligation_prefix + "/" + shard_id;
                partial_query::RoutingObligation obligation;
                obligation.id = obligation_id;
                obligation.direction = direction;
                obligation.layer = layer;
                obligation.consumer = obligation_prefix;
                obligation.depth = depth;
                _runtime.add_obligation(obligation, {{shard_id, descriptor->byte_size, false, 1}});
                routed[shard_id] = {section, index};
            }
        };
        add(PartialChunkSection::left, resolution.left);
        add(PartialChunkSection::right, resolution.right);
        add(PartialChunkSection::name_of_node, resolution.name_of_node);
        add(PartialChunkSection::node_of_name, resolution.node_of_name);
        emit_new_events();

        while (_runtime.budget_available())
        {
            const auto batch = _runtime.next_batch(1);
            if (batch.empty()) break;
            const auto it = routed.find(batch.front());
            if (it == routed.end()) break;
            const std::string obligation_id = obligation_prefix + "/" + batch.front();
            load_chunk(it->second.section, it->second.index, obligation_id);
        }
        if (!_runtime.budget_available())
        {
            if (_runtime.budget().max_elapsed_milliseconds
                && _runtime.elapsed_milliseconds() >= _runtime.budget().max_elapsed_milliseconds)
                _runtime.stop("elapsed-time-budget-exhausted");
            else
                _runtime.stop("shard-or-byte-budget-exhausted");
            emit_new_events();
        }
    }

    bool PartialQuerySession::ensure_name(const std::string& name, const std::string& language)
    {
        std::lock_guard lock(_mutex);
        try
        {
            const auto resolution = route_name(name, language);
            load_resolution(resolution, "name:" + language + ":" + name,
                            partial_query::Direction::name_lookup,
                            partial_query::RepresentationLayer::names, 0);
            return true;
        }
        catch (const std::exception& error)
        {
            if (std::string(error.what()).find("resolved no matching chunks") != std::string::npos) return false;
            throw;
        }
    }

    void PartialQuerySession::ensure_node_names(const Node node)
    {
        std::lock_guard lock(_mutex);
        try
        {
            const auto resolution = route_nodes({node});
            detail::RouteSelectionResolution names;
            names.name_of_node = resolution.name_of_node;
            load_resolution(names, "node-name:" + std::to_string(node), partial_query::Direction::name_lookup,
                            partial_query::RepresentationLayer::names, 0);
        }
        catch (const std::exception& error)
        {
            if (std::string(error.what()).find("resolved no matching chunks") == std::string::npos) throw;
        }
    }

    partial_query::RepresentationLayer PartialQuerySession::layer_for_predicate(const Node predicate) const
    {
        const auto name = _owner.get_name(predicate, "wikidata", false);
        if (name.rfind("p:", 0) == 0) return partial_query::RepresentationLayer::statement;
        if (name.rfind("ps:", 0) == 0) return partial_query::RepresentationLayer::main_snak;
        if (name.rfind("pq:", 0) == 0) return partial_query::RepresentationLayer::qualifier;
        if (name.rfind("pr:", 0) == 0) return partial_query::RepresentationLayer::reference;
        if (name == "wikibase:rank") return partial_query::RepresentationLayer::rank;
        return partial_query::RepresentationLayer::direct_claim;
    }

    std::string PartialQuerySession::layer_name(const partial_query::RepresentationLayer layer) const
    {
        switch (layer)
        {
        case partial_query::RepresentationLayer::direct_claim: return "directClaim";
        case partial_query::RepresentationLayer::statement: return "statement";
        case partial_query::RepresentationLayer::main_snak: return "mainSnak";
        case partial_query::RepresentationLayer::qualifier: return "qualifier";
        case partial_query::RepresentationLayer::reference: return "reference";
        case partial_query::RepresentationLayer::rank: return "rank";
        case partial_query::RepresentationLayer::names: return "names";
        }
        return "directClaim";
    }

    void PartialQuerySession::ensure_outgoing(const Node subject, const Node predicate, const uint64_t depth)
    {
        std::lock_guard lock(_mutex);
        const auto layer = layer_for_predicate(predicate);
        if (!_manifest.layer_available(layer_name(layer)))
            throw std::runtime_error("Partial-query manifest does not include representation layer " + layer_name(layer));
        const auto subject_resolution = route_nodes({subject});
        load_resolution(subject_resolution, "out:" + std::to_string(subject) + ":" + std::to_string(predicate),
                        partial_query::Direction::outgoing, layer, depth);
        const auto relations = _owner.get_right(subject);
        if (!relations.empty())
        {
            std::vector<uint64_t> relation_nodes(relations.begin(), relations.end());
            const auto relation_resolution = route_nodes(relation_nodes);
            load_resolution(relation_resolution, "out-rel:" + std::to_string(subject) + ":" + std::to_string(predicate),
                            partial_query::Direction::outgoing, layer, depth);
        }
    }

    void PartialQuerySession::ensure_incoming(const Node predicate, const Node object, const uint64_t depth)
    {
        std::lock_guard lock(_mutex);
        const auto layer = layer_for_predicate(predicate);
        if (!_manifest.layer_available(layer_name(layer)))
            throw std::runtime_error("Partial-query manifest does not include representation layer " + layer_name(layer));
        const auto object_resolution = route_nodes({object});
        load_resolution(object_resolution, "in:" + std::to_string(predicate) + ":" + std::to_string(object),
                        partial_query::Direction::incoming, layer, depth);
        const auto relations = _owner.get_right(object);
        if (!relations.empty())
        {
            std::vector<uint64_t> relation_nodes(relations.begin(), relations.end());
            const auto relation_resolution = route_nodes(relation_nodes);
            load_resolution(relation_resolution, "in-rel:" + std::to_string(predicate) + ":" + std::to_string(object),
                            partial_query::Direction::incoming, layer, depth);
        }
    }

    bool PartialQuerySession::supports_layers(const std::vector<std::string>& layers) const
    {
        for (const auto& layer : layers)
            if (!_manifest.layer_available(layer)) return false;
        return true;
    }

    void PartialQuerySession::begin_query(const std::string& normalized_query,
                                          const std::vector<std::string>& required_layers,
                                          const partial_query::RowContract contract)
    {
        std::lock_guard lock(_mutex);
        _runtime = partial_query::Runtime(environment_budget());
        _runtime.set_query_id("pq-" + std::to_string(++global_query_serial));
        _runtime.set_manifest_exhaustive(_manifest.certifiable());
        _runtime.set_representation_coverage(supports_layers(required_layers));
        _row_contract = contract;
        _query_active = true;
        _emitted_events = 0;
        _runtime.emit(partial_query::EventType::query_started,
                      "query_sha256=" + sha256::bytes(normalized_query));
        emit_new_events();
    }

    std::string PartialQuerySession::finish_query(const bool evaluation_fixed_point, const uint64_t result_rows)
    {
        std::lock_guard lock(_mutex);
        const auto started = std::chrono::steady_clock::now();
        if (result_rows) _runtime.add_result_rows(result_rows, _row_contract);
        _runtime.set_evaluation_fixed_point(evaluation_fixed_point);
        const auto certificate = _runtime.completion();
        _runtime.metrics().completion_milliseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        _runtime.emit(certificate.state == partial_query::CompletionState::complete
                          ? partial_query::EventType::query_completed
                          : partial_query::EventType::coverage_changed,
                      certificate.reason);
        emit_new_events();
        _query_active = false;
        return status_json();
    }

    std::string PartialQuerySession::status_json() const
    {
        std::lock_guard lock(_mutex);
        const auto certificate = _runtime.completion();
        const auto& metrics = _runtime.metrics();
        std::ostringstream out;
        out << "{\"query_id\":\"" << _runtime.query_id() << "\",\"completeness\":\""
            << completion_name(certificate.state) << "\",\"reason\":\"" << certificate.reason
            << "\",\"evaluation_fixed_point\":" << (certificate.evaluation_fixed_point ? "true" : "false")
            << ",\"routing_fixed_point\":" << (certificate.routing_fixed_point ? "true" : "false")
            << ",\"manifest_exhaustive\":" << (certificate.manifest_exhaustive ? "true" : "false")
            << ",\"representation_coverage\":" << (certificate.representation_coverage ? "true" : "false")
            << ",\"loaded_shards\":" << certificate.loaded_shards
            << ",\"failed_shards\":" << certificate.failed_shards
            << ",\"unresolved_obligations\":" << certificate.unresolved_obligations
            << ",\"bytes_fetched\":" << metrics.bytes_fetched
            << ",\"cache_hits\":" << metrics.cache_hits
            << ",\"routing_ms\":" << metrics.routing_lookup_milliseconds
            << ",\"fetch_ms\":" << metrics.network_fetch_milliseconds
            << ",\"verify_ms\":" << metrics.checksum_verify_milliseconds
            << ",\"integrate_ms\":" << metrics.graph_integration_milliseconds
            << '}';
        return out.str();
    }

    void PartialQuerySession::emit_new_events()
    {
        const auto& events = _runtime.events();
        while (_emitted_events < events.size())
            _owner.diagnostic("[partial-query] " + events[_emitted_events++].json(), true);
    }

    void Zelph::configure_partial_query_session(const std::string& manifest_source,
                                                const BinChunkSelection& initial_selection,
                                                const std::string& shard_root,
                                                const std::string& source_bin_override,
                                                const bool initial_payload_loaded) const
    {
        auto session = std::make_unique<PartialQuerySession>(
            *const_cast<Zelph*>(this), manifest_source, initial_selection,
            shard_root, source_bin_override, initial_payload_loaded);
        std::lock_guard lock(registry_mutex);
        sessions[this] = std::move(session);
    }

    void Zelph::clear_partial_query_session() const
    {
        std::lock_guard lock(registry_mutex);
        sessions.erase(this);
    }

    bool Zelph::partial_query_active() const { return find_session(this) != nullptr; }

    bool Zelph::ensure_partial_name(const std::string& name, const std::string& language) const
    {
        if (auto* session = find_session(this)) return session->ensure_name(name, language);
        return false;
    }

    void Zelph::ensure_partial_node_names(const Node node) const
    {
        if (auto* session = find_session(this)) session->ensure_node_names(node);
    }

    void Zelph::ensure_partial_outgoing(const Node subject, const Node predicate, const uint64_t depth) const
    {
        if (auto* session = find_session(this)) session->ensure_outgoing(subject, predicate, depth);
    }

    void Zelph::ensure_partial_incoming(const Node predicate, const Node object, const uint64_t depth) const
    {
        if (auto* session = find_session(this)) session->ensure_incoming(predicate, object, depth);
    }

    bool Zelph::partial_query_supports_layers(const std::vector<std::string>& layers) const
    {
        if (auto* session = find_session(this)) return session->supports_layers(layers);
        return false;
    }

    void Zelph::begin_partial_query(const std::string& query,
                                    const std::vector<std::string>& layers,
                                    const partial_query::RowContract contract) const
    {
        if (auto* session = find_session(this)) session->begin_query(query, layers, contract);
    }

    std::string Zelph::finish_partial_query(const bool evaluation_fixed_point, const uint64_t result_rows) const
    {
        if (auto* session = find_session(this)) return session->finish_query(evaluation_fixed_point, result_rows);
        return "{\"completeness\":\"unknown\",\"reason\":\"no-routed-manifest-session\"}";
    }

    std::string Zelph::partial_query_status_json() const
    {
        if (auto* session = find_session(this)) return session->status_json();
        return "{\"completeness\":\"unknown\",\"reason\":\"no-routed-manifest-session\"}";
    }
} // namespace zelph::network
#endif
