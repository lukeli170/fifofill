# FifoFill v1 semantics

These are project rules for one symbol in a synchronous, single-threaded book,
not a claim about every exchange. Commands execute in input order. There are no
accounts, timestamps, self-trade rules, persistence, or network behavior.

## Values and configuration

- `OrderId` is `uint64_t`, nonzero and unique among resting orders. Reuse is
  allowed after filling or cancellation. Clients needing protection from delayed
  cancels must avoid reuse themselves. A duplicate market add is also rejected.
- `Price` is `int64_t` in integer monetary base units: with cents as the input
  unit, `10125` means $101.25. No floating point enters the engine.
- `Qty` is `uint32_t` and must be nonzero. Modify quantity means **new outstanding
  quantity**, regardless of original size or earlier fills.
- `Side` is `Buy` or `Sell`; other enum values are invalid input.
- An `Add` has ID, side, optional limit price, and quantity. An absent price
  explicitly means market. `Cancel` has ID. `Modify` has ID, new limit price,
  and new remaining quantity; it cannot change ID or side.
- `BookConfig` has positive `min_price`, `max_price >= min_price`, positive
  `tick_size`, and positive resting-order `capacity` C. Both endpoints must be
  divisible by the tick. Every limit or modification price must be positive,
  inside this inclusive range, and divisible by the tick.
- The number of levels is `(max_price - min_price) / tick_size + 1`; the index
  of a valid price is `(price - min_price) / tick_size`. Validate before any
  subtraction or unsigned conversion. Counts must fit `size_t`; C must allow
  `C + 2` events, their actual vector allocation bounds, and a worst-case sum
  of C maximum quantities in `uint64_t`. Storage introduced in later steps
  must check its own allocation sizes before allocating.

Invalid construction throws `std::invalid_argument` for invalid fields or
`std::length_error` for unrepresentable sizes, before a usable book exists.
Allocation failures propagate; abandon that run/book. They are fatal resource
failures, not business rejections with rollback guarantees. The reference may
allocate while processing; only the later final fast engine promises otherwise.

## Matching and capacity

An incoming buy consumes asks from lowest to highest; a sell consumes bids from
highest to lowest. At each price, consume oldest orders first. The trade price
is always the resting order's price. A partial maker retains its queue position.
Remove filled orders and empty levels immediately, including the ID lookup.

A limit consumes all eligible liquidity, then rests any remainder at its limit,
at the tail of that price's queue. A market consumes available liquidity, then
cancels any remainder; it never rests. The completed command cannot leave a
crossed book (best bid >= best ask).

At C resting orders, reject a new limit **before matching**, even one that could
fill immediately. Markets may execute at capacity. A modify reuses its existing
capacity and is allowed at capacity. Cancel and fills release capacity
immediately. This deliberately conservative policy avoids speculative matching
or rollback.

## Modify and cancellation

Validate a modification entirely before changing the old order. At the same
price, a smaller remaining quantity retains FIFO priority; equal quantity is an
accepted no-op. A quantity increase or any price change loses priority. Remove
the old queue position, match the replacement as an incoming limit, and append
any remainder at the new price. A crossing replacement can partially or fully
fill. Do not emit a cancel/add pair for this internal replacement.

Zero-size modify rejects and preserves the order. Explicit cancel removes all
remaining quantity. Filled, canceled, and never-seen IDs all reject as unknown;
there is no historical ID registry.

## Events and validation order

Events are owning scalar values with fieldwise equality and no strings/pointers:

| Event | Fields |
|---|---|
| `Accepted` | `operation` (Add/Modify), `id`, `side`, optional `price`, `quantity`, `priority_retained` (false for adds) |
| `Trade` | `buyer_id`, `seller_id`, resting `price`, `quantity` |
| `Canceled` | `id`, removed `quantity`, reason (`Explicit` or `MarketRemainder`) |
| `Rejected` | `operation` (Add/Cancel/Modify), `id`, stable reason |

Accepted quantities describe the request, not its eventual resting state.
Valid add emits Accepted, then trades in matching order, then Canceled only for
a market remainder. Valid modify emits Accepted(Modify), then any trades.
Valid cancel emits exactly one Canceled. Full fills have no extra event.

Every invalid command emits **exactly one Rejected** and preserves the entire
book, including FIFO order. First failing check wins:

| Operation | Precedence |
|---|---|
| Add | `InvalidId` -> `InvalidSide` -> `InvalidQuantity` -> `InvalidPrice` -> `DuplicateId` -> `CapacityExceeded` |
| Modify | `InvalidId` -> `UnknownOrder` -> `InvalidQuantity` -> `InvalidPrice` |
| Cancel | `InvalidId` -> `UnknownOrder` |

`InvalidPrice` covers nonpositive, outside-range, and off-tick values. Absence
of a market limit skips price validation. Malformed text is rejected by the
trace parser before replay begins and never becomes a command.

## API and diagnostics

