# PL/php performance

How fast is PL/php compared to the built-in procedural languages? These are
the first published numbers for the modernized extension. Reproduce them any
time with the committed suite:

```sh
PGPORT=5432 sh bench/run.sh
```

## Results

PostgreSQL 18, PHP 8.3 (embed, NTS), single client, `pgbench -T 8`, one
warm session, Ubuntu 24.04 container on x86-64. Higher is better; treat
±3% as noise.

| Benchmark | PL/php | PL/pgSQL | PL/Perl |
|---|---:|---:|---:|
| Scalar math (`(a*3+b)%97`) | 58,800 | 60,000 | 57,900 |
| String ops (reverse+upper+length) | 43,400 | 44,000 | 46,300 |
| SPI loop over 1,000 rows | 4,700 | 8,600 | 2,700 |
| 10 small SPI statements per call | 23,500 | 26,000 | 18,700 |

## Reading the numbers

- **Scalar and string work is call-overhead-bound.** All three languages sit
  within a few percent of each other: the executor's function-call machinery
  dominates, not the interpreter. PL/php's text-based argument conversion is
  not a measurable factor at this scale.
- **Row iteration is PL/pgSQL's home turf.** Its `FOR ... IN SELECT` loop
  iterates natively without crossing a language boundary per row. PL/php
  pays a C-to-PHP conversion per row (one output-function call and one zval
  per column) — and is still **1.75× faster than PL/Perl**, which
  materializes the entire result into Perl structures up front.
- **Repeated SPI statements** carry a per-call subtransaction in both PL/php
  and PL/Perl (that is what makes their errors catchable); PL/pgSQL's
  `EXECUTE` does not. PL/php lands between the two, ~25% ahead of PL/Perl.

## What was tried and rejected

Interning the column-name hash keys once per result (instead of hashing them
per row) measured as a no-op: the row-loop cost lives in per-cell value
conversion, not key handling. The optimization was dropped rather than
carried as complexity without benefit. A future fast path worth exploring is
converting common scalar types (int/float/bool/text) from their binary Datum
form instead of through the type output functions — that requires threading
type metadata into `spi_fetch_row`'s result handling.

## Guidance

- For pure computation, use whichever language reads best — the overhead
  differences are negligible.
- For tight loops over large results, prefer a set-based SQL statement (or
  PL/pgSQL) when the logic allows; when you need PHP's expressiveness per
  row, `spi_query`/`spi_fetchrow` keeps memory flat and PL/php's per-row
  cost is the best of the interpreted PLs measured here.
