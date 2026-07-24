This changes the interpretation substantially.

## What Stefan is actually saying

Stefan is being warm and deliberately non-demanding, but beneath that is a **very concrete engineering request**:

> Please make sharding + SPARQL genuinely build and run, ideally before Tuesday, July 28, 2026.

He is not merely asking for architecture, documentation, a classifier, or a runtime foundation. He wants a demonstrable end-to-end path:

```text
load partial/sharded Wikidata
→ import SPARQL
→ submit familiar SPARQL
→ fetch/load needed shards
→ return results
```

The invitation to join the Wikidata meeting is meaningful. He is offering:

* a real user community;
* an imminent demonstration target;
* direct technical collaboration;
* public credit and an opportunity to explain the work;
* tolerance if it is not ready.

But “no pressure” does not mean “no request.” It is his way of making a significant request without making the relationship transactional.

## The uncomfortable technical correction

PR #4 was merged into `codex/hf-cache` on **July 22, 2026**, despite explicitly saying that it did not claim a full repository build and did not yet connect the SPARQL planner to live shard fetching.

So Stefan’s account is accurate:

1. He tried the branch.
2. It did not build.
3. The PR was later merged.
4. Even if it built, it explicitly stopped before the feature he was asking to demonstrate.

The PR contains useful architectural work, but it was presented too close to “implemented” in our conversation. The honest description should have been:

> I added an unbuilt design/prototype for resident-slice SPARQL safety and a routing state machine, but the repository has not compiled it, and query-driven shard loading is not connected.

That distinction matters because Stefan was preparing to try it before a meeting.

There is also a process failure: **an unbuilt 2,000-line change was merged into the working sharding branch**. The PR added 2,144 lines, deleted 467, and touched core REPL code, yet no full build had run.  That should now be treated as a regression to repair, not a successful completed feature.

## Socially, Stefan handled this exceptionally well

He does not accuse you of wasting his time or overstating the state. Instead, he says:

> “that all lines up, nothing surprising.”

That is generous face-saving language. He is letting you correct course without embarrassment.

He then establishes why he wants **you**, specifically:

> “you’re the author of the sharding”

That is partly technical and partly relational. He sees this as your subsystem and does not want to fork the conceptual ownership or duplicate it.

His line:

> “you wrote in yesterday’s meet chat that you’d tried it and it works”

is gentle but important. It records a mismatch between the public claim and the reality. I would read it as:

> People may now expect a working demonstration, so please understand why completing or correcting this matters.

It is not hostile. But it is a reputational nudge.

## What he values technically

His comments also suggest that the earlier architecture may have been somewhat overengineered relative to his immediate need.

He emphasizes that:

* SPARQL is a `.zph` language extension;
* the `sparql` keyword is dynamically registered;
* the grammar lives in Janet PEG;
* Zelph syntax and Janet syntax are intentionally composable;
* native Zelph syntax translates into the Janet API.

That strongly suggests he expects the fix to respect Zelph’s existing extensibility model rather than moving excessive policy and SPARQL semantics into the C++ REPL layer.

The shortest likely useful sequence is therefore:

1. **Restore a clean build first.**
2. Audit why `sparql.zph` is blocked in partial mode.
3. Permit its registration through the smallest principled capability mechanism.
4. Confirm ordinary SPARQL works over the currently resident partial graph.
5. Connect the existing manifest/router to the SPARQL access primitives:

   * `zelph/targets`
   * `zelph/sources`
   * closures/path traversal
   * perhaps the general query primitive.
6. Load relevant shards when those primitives encounter an unresolved routed key.
7. Re-evaluate incrementally or restart the affected operation.
8. Demonstrate one narrow cross-shard query.
9. Only then add sophisticated completeness classification and certificates.

The previous branch inverted some of this ordering: it built an elaborate semantic framework before proving that the repository compiled or that one routed query worked.

## What “ready by Tuesday” realistically means

A credible Tuesday demonstration does **not** require the full formal system described in PR #4.

A strong demonstration could be narrowly scoped to:

```sparql
SELECT ?place WHERE {
  wd:Q123 wdt:P19 ?place .
}
```

or a two-step join where:

