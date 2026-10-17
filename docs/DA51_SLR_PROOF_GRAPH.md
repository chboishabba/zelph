# DA51 × SLR proof-linked shard consumer boundary

Date: 2026-10-17

Zelph remains a selector/query consumer of logical artifacts and shards. DA51/formal-proof provenance is optional metadata on those logical artifacts; it does not replace the selector model or grant proof authority.

Resolution order stays:

```text
logical selector
 -> logical shardId
 -> validate artifactRevision/contentDigest
 -> choose objectRef sink (HF/IPFS/file)
 -> expose optional DA51/demand/evidence provenance
```

Optional provenance fields may include `da51Address`, `formalDemandRef`, `formalEvidenceRef`, and `statementDigest`.

Hard boundaries:

```text
selector != theorem query authority
DA51 status/address != proof
formalEvidenceRef != local proof payment
IPFS CID/HF path != shard semantic identity
sink availability != artifact truth
runtime evidence != theorem/security conclusion
```

If a formal-evidence view is exposed, the exact payment coordinate is `demand_ref + obligation_ref + statement_digest`; Zelph may display or filter on it but must not loosen or recompute that relation.
