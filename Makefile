# PL/php - PHP as a procedural language for PostgreSQL
#
# PGXS-based build.  Requires:
#   * PostgreSQL server development files (pg_config on PATH, or set PG_CONFIG)
#   * PHP (>= 8.0) built with the embed SAPI, non-thread-safe (php-config on PATH)
#
# Build & install:
#   make
#   sudo make install
#   (in a database) CREATE EXTENSION plphp;
#
# Run the regression tests against a running server:
#   make installcheck

MODULE_big = plphp
OBJS = plphp.o plphp_io.o plphp_spi.o

EXTENSION = plphp
DATA = plphp--2.0.sql

# PHP embed SAPI compile/link flags, discovered via php-config.
PHP_CONFIG ?= php-config
PHP_INCLUDES := $(shell $(PHP_CONFIG) --includes)
PHP_LIBDIR := $(shell $(PHP_CONFIG) --prefix)/lib
PHP_LIBNAME := $(patsubst lib%.so,%,$(notdir $(firstword $(wildcard $(PHP_LIBDIR)/libphp*.so /usr/lib/libphp*.so /usr/lib/*/libphp*.so))))

PG_CPPFLAGS = $(PHP_INCLUDES)
# Link against the PHP embed library only; its own transitive dependencies
# (libz, libsodium, ...) are resolved at load time via libphp's NEEDED entries,
# so we do not add "php-config --libs" here (that would require their -dev
# symlinks at build time).
SHLIB_LINK = -L$(PHP_LIBDIR) -l$(PHP_LIBNAME) $(shell $(PHP_CONFIG) --ldflags)

# Regression tests.  "init" installs the extension; keep it first.
REGRESS = init base shared trigger spi raise cargs pseudo srf out varnames validator compat txn evttrig

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