`ReferenceBook::process(Command)` returns `std::span<const Event>` into an
engine-owned buffer reserved for C + 2 events at construction. A command trades
against at most C makers, plus one acceptance and one possible market cancel.
Consume/copy events before the next process call or destruction; that call
invalidates the previous view, including on rejection. Processing is neither
reentrant nor thread-safe. Books cannot be copied or moved because their ID
handles refer to their own queues.

`snapshot()` owns its values: bids in descending price/FIFO order, then asks in
ascending price/FIFO order, each order holding ID, side, price, remaining size.
It may allocate. `check_invariants()` diagnoses crossed books, empty levels,
invalid/duplicate orders, incorrect membership/handles, capacity, and checked
quantity totals; debug builds also call it after each command. Neither
snapshots nor invariant scans belong in performance measurements.

## Exact acceptance traces

In these traces prices are integer labels, tick is 1, the range is 1..1000,
and C is 10 unless stated otherwise. Queue notation is oldest first. Every
setup add is noncrossing and emits Accepted(Add, submitted fields, false).

### Brief examples (independent runs)

Setup: Add(1,Sell,100,100), Add(2,Sell,100,50), Add(3,Sell,101,200),
Add(4,Buy,99,300).

`Add(5,Buy,101,220)` emits:

1. Accepted(Add,5,Buy,101,220,false)
2. Trade(5,1,100,100)
3. Trade(5,2,100,50)
4. Trade(5,3,101,70)

Final bids: `[4:300@99]`; asks: `[3:130@101]`.

From the same initial setup, `Add(5,Buy,100,400)` emits:

1. Accepted(Add,5,Buy,100,400,false)
2. Trade(5,1,100,100)
3. Trade(5,2,100,50)

Final bids: `[5:250@100, 4:300@99]`; asks: `[3:200@101]`.

On an empty book, `Add(6,Buy,market,10)` emits
Accepted(Add,6,Buy,market,10,false), Canceled(6,10,MarketRemainder).
Both sides remain empty.

### Every modify classification (independent runs)

Setup: Add(1,Buy,100,10), Add(2,Buy,100,10), Add(3,Buy,99,10),
Add(4,Buy,101,10). No asks. Each row emits exactly
Accepted(Modify,1,Buy,new_price,new_quantity,retained) and no trades.

| Modify | Retained | Complete final bids (asks empty) |
|---|---|---|
| (1,100,5) | true | `[4:10@101, 1:5@100, 2:10@100, 3:10@99]` |
| (1,100,10) | true | `[4:10@101, 1:10@100, 2:10@100, 3:10@99]` |
| (1,100,15) | false | `[4:10@101, 2:10@100, 1:15@100, 3:10@99]` |
| (1,99,5) | false | `[4:10@101, 2:10@100, 3:10@99, 1:5@99]` |
| (1,99,10) | false | `[4:10@101, 2:10@100, 3:10@99, 1:10@99]` |
| (1,99,15) | false | `[4:10@101, 2:10@100, 3:10@99, 1:15@99]` |
| (1,101,5) | false | `[4:10@101, 1:5@101, 2:10@100, 3:10@99]` |
| (1,101,10) | false | `[4:10@101, 1:10@101, 2:10@100, 3:10@99]` |
| (1,101,15) | false | `[4:10@101, 1:15@101, 2:10@100, 3:10@99]` |

FIFO probes with only buys 1 then 2 at 100, each size 10:
shrink 1 to 5, then Add(5,Sell,100,6) emits acceptance, Trade(1,5,100,5),
Trade(2,5,100,1), leaving bid `[2:9@100]`. Increase 1 to 15 instead, then
Add(5,Sell,100,11) emits acceptance, Trade(2,5,100,10), Trade(1,5,100,1),
leaving bid `[1:14@100]`.

Partial-fill setup: buys 1 then 2 at 100, each 100; sell 3 at 100 for 40
emits Accepted then Trade(1,3,100,40). Modify(1,100,80) emits
Accepted(Modify,1,Buy,100,80,false), leaving `[2:100@100, 1:80@100]`.

Crossing setup: bid `[1:10@99]`, ask `[2:5@101]`. Modify(1,102,10) emits
Accepted(Modify,1,Buy,102,10,false), Trade(1,2,101,5); final bids
`[1:5@102]`, asks empty. With new quantity 5, the trade fully empties the book.

For the same setup, Modify(1,102,0) emits
Rejected(Modify,1,InvalidQuantity); Modify(1,0,10) emits
Rejected(Modify,1,InvalidPrice). Both preserve `[1:10@99]` / `[2:5@101]`.
Modify(99,0,0) emits Rejected(Modify,99,UnknownOrder); Modify(0,0,0)
emits Rejected(Modify,0,InvalidId).

With C=2 and those two resting orders, Add(3,Buy,102,5) emits
Rejected(Add,3,CapacityExceeded) without trading. Modify(1,102,10) still
accepts and trades as above. Cancel(1) on the original setup emits
Canceled(1,10,Explicit), leaving only ask `[2:5@101]`; another Cancel(1)
emits Rejected(Cancel,1,UnknownOrder). Add(1,Buy,99,4) can then reuse ID 1.
