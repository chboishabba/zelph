# Partial-graph SPARQL: safety, semantics, and progressive completeness

Zelph's partial loader and its SPARQL subset solve different problems. Partial
loading controls which serialized graph shards are resident. SPARQL defines a
query language over the graph that is resident. Treating `.import sparql` as an
ordinary graph program conflates language registration, graph mutation, and
query completeness.

This document defines the contracts between those layers.

## 1. Import capabilities

An import has one of three capabilities:

- `language-extension`: registers syntax or query handlers without changing the
  extensional graph;
- `graph-transform`: materializes or rewrites graph data;
- `rule-program`: installs rules or arbitrary executable graph logic.

A partial session permits only trusted standard-library language extensions.
The trust decision is not based on the filename. An installed sidecar named
`<module>.zph.capability` must declare `version=1` and
`capability=language-extension`, and the module must resolve under a configured
standard-library root.

The import executes inside a temporary graph cluster. Zelph fingerprints the
extensional graph before the import, removes every node created in the cluster,
and verifies that the original fingerprint is restored. A declared language
extension that changes the graph is rejected. Dot commands, ordinary Zelph
facts/rules, and raw Janet input remain blocked while a partial graph is loaded.

`stdlib/sparql.zph.capability` is the first installed declaration.

## 2. Resident-slice result semantics

Let `G_p` be the resident graph and `G` the intended complete graph, with
`G_p subset-of G`.

For a positive monotone query `Q`:

```text
Q(G_p) subset-of Q(G)
```

Rows returned from `G_p` are therefore valid, but the result set may omit rows.
Zelph reports this as:

```text
dataset_mode: partial
evaluation_scope: resident_graph
query_class: positive-monotone
row_contract: sound-lower-bound
result_set_status: incomplete
```

The partial resident-slice policy rejects operators whose ordinary result may
become false under graph extension:

- `MINUS`, `OPTIONAL`, and absence-sensitive forms;
- `COUNT`, `GROUP BY`, and other exact cardinality claims;
- `ORDER BY`, `LIMIT`, and `OFFSET` windows;
- qualifier/reified-statement queries when the manifest has not certified those
  representation layers;
- unanchored global scans that the current routing metadata cannot exhaustively
  cover.

Property paths with an anchored endpoint may run over the resident graph, but
are explicitly marked `recursive-path` and incomplete until routed closure is
certified.

SPARQL currently creates temporary pattern nodes for some joins. Partial query
execution therefore also runs inside an ephemeral cluster. The cluster is
removed after the query and the graph fingerprint must return to its pre-query
value. Result output survives; query scaffolding does not.

## 3. Query-specific completeness

Completeness is a property of a query and a dataset manifest, not merely a
boolean property of the process.

For a query `Q`, loaded graph `G_p`, and manifest/routing contract `M`, define:

```text
Coverage(Q, G_p, M) in {Complete, Incomplete, Unknown}
```

A partial database can completely answer a narrowly anchored query when the
manifest proves that every shard capable of matching its obligations was
loaded. Conversely, a large resident graph cannot certify an unrestricted
variable scan without exhaustive routing metadata.

A completeness certificate binds at least:

- normalized query plan;
- dataset identity and version;
- manifest and routing-index versions/hashes;
- required and successfully loaded shard sets;
- representation layers used;
- failed loads and resource limits;
- evaluation fixed-point status;
- routing fixed-point status.

## 4. Progressive routing and evaluation

The routed executor maintains:

```text
(G_n, B_n, F_n, U_n)
```

where `G_n` is the loaded graph, `B_n` the current bindings, `F_n` the active
join/path frontier, and `U_n` unresolved routing obligations.

Each arriving shard is useful immediately:

```text
load shard
  -> validate and index
  -> propagate only its delta through the query plan
  -> emit newly justified rows
  -> derive and batch new routing obligations
  -> reprioritize pending shards
```

There is no global barrier that waits for every shard discovered so far.
Monotone result batches can be emitted permanently while completeness remains
uncertified.

The routing index must be exhaustive for each obligation key `k`:

```text
Matches(k, G) subset-of union(Shard(s) for s in route(k))
```

A query is complete only when all of the following hold:

1. evaluation produces no new bindings or frontier nodes;
2. no relevant shard remains pending or in flight;
3. no routing obligation remains unresolved or unroutable;
4. every required representation layer is certified;
5. the manifest/routing index is exhaustive;
6. no relevant shard failed and no resource limit terminated execution.

This is the joint evaluation-and-routing fixed point. Reaching a fixed point in
only the currently resident graph is insufficient.

## 5. Progressive runtime foundation

`network/partial_query_runtime.hpp` implements the state machine used by a
future native SPARQL routing adapter:

- deduplicated routing obligations;
- many-obligations-to-one-shard batching;
- cache/cost/yield-aware shard prioritization;
- pending, in-flight, loaded, and failed shard states;
- per-shard acceptance without a global wait barrier;
- explicit stop/resource-limit reasons;
- completion certificates implementing the joint fixed-point conditions.

It is deliberately transport-neutral. Candidate shards can resolve to local,
HTTP/Hugging Face, IPFS, or another sink after logical routing.

The runtime foundation does **not** claim that `sparql.zph` already drives
manifest fetches itself. The remaining integration boundary is a native adapter
that exposes query obligations and loaded shard deltas to the Janet SPARQL
planner. Until that adapter is connected, interactive partial SPARQL uses the
resident-slice contract above and never claims global completeness.

## 6. Hosted artifact contract

`docs/schema/partial-query-manifest-v1.schema.json` specifies the minimum
machine-readable contract for trustworthy routed queries:

- immutable dataset identity/version;
- partitioning and overlap semantics;
- shard IDs, sizes, hashes, and transport references;
- subject/predicate/key ranges;
- direct-claim, statement, qualifier, reference, and rank coverage;
- routing-index identities and exhaustiveness declarations;
- format compatibility versions.

Fallback sequential loading must be reported as a distinct execution path. It
cannot silently satisfy a production completeness certificate unless the
fallback itself is specified, validated, and exhaustive.

## 7. Acceptance matrix

Production routed SPARQL must test both answer correctness and status
correctness:

- resident answer present;
- relevant answer located in an initially unloaded shard;
- progressive anchored join crossing several shards;
- forward and backward property paths crossing shard boundaries;
- qualifier query with and without certified qualifier coverage;
- refusal/finalization barrier for `MINUS`, absence, aggregate, and ordered
  window queries;
- corrupt shard/index, manifest mismatch, stale cache, and failed fetch;
- shard/byte/time budget exhaustion with unresolved-obligation counts;
- equivalence between a full-graph answer and a routed answer carrying a
  completeness certificate.