* the subject is initially resolvable;
* the required adjacency shard is located by the existing route index;
* the shard is loaded on demand;
* the query returns the correct row;
* diagnostic output shows which shard was fetched.

A second demonstration could show an anchored property path, but only after direct subject/predicate lookup works.

It would be perfectly respectable to say during the talk:

> Positive anchored queries are routed on demand today. Global scans, exact negation, aggregates, and certified recursive closure are future work.

That is much stronger than having a comprehensive framework that does not build.

## Strategic significance

Stefan is offering evidence that Zelph has a potential adoption wedge:

```text
familiar SPARQL interface
+
Wikidata-scale selective loading
+
local/native symbolic execution
```

That combination is much easier for the Wikidata community to understand than Zelph’s broader conceptual pitch. SPARQL acts as a compatibility layer: users can evaluate the engine through a language they already trust.

His frustration that “no one is trying it” also means a working sharded-SPARQL demo could be unusually valuable. It transforms Zelph from something the group must conceptually understand into something they can test using their existing queries.

## My bottom-line reading

Stefan is:

* genuinely enthusiastic about your work;
* treating you as a collaborator rather than a contractor;
* giving you an unusually forgiving opportunity to repair an overclaimed implementation;
* signalling that this feature has a real audience;
* asking for a working vertical slice, not another architecture document;
* subtly reminding you that “it works” was already said publicly.

The correct response is not defensiveness or a long explanation of why the prior design was reasonable. It is something like:

> You’re right—I overstated the branch state. The architecture was there, but it was neither repository-built nor connected end to end. I’ll treat the current merge as broken until it compiles, then target the smallest real routed-SPARQL vertical slice. I won’t call it working again until I have run the exact checkout/build/query path Stefan can reproduce.

That would align extremely well with the tone he has offered.


Yes. The existing ITIR state actually makes your explanation to Stefan stronger and more precise.

The key distinction is:

[
\boxed{
\text{Zelph is already present in ITIR, but it is not the core semantic compiler.}
}
]

That is materially different from either:

* “ITIR does not use Zelph”; or
* “ITIR is built on Zelph.”

Neither would be accurate.

## The current architecture, as described

The main legal-semantic compilation lane is approximately:

[
\begin{aligned}
&\text{persisted sources}\
&\to \text{canonical projection}\
&\to \text{spaCy annotation}\
&\to \text{parser observations}\
&\to \text{base factor/proposal generation}\
&\to \text{base graph reduction}\
&\to \text{composition proposals}\
&\to \text{compositional graph reduction}\
&\to \text{constraints, revisions, residuals}\
&\to \text{Legal IR projection}\
&\to \text{PostgreSQL persistence}.
\end{aligned}
]

Zelph is currently outside that critical path.

Its active role is narrower:

[
\text{fact intake}
\quad+\quad
\text{Wikidata structural candidate generation}
\quad+\quad
\text{optional bounded inference}.
]

That is a sensible architectural boundary. Zelph is not currently entrusted with defining what the legal semantics mean. It operates on already represented facts or ontology structures and helps infer, inspect, or generate candidates.

## This resolves the apparent contradiction

You said:

> I had been planning to build or find something like Zelph for ITIR.

That is still basically true at the architectural level.

You already have pieces of the desired reasoning layer, but they are distributed:

* spaCy provides observations;
* PNF turns observations into structured alternatives and factors;
* graph reduction resolves or preserves those alternatives;
* Legal IR gives a stable projection;
* PostgreSQL persists the result;
* Zelph is used in a separate bounded-inference lane.

So ITIR has not yet made Zelph—or any one component—the universal reasoning substrate. You are currently consolidating several implementation lanes that independently approximate parts of that intended product.

A more exact statement would be:

> I had been planning to build or adopt a general symbolic reasoning substrate for ITIR. Zelph is already used in some bounded inference and Wikidata-review lanes, but it is not currently underneath the main PNF or Legal IR compiler.

That explains both why Zelph is familiar and why the integration still matters.

## The current Zelph role is probably the right starting role

The existing use cases are low-risk:

* fact-review assistance;
* candidate generation;
* ontology inconsistency detection;
* Wikidata disjointness scans;
* optional demonstrations.

