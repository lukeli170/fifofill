# FifoFill: C++20 Mini Stock Exchange Engine

FifoFill is a mini stock exchange engine built in **C++20**. Orders come in, it keeps track of who wants to buy and sell at what price, matches them when prices cross, and reports the trades. There's no screen, no real money, and no internet connection. It's just the core engine, built to be provably correct and then fast.

## The core idea

**The book** has two sides:
- **Bids**: buyers, sorted from highest price down.
- **Asks**: sellers, sorted from lowest price up.

Each price has a queue of orders waiting there. That queue is the FIFO in the name: first in line gets filled first.

**Matching rule (price-time priority):** an incoming buy matches the cheapest seller first. If several sellers sit at that price, the one who arrived earliest goes first. Sells work the same way in mirror image.

**Trade price = the waiting order's price.** If you're willing to pay $101 but someone is already selling at $100, you pay $100. A lot of student versions get this wrong.

**Order types to support:**
- **Limit order:** "buy at most $X." It matches what it can, and the rest waits in the book.
- **Market order:** "buy now at any price." It matches what it can, and the rest is canceled. It never waits in the book.
- **Cancel:** remove a waiting order.
- **Modify:** shrinking an order keeps its place in line. Changing the price or increasing the size sends it to the back. Real exchanges work this way.

## Worked example

Book before:

| Side | Price | Queue (oldest first) |
|---|---|---|
| Ask | 101 | S3: 200 |
| Ask | 100 | S1: 100, S2: 50 |
| Bid | 99 | B1: 300 |

Incoming: **Buy 220 @ 101**
1. It fills against S1: 100 shares @ 100.
2. It fills against S2: 50 shares @ 100. The 100 level is now empty.
3. It fills against S3: 70 shares @ 101. S3 has 130 left and keeps its place.
4. The incoming order is fully filled, so nothing is added to the book.

If it had been **Buy 400 @ 100** instead, it would fill 150 @ 100, and the leftover 250 would rest as the new best bid at 100.

The book must never end up "crossed," meaning the best bid at or above the best ask. If that ever happens, you have a bug.

## Build it in this order

**1. Reference engine (slow, obviously correct)**
Write it with the simplest structures possible: a sorted map from price to a queue of orders, plus a lookup from order ID to order. Aim for about 200 lines that are easy to read, with no cleverness. This version is your answer key.

**2. Unit tests for edge cases**
- An order that eats through several price levels
- A partial fill on either side
- Cancelling an order that was already filled, doesn't exist, or uses a duplicate ID
- A market order when the other side is empty
- Zero quantity, or a price not on the allowed increments
- Modify keeping or losing its place in line

**3. Fast engine**
Same inputs, same outputs, different internals (details below).

**4. Fuzz testing (the impressive part)**
A random order generator with a seed feeds millions of orders to both engines and compares every output event. If they ever disagree, print the seed so you can replay the failure. Also check invariants after every event: the book isn't crossed, no zero-size orders are resting, and quantities add up.

**5. Benchmarks**
Measure, write it up, and done.

## Core types

```cpp
using Price = int64_t;   // in ticks: $101.25 -> 10125
using Qty   = uint32_t;
enum class Side : uint8_t { Buy, Sell };

struct Order {
    uint64_t id;
    Side side;
    Price price;
    Qty remaining;
    Order* prev;   // neighbors in this price's queue
    Order* next;
};
```

The engine takes `add`, `cancel` and `modify`, and emits events: accepted, trade (buyer ID, seller ID, price, quantity), canceled, or rejected. It must be **deterministic**, meaning the same inputs always produce the same outputs. Determinism is what makes the fuzz comparison possible.

**Prices are integers, never decimals.** Computers can't store 0.1 exactly, so two "equal" decimal prices can fail to match. Interviewers ask about this.

## What makes the fast version fast

Start with a straightforward version, then change one thing at a time and measure after each change. Each change becomes a row in your README's results table.

1. **Flat array of price levels** instead of a sorted tree. Most activity happens near the current price, so you jump straight to a level instead of searching for it.
2. **Orders linked through themselves** (`prev`/`next` above). Adding to the back of a queue and removing from the middle on a cancel are both instant.
3. **ID-to-order lookup table**, so a cancel never has to search.
4. **Preallocate every order up front.** Asking the system for memory in the middle of a match is slow and unpredictable.
5. **Track the best bid and ask directly.** When a level empties, find the next one quickly, for example with a bitmap of non-empty levels.
6. **Single-threaded.** Real matching engines often run one thread per stock, because coordinating threads costs more than it saves. This makes a great interview answer.

## Benchmarks that hold up

- **Synthetic flow:** prices clustered near the middle, mostly adds and cancels, and fewer orders that actually trade. That's how real markets look.
- **Real data:** Nasdaq has posted free sample days of its full order feed (called ITCH) at emi.nasdaq.com; the full historical archive is a paid subscription. Replaying a real trading day makes your benchmark numbers far more believable than synthetic data alone. One catch: the feed shows fills against resting orders but not the incoming orders that caused them, so you'll need a small translation step.
- **Report:** orders per second; typical, 99th-percentile and 99.9th-percentile latency for each operation; your machine and compiler settings; and the speedup of the fast engine over your reference.

## Don't build these (scope traps)

No GUI, no network protocol, no connecting to a real exchange, no trading strategies, no multithreading, and no crypto. Every one of these is a way to never finish the part that matters.

## Tooling and layout

Use **C++20** throughout the project. Build with CMake, test with GoogleTest, benchmark with Google Benchmark, and run everything automatically on GitHub Actions. Also run your tests with AddressSanitizer turned on, which catches memory bugs for free.

```text
fifofill/
  include/fifofill/   order.hpp, book.hpp, events.hpp
  src/                fast engine
  reference/          slow answer-key engine
  tests/              unit tests + fuzz
  bench/              benchmarks
  tools/              order generator, ITCH replayer
  README.md  LICENSE  CMakeLists.txt
```

**README:** a one-paragraph explanation, a small diagram of the book, the results table, how correctness is verified (fuzzing against the reference), key design decisions, and what you'd do next.

## Rough timeline (part-time, about 6 weeks)

| Week | Goal |
|---|---|
| 1 | Learn the concepts, build the reference engine and unit tests |
| 2–3 | Fast engine |
| 4 | Fuzz testing; fix whatever it finds (it will find things) |
| 5 | Benchmarks and optimization, one change at a time |
| 6 | README, cleanup, CI |
| Later | ITCH replay, more order types (fill-or-kill, post-only) |

## Be ready to answer in interviews

Why integer prices? Why single-threaded? Why do trades happen at the resting order's price? How do you know the engine is correct? What was the slowest part, and how did you find it?

## Sources

- [RITCH package docs (sample ITCH files at emi.nasdaq.com)](https://archive.linux.duke.edu/cran/web/packages/RITCH/refman/RITCH.html)
- [Nasdaq Historical TotalView-ITCH](https://nasdaqtrader.com/Trader.aspx?id=itch)
