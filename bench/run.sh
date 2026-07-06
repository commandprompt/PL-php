#!/bin/sh
# Benchmark PL/php against PL/pgSQL and PL/Perl.
#
# Usage: PGPORT=5432 [PGBENCH=/usr/lib/postgresql/18/bin/pgbench] sh bench/run.sh
# Requires: a running cluster you can create a "plphp_bench" database in,
# with plphp installed and (optionally) plperl available.
set -e
PGBENCH=${PGBENCH:-pgbench}
DB=plphp_bench
SECS=${SECS:-8}

dropdb --if-exists $DB 2>/dev/null || true
createdb $DB
psql -qX -d $DB -f "$(dirname $0)/setup.sql"

for fn in math str rows spi; do
  case $fn in
    math) body='\set a random(1,1000)
SELECT FN(:a, 7);' ;;
    str)  body='SELECT FN(chr(97+(random()*20)::int) || repeat(chr(98), 30));' ;;
    rows) body='SELECT FN();' ;;
    spi)  body='\set a random(1,1000)
SELECT FN(:a);' ;;
  esac
  for lang in php pgsql perl; do
    printf '%s\n' "$body" | sed "s/FN/${fn}_${lang}/" > /tmp/plphp_bench_$$.sql
    tps=$($PGBENCH -n -c 1 -T $SECS -f /tmp/plphp_bench_$$.sql $DB 2>/dev/null \
          | awk '/^tps/ {printf "%.0f", $3}')
    printf '%-12s %10s tps\n' "${fn}_${lang}" "$tps"
  done
done
rm -f /tmp/plphp_bench_$$.sql
