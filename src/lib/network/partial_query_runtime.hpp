/*
Copyright (c) 2026 acrion innovations GmbH

Progressive shard-routing state machine for query-driven partial evaluation.
*/
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zelph::network::partial_query
{
    enum class Direction { outgoing, incoming, predicate_scan, global_scan, name_lookup };
    enum class RepresentationLayer { direct_claim, statement, main_snak, qualifier, reference, rank, names };
    enum class ShardState { pending, in_flight, loaded, failed };
    enum class CompletionState { running, complete, incomplete_known, unknown };
    enum class RowContract { sound_lower_bound, provisional_retractable, withheld_until_complete, exact };
    enum class EventType
    {
        query_started,
        routing_obligations_added,
        shard_fetch_started,
        shard_loaded,
        result_batch,
        coverage_changed,
        query_completed,
        query_cancelled,
        query_failed,
    };

    struct RoutingObligation
    {
        std::string id;
        std::optional<uint64_t> subject;
        std::optional<uint64_t> predicate;
        std::optional<uint64_t> object;
        Direction direction = Direction::outgoing;
        RepresentationLayer layer = RepresentationLayer::direct_claim;
        std::string consumer;
        uint64_t depth = 0;
    };

    struct ShardCandidate
    {
        std::string id;
        uint64_t byte_size = 0;
        bool cached = false;
        uint64_t expected_resolved_obligations = 1;
    };

    struct Budget
    {
        uint64_t max_shards = 0;
        uint64_t max_bytes = 0;
        uint64_t max_elapsed_milliseconds = 0;
        uint64_t max_path_depth = 0;
        uint64_t max_result_rows = 0;
    };

    struct QueryMetrics
    {
        uint64_t routing_lookup_milliseconds = 0;
        uint64_t cache_lookup_milliseconds = 0;
        uint64_t network_fetch_milliseconds = 0;
        uint64_t checksum_verify_milliseconds = 0;
        uint64_t shard_decode_milliseconds = 0;
        uint64_t graph_integration_milliseconds = 0;
        uint64_t completion_milliseconds = 0;
        uint64_t bytes_fetched = 0;
        uint64_t shards_loaded = 0;
        uint64_t cache_hits = 0;
        uint64_t obligations_added = 0;
        uint64_t duplicate_obligations = 0;
        uint64_t result_rows = 0;
        uint64_t bindings_unblocked = 0;
    };

    struct Event
    {
        EventType type = EventType::coverage_changed;
        std::string query_id;
        std::string detail;
        uint64_t rows = 0;
        uint64_t bytes = 0;
        uint64_t elapsed_milliseconds = 0;

        static const char* name(EventType value)
        {
            switch (value)
            {
            case EventType::query_started: return "query_started";
            case EventType::routing_obligations_added: return "routing_obligations_added";
            case EventType::shard_fetch_started: return "shard_fetch_started";
            case EventType::shard_loaded: return "shard_loaded";
            case EventType::result_batch: return "result_batch";
            case EventType::coverage_changed: return "coverage_changed";
            case EventType::query_completed: return "query_completed";
            case EventType::query_cancelled: return "query_cancelled";
            case EventType::query_failed: return "query_failed";
            }
            return "coverage_changed";
        }

        std::string json() const
        {
            auto escaped = [](const std::string& value)
            {
                std::string out;
                for (const char c : value)
                {
                    if (c == '\\' || c == '"') out.push_back('\\');
                    if (c == '\n') { out += "\\n"; continue; }
                    out.push_back(c);
                }
                return out;
            };
            std::ostringstream out;
            out << "{\"type\":\"" << name(type) << "\",\"query_id\":\"" << escaped(query_id)
                << "\",\"detail\":\"" << escaped(detail) << "\",\"rows\":" << rows
                << ",\"bytes\":" << bytes << ",\"elapsed_ms\":" << elapsed_milliseconds << '}';
            return out.str();
        }
    };

    struct CompletionCertificate
    {
        CompletionState state = CompletionState::running;
        bool evaluation_fixed_point = false;
        bool routing_fixed_point = false;
        bool manifest_exhaustive = false;
        bool representation_coverage = false;
        uint64_t loaded_shards = 0;
        uint64_t failed_shards = 0;
        uint64_t unresolved_obligations = 0;
        uint64_t relevant_unloaded_shards = 0;
        std::string reason;
    };

    class Runtime
    {
    public:
        explicit Runtime(Budget budget = {}) : _budget(budget), _started(std::chrono::steady_clock::now()) {}

        void set_query_id(std::string value) { _query_id = std::move(value); }
        const std::string& query_id() const { return _query_id; }
        void set_manifest_exhaustive(bool value) { _manifest_exhaustive = value; }
        void set_representation_coverage(bool value) { _representation_coverage = value; }
        void set_evaluation_fixed_point(bool value) { _evaluation_fixed_point = value; }
        void set_budget(Budget value) { _budget = value; }
        const Budget& budget() const { return _budget; }
        QueryMetrics& metrics() { return _metrics; }
        const QueryMetrics& metrics() const { return _metrics; }
        const std::vector<Event>& events() const { return _events; }

        uint64_t elapsed_milliseconds() const
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - _started).count());
        }

        void emit(EventType type, std::string detail = {}, uint64_t rows = 0, uint64_t bytes = 0)
        {
            _events.push_back({type, _query_id, std::move(detail), rows, bytes, elapsed_milliseconds()});
        }

        bool add_obligation(const RoutingObligation& obligation, const std::vector<ShardCandidate>& candidates)
        {
            if (obligation.id.empty()) throw std::runtime_error("Routing obligation IDs must be non-empty");
            if (_obligations.contains(obligation.id))
            {
                ++_metrics.duplicate_obligations;
                return false;
            }
            if (_budget.max_path_depth && obligation.depth > _budget.max_path_depth)
            {
                stop("path-depth-budget-exhausted");
                return false;
            }
            _obligations[obligation.id] = obligation;
            ++_metrics.obligations_added;
            if (candidates.empty()) _unroutable_obligations.insert(obligation.id);
            for (const auto& candidate : candidates)
            {
                if (candidate.id.empty()) throw std::runtime_error("Shard IDs must be non-empty");
                auto& entry = _shards[candidate.id];
                entry.candidate.id = candidate.id;
                entry.candidate.byte_size = std::max(entry.candidate.byte_size, candidate.byte_size);
                entry.candidate.cached = entry.candidate.cached || candidate.cached;
                entry.candidate.expected_resolved_obligations = std::max(
                    entry.candidate.expected_resolved_obligations, candidate.expected_resolved_obligations);
                entry.obligations.insert(obligation.id);
            }
            _evaluation_fixed_point = false;
            emit(EventType::routing_obligations_added, obligation.id);
            return true;
        }

        std::vector<std::string> next_batch(uint64_t max_batch = 1) const
        {
            struct Ranked { std::string id; uint64_t byte_size; long double score; };
            std::vector<Ranked> ranked;
            for (const auto& [id, entry] : _shards)
            {
                if (entry.state != ShardState::pending) continue;
                uint64_t unresolved = 0;
                for (const auto& obligation : entry.obligations)
                    if (!_resolved_obligations.contains(obligation)) ++unresolved;
                if (!unresolved) continue;
                const uint64_t cost = std::max<uint64_t>(1, entry.candidate.byte_size);
                const uint64_t cache_multiplier = entry.candidate.cached ? 8 : 1;
                const long double score = static_cast<long double>(
                    unresolved * std::max<uint64_t>(1, entry.candidate.expected_resolved_obligations) * cache_multiplier)
                    / static_cast<long double>(cost);
                ranked.push_back({id, entry.candidate.byte_size, score});
            }
            std::sort(ranked.begin(), ranked.end(), [](const Ranked& lhs, const Ranked& rhs)
            {
                if (lhs.score != rhs.score) return lhs.score > rhs.score;
                if (lhs.byte_size != rhs.byte_size) return lhs.byte_size < rhs.byte_size;
                return lhs.id < rhs.id;
            });

            std::vector<std::string> result;
            uint64_t selected_bytes = 0;
            for (const auto& candidate : ranked)
            {
                if (max_batch && result.size() >= max_batch) break;
                if (_budget.max_shards && _metrics.shards_loaded + result.size() >= _budget.max_shards) break;
                if (_budget.max_bytes && _metrics.bytes_fetched + selected_bytes + candidate.byte_size > _budget.max_bytes) continue;
                result.push_back(candidate.id);
                selected_bytes += candidate.byte_size;
            }
            return result;
        }

        bool budget_available() const
        {
            if (_stopped) return false;
            if (_budget.max_elapsed_milliseconds && elapsed_milliseconds() >= _budget.max_elapsed_milliseconds) return false;
            if (_budget.max_result_rows && _metrics.result_rows >= _budget.max_result_rows) return false;
            return true;
        }

        void mark_in_flight(const std::string& shard_id)
        {
            auto& entry = shard(shard_id);
            if (entry.state != ShardState::pending) throw std::runtime_error("Only pending shards can enter flight: " + shard_id);
            entry.state = ShardState::in_flight;
            emit(EventType::shard_fetch_started, shard_id, 0, entry.candidate.byte_size);
        }

        void accept_shard(const std::string& shard_id, const std::vector<std::string>& resolved_obligations, uint64_t bytes)
        {
            auto& entry = shard(shard_id);
            if (entry.state != ShardState::pending && entry.state != ShardState::in_flight)
                throw std::runtime_error("Shard cannot be accepted from its current state: " + shard_id);
            entry.state = ShardState::loaded;
            ++_metrics.shards_loaded;
            _metrics.bytes_fetched += bytes;
            for (const auto& obligation : resolved_obligations)
            {
                if (_obligations.contains(obligation))
                {
                    _resolved_obligations.insert(obligation);
                    _unroutable_obligations.erase(obligation);
                }
            }
            _evaluation_fixed_point = false;
            emit(EventType::shard_loaded, shard_id, 0, bytes);
        }

        void resolve_obligation(const std::string& id)
        {
            if (_obligations.contains(id))
            {
                _resolved_obligations.insert(id);
                _unroutable_obligations.erase(id);
            }
        }

        void fail_shard(const std::string& shard_id, std::string reason)
        {
            auto& entry = shard(shard_id);
            entry.state = ShardState::failed;
            entry.failure_reason = reason;
            emit(EventType::query_failed, shard_id + ":" + reason);
        }

        void add_result_rows(uint64_t rows, RowContract contract)
        {
            _metrics.result_rows += rows;
            const char* label = contract == RowContract::sound_lower_bound ? "sound-lower-bound"
                              : contract == RowContract::provisional_retractable ? "provisional-retractable"
                              : contract == RowContract::withheld_until_complete ? "withheld-until-complete" : "exact";
            emit(EventType::result_batch, label, rows);
            if (_budget.max_result_rows && _metrics.result_rows >= _budget.max_result_rows)
                stop("result-row-budget-exhausted");
        }

        void stop(std::string reason)
        {
            _stopped = true;
            _stop_reason = std::move(reason);
            emit(EventType::query_cancelled, _stop_reason);
        }

        size_t unresolved_obligation_count() const { return _obligations.size() - _resolved_obligations.size(); }
        size_t pending_shard_count() const { return count_state(ShardState::pending); }
        size_t in_flight_shard_count() const { return count_state(ShardState::in_flight); }
        size_t loaded_shard_count() const { return count_state(ShardState::loaded); }
        size_t failed_shard_count() const { return count_state(ShardState::failed); }

        CompletionCertificate completion() const
        {
            CompletionCertificate certificate;
            certificate.evaluation_fixed_point = _evaluation_fixed_point;
            certificate.manifest_exhaustive = _manifest_exhaustive;
            certificate.representation_coverage = _representation_coverage;
            certificate.loaded_shards = loaded_shard_count();
            certificate.failed_shards = failed_shard_count();
            certificate.unresolved_obligations = unresolved_obligation_count();
            certificate.relevant_unloaded_shards = relevant_unloaded_shard_count();
            certificate.routing_fixed_point = certificate.unresolved_obligations == 0
                                             && certificate.relevant_unloaded_shards == 0
                                             && in_flight_shard_count() == 0
                                             && _unroutable_obligations.empty();
            if (_stopped)
            {
                certificate.state = CompletionState::incomplete_known;
                certificate.reason = _stop_reason.empty() ? "execution-stopped" : _stop_reason;
            }
            else if (certificate.failed_shards)
            {
                certificate.state = CompletionState::incomplete_known;
                certificate.reason = "relevant-shard-failed";
            }
            else if (!_unroutable_obligations.empty())
            {
                certificate.state = CompletionState::unknown;
                certificate.reason = "routing-index-not-exhaustive-for-obligation";
            }
            else if (certificate.evaluation_fixed_point && certificate.routing_fixed_point
                     && certificate.manifest_exhaustive && certificate.representation_coverage)
            {
                certificate.state = CompletionState::complete;
                certificate.reason = "joint-routing-evaluation-fixed-point";
            }
            else
            {
                certificate.state = CompletionState::running;
                certificate.reason = "more-routing-or-evaluation-required";
            }
            return certificate;
        }

    private:
        struct ShardEntry
        {
            ShardCandidate candidate;
            ShardState state = ShardState::pending;
            std::set<std::string> obligations;
            std::string failure_reason;
        };

        ShardEntry& shard(const std::string& id)
        {
            const auto it = _shards.find(id);
            if (it == _shards.end()) throw std::runtime_error("Unknown shard: " + id);
            return it->second;
        }

        size_t count_state(ShardState state) const
        {
            return std::count_if(_shards.begin(), _shards.end(), [state](const auto& pair)
                                 { return pair.second.state == state; });
        }

        size_t relevant_unloaded_shard_count() const
        {
            size_t count = 0;
            for (const auto& [_, entry] : _shards)
            {
                if (entry.state == ShardState::loaded || entry.state == ShardState::failed) continue;
                if (std::any_of(entry.obligations.begin(), entry.obligations.end(), [this](const auto& id)
                                { return !_resolved_obligations.contains(id); })) ++count;
            }
            return count;
        }

        Budget _budget;
        std::chrono::steady_clock::time_point _started;
        std::string _query_id;
        std::map<std::string, RoutingObligation> _obligations;
        std::set<std::string> _resolved_obligations;
        std::set<std::string> _unroutable_obligations;
        std::map<std::string, ShardEntry> _shards;
        std::vector<Event> _events;
        QueryMetrics _metrics;
        bool _manifest_exhaustive = false;
        bool _representation_coverage = false;
        bool _evaluation_fixed_point = false;
        bool _stopped = false;
        std::string _stop_reason;
    };
} // namespace zelph::network::partial_query
