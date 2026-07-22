/*
Copyright (c) 2026 acrion innovations GmbH

Progressive shard-routing state machine for query-driven partial evaluation.
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zelph::network::partial_query
{
    enum class Direction
    {
        outgoing,
        incoming,
        predicate_scan,
        global_scan,
    };

    enum class RepresentationLayer
    {
        direct_claim,
        statement,
        main_snak,
        qualifier,
        reference,
        rank,
    };

    enum class ShardState
    {
        pending,
        in_flight,
        loaded,
        failed,
    };

    enum class CompletionState
    {
        running,
        complete,
        incomplete_known,
        unknown,
    };

    struct RoutingObligation
    {
        std::string         id;
        uint64_t            node = 0;
        uint64_t            predicate = 0;
        Direction           direction = Direction::outgoing;
        RepresentationLayer layer = RepresentationLayer::direct_claim;

        friend bool operator<(const RoutingObligation& lhs, const RoutingObligation& rhs)
        {
            return lhs.id < rhs.id;
        }
    };

    struct ShardCandidate
    {
        std::string id;
        uint64_t    byte_size = 0;
        bool        cached = false;
        uint64_t    expected_resolved_obligations = 1;
    };

    struct Budget
    {
        uint64_t max_shards = 0;
        uint64_t max_bytes = 0;
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
        void set_manifest_exhaustive(const bool value) { _manifest_exhaustive = value; }
        void set_representation_coverage(const bool value) { _representation_coverage = value; }
        void set_evaluation_fixed_point(const bool value) { _evaluation_fixed_point = value; }

        void add_obligation(const RoutingObligation& obligation, const std::vector<ShardCandidate>& candidates)
        {
            if (obligation.id.empty()) throw std::runtime_error("Routing obligation IDs must be non-empty");
            _obligations[obligation.id] = obligation;
            _resolved_obligations.erase(obligation.id);
            if (candidates.empty())
            {
                _unroutable_obligations.insert(obligation.id);
                return;
            }
            _unroutable_obligations.erase(obligation.id);
            for (const auto& candidate : candidates)
            {
                if (candidate.id.empty()) throw std::runtime_error("Shard IDs must be non-empty");
                auto& entry = _shards[candidate.id];
                entry.candidate.id = candidate.id;
                entry.candidate.byte_size = std::max(entry.candidate.byte_size, candidate.byte_size);
                entry.candidate.cached = entry.candidate.cached || candidate.cached;
                entry.candidate.expected_resolved_obligations = std::max(
                    entry.candidate.expected_resolved_obligations,
                    candidate.expected_resolved_obligations);
                entry.obligations.insert(obligation.id);
            }
            _evaluation_fixed_point = false;
        }

        std::vector<std::string> next_batch(const Budget& budget) const
        {
            struct Ranked
            {
                std::string id;
                uint64_t byte_size = 0;
                long double score = 0;
            };
            std::vector<Ranked> ranked;
            for (const auto& [id, entry] : _shards)
            {
                if (entry.state != ShardState::pending) continue;
                uint64_t unresolved = 0;
                for (const auto& obligation : entry.obligations)
                    if (!_resolved_obligations.contains(obligation)) ++unresolved;
                if (unresolved == 0) continue;
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
            uint64_t bytes = 0;
            for (const auto& candidate : ranked)
            {
                if (budget.max_shards && result.size() >= budget.max_shards) break;
                if (budget.max_bytes && !result.empty() && bytes + candidate.byte_size > budget.max_bytes) continue;
                if (budget.max_bytes && result.empty() && candidate.byte_size > budget.max_bytes) continue;
                result.push_back(candidate.id);
                bytes += candidate.byte_size;
            }
            return result;
        }

        void mark_in_flight(const std::string& shard_id)
        {
            auto& entry = shard(shard_id);
            if (entry.state != ShardState::pending)
                throw std::runtime_error("Only pending shards can enter flight: " + shard_id);
            entry.state = ShardState::in_flight;
        }

        void accept_shard(const std::string& shard_id, const std::vector<std::string>& resolved_obligations)
        {
            auto& entry = shard(shard_id);
            if (entry.state != ShardState::pending && entry.state != ShardState::in_flight)
                throw std::runtime_error("Shard cannot be accepted from its current state: " + shard_id);
            entry.state = ShardState::loaded;
            for (const auto& obligation : resolved_obligations)
            {
                if (!_obligations.contains(obligation))
                    throw std::runtime_error("Unknown routing obligation resolved by shard " + shard_id + ": " + obligation);
                _resolved_obligations.insert(obligation);
                _unroutable_obligations.erase(obligation);
            }
            _evaluation_fixed_point = false;
        }

        void fail_shard(const std::string& shard_id, std::string reason)
        {
            auto& entry = shard(shard_id);
            entry.state = ShardState::failed;
            entry.failure_reason = std::move(reason);
        }

        void stop(std::string reason)
        {
            _stopped = true;
            _stop_reason = std::move(reason);
        }

        size_t unresolved_obligation_count() const
        {
            return _obligations.size() - _resolved_obligations.size();
        }

        size_t pending_shard_count() const
        {
            return count_state(ShardState::pending);
        }

        size_t in_flight_shard_count() const
        {
            return count_state(ShardState::in_flight);
        }

        size_t loaded_shard_count() const
        {
            return count_state(ShardState::loaded);
        }

        size_t failed_shard_count() const
        {
            return count_state(ShardState::failed);
        }

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
                certificate.reason = _stop_reason.empty() ? "execution stopped before the routing fixed point" : _stop_reason;
            }
            else if (certificate.failed_shards > 0)
            {
                certificate.state = CompletionState::incomplete_known;
                certificate.reason = "one or more relevant shards failed to load";
            }
            else if (!_unroutable_obligations.empty())
            {
                certificate.state = CompletionState::unknown;
                certificate.reason = "the routing index could not exhaustively map every obligation";
            }
            else if (certificate.evaluation_fixed_point
                     && certificate.routing_fixed_point
                     && certificate.manifest_exhaustive
                     && certificate.representation_coverage)
            {
                certificate.state = CompletionState::complete;
                certificate.reason = "evaluation and routing reached a certified fixed point";
            }
            else
            {
                certificate.state = CompletionState::running;
                certificate.reason = "more evaluation, routing, or coverage evidence is required";
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

        size_t count_state(const ShardState state) const
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
                const bool relevant = std::any_of(entry.obligations.begin(), entry.obligations.end(), [this](const auto& id)
                                                  { return !_resolved_obligations.contains(id); });
                if (relevant) ++count;
            }
            return count;
        }

        std::map<std::string, RoutingObligation> _obligations;
        std::set<std::string> _resolved_obligations;
        std::set<std::string> _unroutable_obligations;
        std::map<std::string, ShardEntry> _shards;
        bool _manifest_exhaustive = false;
        bool _representation_coverage = false;
        bool _evaluation_fixed_point = false;
        bool _stopped = false;
        std::string _stop_reason;
    };
} // namespace zelph::network::partial_query