Those are appropriate places for a reasoning system whose conclusions still need review.

They preserve the authority boundary:

[
\text{Zelph inference}
\neq
\text{canonical legal meaning}.
]

Instead, something closer to:

[
\text{Zelph output}
\to
\text{proposal or evidence}
\to
\text{PNF reduction / review / acceptance}.
]

That is likely the correct integration principle even if Zelph becomes much more central later.

## How Zelph could fit the main pipeline

I would not place Zelph before spaCy. It does not replace the observation layer unless you build an entirely new parser in it.

The most natural insertion points are after proposal construction and after persistence.

### 1. Proposal-reduction backend

After parser-derived proposals exist:

[
P_{\mathrm{base}}
=================

\operatorname{propose}(\text{parser observations}),
]

Zelph could derive:

* equivalence candidates;
* implication candidates;
* contradiction candidates;
* ontology compatibility;
* transitive relations;
* missing-link candidates;
* cross-document identity hypotheses.

Its output should become additional proposals or constraints:

[
P' =
P_{\mathrm{base}}
\cup
\operatorname{zelphInfer}(P_{\mathrm{base}},W),
]

where (W) is relevant Wikidata or domain knowledge.

Then the normal reduction system remains responsible for deciding what survives:

[
G =
\operatorname{reduce}(P').
]

This keeps Zelph useful without making it authoritative.

### 2. Post-compilation consistency checker

After PNF or Legal IR construction, serialize a bounded logical view into Zelph:

[
Z =
\operatorname{encode}*{\mathrm{Zelph}}(G*{\mathrm{PNF}}).
]

Run checks for:

* contradictions;
* impossible combinations;
* missing expected consequences;
* cross-document inconsistency;
* conflict with selected Wikidata relations.

Return findings to the failure ledger or review workbench.

This aligns closely with the existing fact-intake usage.

### 3. Query and retrieval substrate

Your conceptual-search examples are especially natural:

* every reference to the United States, irrespective of surface form;
* every passage involving a pet as an entity or superclass;
* every document claiming some relation involving a person, place, office, or event;
* traversals through Wikidata classes and properties.

Here Zelph plus SPARQL could expose the structured corpus as a queryable knowledge graph:

[
\text{text}
\to
\text{PNF/IR entities and relations}
\to
\text{Wikidata-linked graph}
\to
\text{SPARQL or Zelph inference}.
]

The important point is that spaCy and PNF produce the graph; Zelph and SPARQL help interrogate or extend it.

### 4. Learner feedback lane

The country predictor or a Wikidata-repair model could produce scored candidates:

[
(\text{candidate},p,\text{provenance}).
]

Those should enter the same proposal system as parser candidates, not bypass it:

[
P_{\mathrm{learned}}
\to
\operatorname{reduce}
\left(
P_{\mathrm{parser}}
\cup P_{\mathrm{symbolic}}
\cup P_{\mathrm{learned}}
\right).
]

That fits the two-reduction architecture you described:

1. base reduction of direct observations;
2. compositional reduction of derived and externally proposed structure.

You may eventually want a third explicit boundary:

[
\text{external enrichment reduction},
]

covering Wikidata, Zelph, learned predictions, vector retrieval, and LLM-suggested links.

But it could initially be part of compositional reduction, provided provenance remains explicit.

## The timing instrumentation matters here

Your phase-timing discussion is not incidental. Before deciding whether Zelph should replace or augment any stage, you need to know where cost actually occurs.

The nested timings should distinguish:

[
\begin{aligned}
T_{\mathrm{parse}} &=
T_{\mathrm{spaCy}}
+
T_{\mathrm{relational\ bundle}},\
T_{\mathrm{base}} &=
T_{\mathrm{proposal}}
+
T_{\mathrm{base\ reduction}},\
T_{\mathrm{composition}} &=
T_{\mathrm{composition\ proposal}}
+
T_{\mathrm{composition\ reduction}},\
T_{\mathrm{final}} &=
T_{\mathrm{constraints}}
+
T_{\mathrm{LegalIR}}
+
T_{\mathrm{persistence}}.
\end{aligned}
]

Later, Zelph should get its own timed and attributed phase:

```text
external_inference
  backend=zelph
  input_factors=...
  generated_proposals=...
  accepted_proposals=...
  rejected_proposals=...
  elapsed=...
```

Likewise for a learned model:

```text
external_inference
  backend=country_predictor
  model_version=...
  candidates=...
  threshold=...
```

That keeps backends replaceable and avoids turning “Zelph” into a semantic stage name.

## The reproducibility word you were looking for

From the Nix comparison, the likely words are:

* **declarative**
* **reproducible**
* **hermetic**
* **deterministic**
* **content-addressed**, depending on the mechanism

For ITIR, I think the intended property is:

> a deterministic, provenance-preserving, reproducible semantic compilation pipeline.

“Reproducible” alone may be insufficient because model or parser versions can still change outcomes. The stronger design is:

[
\operatorname{Compile}
(
\text{source hash},
\text{compiler version},
\text{model versions},
\text{ontology version},
\text{configuration}
)
\to
\text{versioned semantic artifact}.
]

That is very Nix-like.

## What this means for your reply

I would revise the earlier line:

> Most of the current ITIR implementation does not use Zelph—an older lane did.

That is now slightly misleading because there are active present-day uses.

A more accurate version is:

To be more precise about the existing ITIR state: Zelph is already used in a couple of bounded lanes. The fact-intake review model can run Zelph-backed inference, and some Wikidata structural/disjointness tooling invokes the Zelph CLI for candidate generation and assessment.

It is not currently part of the principal PNF or Legal IR compilation path, though. The main compiler is still roughly spaCy observations → factor/proposal generation → graph reduction → compositional proposals and reduction → constraints/residuals → Legal IR → PostgreSQL.

So I had not forgotten that Zelph existed in the repo so much as mentally separated those optional inference/review lanes from the central semantic compiler. I am currently trying to consolidate these somewhat disparate lanes into one coherent product.

My interest is in whether Zelph can become a general inference and query substrate around the PNF graph without becoming an unreviewed semantic authority. For example, it could contribute ontology links, implications, contradiction candidates, Wikidata repairs, or learned predictions as provenance-bearing proposals, while the existing reduction system decides what is actually accepted into the canonical graph.

That is also why the country-prediction example interests me. A learner could produce scored proposals, Zelph could express or test their logical consequences, and ITIR could reduce them alongside parser-derived and Wikidata-derived candidates. Then the resulting structured corpus could be queried through SPARQL, MCP, database access, vector retrieval, or an LLM.

The overall picture is coherent: **PNF is the semantic compiler; Zelph is currently an inference adjunct; SPARQL is a compatibility/query surface; and learned models are potential proposal producers.** The consolidation task is to make those relationships explicit rather than replacing everything with one subsystem.


The HF/partial-load feature set should be organised around one concrete product promise:

[
\boxed{
\text{Given a query over a hosted dataset, load only the necessary verified shards,}
\
\text{produce useful results progressively, and state exactly whether the result is complete.}
}
]

Everything else is supporting infrastructure.

## P0 — Restore a trustworthy build baseline

This comes before further architecture.

### Deliverables

* Fix the current clean-build failure around `ankerl/unordered_dense.h`.
* Build the exact merged `codex/hf-cache` branch from a fresh checkout.
* Run the full Zelph test suite, not only isolated header tests.
* Add CI for:

  * Linux release build;
  * Linux debug build;
  * tests;
  * clean standard-library packaging;
  * the partial-load/SPARQL acceptance path.
* Ensure the HF/partial changes do not depend on undeclared transitive `PRIVATE` include paths.
* Add one reproducible command:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DZELPH_BUILD_TESTS=ON

cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Exit condition

A fresh environment can compile and run:

```text
.load-partial ...
.import sparql
sparql
SELECT ...
```

No further feature should be described as implemented before this passes.

---

## P1 — Make the hosted artifact contract real

The schema currently documents the desired contract, but query correctness depends on actual produced and validated artifacts. The existing design already recognises that manifest identity, hashes, routing indexes, representation layers, and fallback behaviour are correctness inputs rather than optional metadata.

### Deliverables

A canonical manifest for every hosted dataset containing:

* dataset ID;
* immutable dataset version or source revision;
* manifest schema version;
* manifest hash;
* graph format version;
* partitioning scheme;
* overlap/disjointness semantics;
* shard IDs;
* shard byte sizes;
* cryptographic checksums;
* shard transport URIs;
* routing-index IDs and format versions;
* declared routing exhaustiveness;
* representation-layer coverage:

  * direct claims;
  * statement nodes;
  * main snaks;
  * qualifiers;
  * references;
  * ranks;
  * names.

The producer should generate, in one deterministic operation:

```text
dataset.bin or source data
├── manifest.json
├── shards/
├── routing/
│   ├── subject.idx
│   ├── subject-predicate.idx
│   ├── predicate.idx
│   ├── object-predicate.idx
│   └── names.idx
└── checksums
```

### Validation requirements

The loader must reject:

* mismatched dataset versions;
* unsupported formats;
* invalid checksums;
* missing required sidecars;
* routing indexes for another manifest;
* stale cached files;
* undeclared representation layers;
* silent fallback to another loading path.

### Exit condition

A hosted manifest can be fetched, validated, and used to load one named shard with no sequential source-file fallback.

---

## P2 — Consolidate the HF fetch/cache layer

HF transport should become one implementation of a generic verified object loader, not logic embedded throughout partial loading.

### Deliverables

A transport-neutral interface approximately equivalent to:

```cpp
struct ObjectRequest {
    std::string uri;
    std::string revision;
    std::string expected_digest;
    uint64_t offset;
    uint64_t length;
};

struct VerifiedObject {
    std::filesystem::path local_path;
    uint64_t byte_size;
    CacheDisposition cache_disposition;
};
```

Implementations:

* local file;
* `hf://`;
* ordinary HTTPS where useful;
* later IPFS without changing query semantics.

### Cache identity

Cache identity must include:

[
(\text{dataset version},
\text{object URI},
\text{revision},
\text{range},
\text{digest}).
]

An ETag alone is insufficient as the permanent identity.

### Operational requirements

* bounded connect and transfer timeouts;
* retries only for suitable failures;
* resumable or range-based fetches;
* atomic cache writes;
* lock or single-flight handling for concurrent requests;
* negative-cache expiry;
* checksum validation before publication;
* explicit offline-cache reuse;
* LRU or quota-based eviction;
* telemetry for:

  * probe time;
  * time to first byte;
  * transfer rate;
  * cache hit/miss;
  * verification time;
  * retries and failures.

### Exit condition

Multiple concurrent obligations for the same shard trigger one fetch, all consumers receive the same verified cached object, and stale/corrupt cache entries are rejected.

---

## P3 — Implement the native routing service

This is the first missing functional bridge.

The current repository has a progressive runtime foundation, but its documentation explicitly says the native adapter exposing query obligations to the Janet SPARQL planner is still absent.

### Deliverables

A native service exposed to Janet, approximately:

```text
route-subject(node, layer)
route-subject-predicate(node, predicate, layer)
route-object-predicate(node, predicate, layer)
route-predicate(predicate, layer)
route-name(language, name)
request-shards(obligations)
await-next-shard(query-id)
query-coverage(query-id)
```

Each routing result should include:

* candidate shard IDs;
* whether routing is exhaustive;
* relevant representation layers;
* estimated byte cost;
* cache status;
* routing-index identity.

### Obligation model

At minimum:

```cpp
struct RoutingObligation {
    Direction direction;
    optional<Node> subject;
    optional<Node> predicate;
    optional<Node> object;
    RepresentationLayer layer;
    QueryOperatorId consumer;
};
```

Obligations need stable identities so they can be:

* deduplicated;
* batched;
* resolved by several overlapping shards;
* associated with the operator that generated them;
* included in completion certificates.

### Exit condition

Given `(Q123, P19)`, the router deterministically returns every shard that can contain matching outgoing edges and states whether that answer is exhaustive.

---

## P4 — Build one end-to-end routed SPARQL vertical slice

Do not begin with the whole SPARQL subset. Implement the smallest real query path.

### Initial supported query class

Positive, monotone, anchored basic graph patterns:

```sparql
SELECT ?place WHERE {
  wd:Q123 wdt:P19 ?place .
}
```

Then:

```sparql
SELECT ?country WHERE {
  wd:Q123 wdt:P19 ?place .
  ?place wdt:P17 ?country .
}
```

### Required behaviour

For each triple operator:

1. inspect currently resident indexed edges;
2. emit existing bindings;
3. derive missing routing obligations;
4. request deduplicated shards;
5. accept each shard independently;
6. integrate it into the query-visible graph;
7. evaluate only affected operators;
8. emit new bindings immediately;
9. derive further obligations;
10. stop at the routing/evaluation fixed point or a resource limit.

The existing design correctly defines the desired loop as progressive and without a global shard barrier.

### Important implementation decision

Initially, it is acceptable to re-evaluate the affected BGP or query after each shard if that is much simpler.

Do not block delivery on perfect semi-naive delta propagation.

The sequence should be:

[
\text{correct restartable execution}
\rightarrow
\text{operator-local reevaluation}
\rightarrow
\text{true delta propagation}.
]

### Exit condition

A test starts with the answer-containing shard unloaded, runs a SPARQL query, observes that shard being fetched, and obtains the same answer as a full-graph execution.

---

## P5 — Progressive results and query status protocol

Rows and completeness must be separate.

### Deliverables

A structured event protocol:

```text
query_started
routing_obligations_added
shard_fetch_started
shard_loaded
result_batch
coverage_changed
query_completed
query_cancelled
query_failed
```

Example result event:

```json
{
  "type": "result_batch",
  "query_id": "q-123",
  "rows": [
    {"place": "Q100"}
  ],
  "row_contract": "sound-lower-bound",
  "result_set_status": "incomplete"
}
```

Final event:

```json
{
  "type": "query_completed",
  "completeness": "complete",
  "evaluation_fixed_point": true,
  "routing_fixed_point": true,
  "loaded_shards": 3,
  "failed_shards": 0,
  "unresolved_obligations": 0
}
```

### Resource-limited completion

Support:

* shard-count limit;
* byte limit;
* elapsed-time limit;
* path-depth limit;
* result-row limit;
* cancellation.

Then report:

```text
completeness: incomplete-known
reason: byte-budget-exhausted
```

The completion requirements should remain exactly as currently documented: no new evaluation work, no relevant pending/in-flight shard, no unresolved routing obligation, certified representation coverage and exhaustive indexes, and no failed relevant load.

### Exit condition

A caller can consume the first valid result before all required shards have loaded and later receive either a completeness certificate or an explicit incompleteness reason.

---

## P6 — Property-path routed closure

Only after ordinary joins work.

### Deliverables

Support anchored paths such as:

```sparql
wd:Q123 wdt:P279* ?ancestor
```

and:

```sparql
?subclass wdt:P279* wd:Q5
```

Maintain:

[
(V_n,F_n,U_n,L_n)
]

where:

* (V_n): visited nodes;
* (F_n): active frontier;
* (U_n): unresolved edge obligations;
* (L_n): loaded shards.

For every new frontier node, route its relevant outgoing or incoming predicate edges.

### Exit condition

A multi-hop path whose edges reside in several initially unloaded shards returns the same closure as full-graph execution and certifies that every visited node’s relevant edge space was exhaustively routed.

---

## P7 — Representation-layer-aware Wikidata support

This is necessary before claiming useful Wikidata parity.

### Deliverables

Map query namespaces to layer requirements:

| Query form      | Required layer |
| --------------- | -------------- |
| `wdt:P…`        | direct claim   |
| `p:P…`          | statement      |
| `ps:P…`         | main snak      |
| `pq:P…`         | qualifier      |
| `pr:P…`         | reference      |
| `wikibase:rank` | rank           |

The planner should:

1. infer required layers from the parsed query;
2. check manifest coverage;
3. route the correct layer shards;
4. refuse or report unknown coverage when absent.

### Exit condition

A qualifier query cannot silently return an empty set when qualifier data is not included. It either loads the qualifier layer or returns an explicit coverage error.

---

## P8 — Operator-specific semantic safety

The current resident-slice policy already distinguishes safe lower-bound queries from unstable forms.

The next step is to replace blanket refusal with operator finalisation barriers.

### Priorities

#### Positive monotone BGPs

* stream rows immediately;
* complete at routed fixed point.

#### `COUNT`

Initially:

```text
count_lower_bound: N
final: false
```

Exact only after complete coverage.

#### `ORDER BY` / `LIMIT`

* progressive candidates permitted;
* final ordered window withheld until coverage is complete.

#### `MINUS`, `NOT EXISTS`, absence-sensitive `OPTIONAL`

* no globally final negative conclusion before exhaustive relevant coverage;
* either withhold the affected rows or mark them provisional and retractable.

### Exit condition

Every supported operator declares:

* what may be streamed before completion;
* whether outputs can be retracted;
* what coverage evidence permits finalisation.

---

## P9 — Performance and observability

Only optimize after the vertical slice is measured.

### Timed phases

Per query:

```text
parse
plan
initial resident evaluation
obligation generation
routing lookup
cache lookup
network fetch
checksum verification
shard decode
graph integration
incremental/restarted evaluation
result serialization
completion certification
```

Per shard:

```text
routing_score
queued_duration
fetch_duration
decode_duration
bindings_unblocked
new_obligations_generated
```

### Metrics that matter

* time to first row;
* time to complete result;
* bytes fetched;
* shards fetched;
* cache hit ratio;
* bindings per fetched MiB;
* obligations per shard;
* duplicate obligations eliminated;
* resident-versus-routed evaluation cost;
* peak memory;
* unresolved obligations on termination.

### Exit condition

For every benchmark query, the engine can explain whether time was spent in routing, transfer, decoding, graph insertion, or query evaluation.

---

## P10 — ITIR integration

This should consume the feature set rather than distort Zelph around ITIR-specific semantics.

### Deliverables

#### A. ITIR graph export

Export a selected PNF/IR slice into a Zelph-queryable representation with:

* stable document IDs;
* source-span provenance;
* Wikidata entity IDs;
* relation/factor types;
* proposal status;
* confidence or evidential weight;
* compiler/artifact version.

#### B. Corpus routing

Shard by a combination of:

* document or source;
* linked entity;
* predicate/relation;
* representation type.

A query such as:

```text
all passages referring to the United States
```

should route through the linked `Q30` entity, not scan every source string.

#### C. Inference remains proposal-producing

Zelph or learned results should enter ITIR as:

```text
external proposal
+ provenance
+ backend/model version
+ score/evidence
```

not directly mutate canonical PNF/Legal IR.

#### D. MCP/query interface

Expose:

* SPARQL query;
* structured Zelph query;
* entity/concept lookup;
* provenance retrieval;
* completeness status.

### Exit condition

An ITIR corpus query can fetch only relevant hosted graph shards, return linked text spans and logical relations, and clearly distinguish:

* canonical compiler output;
* Wikidata enrichment;
* Zelph-derived inference;
* learned proposals.

---

# Recommended implementation order

The shortest meaningful sequence is:

[
\boxed{
P0 \rightarrow P1 \rightarrow P2 \rightarrow P3 \rightarrow P4 \rightarrow P5
}
]

That yields a real demonstrable product:

1. it builds;
2. hosted artifacts are trustworthy;
3. HF objects are fetched and cached correctly;
4. queries produce routing obligations;
5. simple SPARQL actually pulls shards;
6. results stream with honest status.

Then:

[
\boxed{
P6 \rightarrow P7 \rightarrow P8 \rightarrow P9 \rightarrow P10
}
]

adds:

* cross-shard closure;
* full Wikidata representation support;
* more SPARQL semantics;
* optimization;
* ITIR integration.

## The immediate cutset

The next five concrete engineering tasks should therefore be:

1. **Fix the merged clean build and run all tests.**
2. **Produce one canonical hosted test artifact with manifest, hashes, routing indexes, and two or three deliberately separated shards.**
3. **Expose subject/predicate routing and verified shard loading through the Janet API.**
4. **Modify one SPARQL triple-pattern evaluator to request and consume those shards progressively.**
5. **Add a full-graph-versus-routed equivalence test that starts with the required answer shard unloaded.**

Once those five pass, “partial SPARQL that pulls shards on demand” becomes a truthful description rather than a design target.
