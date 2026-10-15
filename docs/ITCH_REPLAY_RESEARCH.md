# ITCH replay feasibility for FifoFill

Investigated 2026-10-15. This is a design investigation, not a completed replay experiment.

**Recommendation:** keep synthetic differential testing and benchmarks as the core deliverable. Treat ITCH as an optional extension with two separate outputs: a reconstructed displayed source book, and a clearly labeled **ITCH-derived matching workload**.

## Verified protocol facts

ITCH numbers are big-endian; ordinary prices have four decimal places. Timestamps count nanoseconds since midnight; locates are daily. See [ITCH 5.0, Data Types](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf). The mapping below consolidates the remaining message facts needed here.

Nasdaq describes displayed orders as price/time prioritized, with hidden shares following displayed shares at the same price. It also supports functionality outside FifoFill's model, including minimum quantities, midpoint pegs, self-match prevention, and auctions. Sharing the price/time label does not make the two engines equivalent. See [Nasdaq order types and functionality](https://nasdaqtrader.com/Trader.aspx?id=TradingUSEquities).

For comparison, Nasdaq's actual order-entry protocol includes a submitted quantity, price, time-in-force, display instruction and optional modifiers. Its replacement request also uses different quantity semantics from FifoFill's proposed `new_remaining`. See [OUCH 5.0 §§2.1–2.2](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/Ouch5.0.pdf). **Design inference:** recovering such instructions requires evidence of those inputs; matching one observed reduction is insufficient. This project should not implement OUCH.

## Why the naive translator fails

Consider an educational book with asks `S1: 10 @ 100` followed by `S2: 10 @ 100`. If an observed fill targets S2, sending a buy market order for 10 to FifoFill consumes S1. An adapter that then ignores a later cancellation of S1 has concealed the first divergence and produced a different workload. This example is an adversarial translator test, not a claim that ordinary Nasdaq displayed FIFO orders execute in that sequence.

This test follows directly from the project's FIFO rule. It must fail before another command is generated; later errors must not be treated as harmless noise.

## Proposed architecture and mapping

Keep parsing and source reconstruction outside `ReferenceBook::process` and `FastBook::process`. The source component applies observed deltas without invoking matching. The matching component receives only generated commands that satisfy FifoFill's public contract. Never apply the matching engine's “book must not cross” assertion indiscriminately to the source reconstruction.

Proposed mapping, using [ITCH 5.0 §§1.3–1.5](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf):

| Record | Proposed command generation / guard |
|---|---|
| A/F | Limit add; must rest unchanged. |
| E | Opposite market; target must be best-price FIFO head with sufficient quantity. |
| C | Alternate execution price: end segment. |
| X | Subtract canceled quantity; shrink or cancel. |
| D | Cancel. |
| U | Cancel old; add new ID at tail. |
| P/Q/B | Hidden/cross/broken reports: no generated commands; B never restores book quantity. |

Validate the adapter against the source after every translated record. Unsupported book mutations must end the segment, not be silently dropped. Continue source reconstruction so diagnostics remain meaningful. Restart a segment only from a documented synchronized state: initially prefer an empty source book; add snapshot bootstrap only if useful coverage requires it. Bootstrap work and its artificial commands stay outside measured intervals.

**Proposed boundary policy:** end derived segments on session/epoch changes, input gaps, halt entry/exit, and auction transitions. A halt does not justify clearing reconstructed orders: continue source processing, then require synchronization before a new segment. A known new epoch starts fresh session-local maps; an unexplained gap requires reconstruction from an earlier trusted boundary or snapshot. Never stitch incompatible intervals into one continuous benchmark trace.

Use an explicit source-ID-to-engine-ID map and a deterministic allocator for invented aggressor IDs. Do not assume a bit range is unused in historical IDs. Maintain a source book from session start even when only a later time interval is measured. Choose one symbol for the first experiment, retaining its prerequisite records and system/trading-state context.

**Alternative:** a structural replay benchmark applies external adds, decrements, deletes and replacements directly. This measures parsing/book maintenance, not matching throughput; give it a separate result table. It should not require a back door in the core matching API.

## Input handling and price compatibility

BinaryFILE wraps each payload in a two-byte big-endian length. A zero-length record terminates the session; EOF without that marker indicates incomplete data under the format. The wrapper is separate from an ITCH message and from real-time transport packets. See [BinaryFILE 1.00](https://nasdaqtrader.com/content/technicalSupport/specifications/dataproducts/binaryfile.pdf).

Recommended parser checks: exact supported payload length, short length prefix, short payload, invalid field values, missing active IDs, duplicate live IDs, reduction underflow, and replacement collisions. Keep file order; do not sort by timestamps or references. Use byte decoding rather than packed-struct casts. Report byte offset and record index on failure. Unknown record types require an explicit allow/deny policy, not accidental acceptance.

Follow the implementation plan's configurable integer-unit contract. Declare the source-to-engine scale in the manifest and permit only exact conversions; reject and count incompatible prices rather than rounding. Establish price bounds and maximum live orders before timing; reject an insufficient configuration rather than silently discard records. Stream decompression outside engine timing, or report an explicitly separate end-to-end metric. The adapter must use explicit cancel for zero remaining quantity and satisfy the core's duplicate-ID and capacity rules.

## Sample availability

On 2026-10-15 the [official Nasdaq sample directory](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/) listed downloadable files including `12302019.NASDAQ_ITCH50.gz` (3,524,013,057 compressed bytes), `01302020.NASDAQ_ITCH50.gz` (5,597,158,940 bytes), and several files uploaded during 2026. Some older dates have checksum files without corresponding data files. Listing availability is not a guarantee of future availability or proof of redistribution permission.

No data file was downloaded for this investigation. Choose a documented ITCH 5.0 file, record URL/date/size/checksum, and keep large data out of Git and CI. Do not infer encoding from `.txt.gz` alone. Prefer hand-authored tiny protocol fixtures in the repository.

## Small spike and go/no-go decision

Time-box the first experiment to one or two development sessions after the engine is correct:

1. Build tiny binary fixtures for framing, A/F, E/C, X/D/U, and the reporting-only records. Include truncated input, quantity underflow, replacement identity, FIFO incompatibility, alternate execution prices, epoch resets, and halt/auction boundaries.
2. Implement a streaming parser and one-symbol source reconstruction with counters, invariants and deterministic state hashes. No matcher changes.
3. Download one selected sample only when running the spike. Record peak live orders, price span, message mix, missing references, unsupported records, and symbol/session coverage.
4. Run the proposed compatibility checks. Report number and length of usable contiguous segments, every exclusion category, translated command mix, bootstrap count, and reference/fast equality.
5. Choose the outcome from evidence: publish derived-workload benchmarks only if a useful, fully validated segment exists; otherwise publish structural replay separately or defer ITCH entirely. Do not lower correctness requirements to increase coverage.

**Go:** zero unexplained reconstruction errors, all accepted commands reproducible, exact reference/fast agreement, recorded coverage and resource bounds, and enough translated operations for the existing benchmark harness's required sample count. **Scope limit:** passing these checks validates the chosen adapter, not a complete exchange simulation. No arbitrary minimum coverage percentage is proposed before seeing data.

The [official ITCH FAQ](https://www.nasdaqtrader.com/Content/TechnicalSupport/FAQs/ITCH_FAQ.pdf) was located, but its full PDF could not be retrieved by the browsing tool in this session; detailed conclusions above use the accessible specification and official product/protocol pages instead. Dataset integrity, observed compatibility rates, and performance remain unmeasured.
