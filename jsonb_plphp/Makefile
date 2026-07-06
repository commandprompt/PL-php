# jsonb_plphp - transform between jsonb and PL/php
#
# Build after (or alongside) PL/php itself; requires the same PHP embed
# development files:
#   make PG_CONFIG=/path/to/pg_config
#   sudo make install
#   (in a database) CREATE EXTENSION jsonb_plphp CASCADE;

MODULE_big = jsonb_plphp
OBJS = jsonb_plphp.o

EXTENSION = jsonb_plphp
DATA = jsonb_plphp--1.0.sql

# PHP embed SAPI compile/link flags, discovered via php-config.
PHP_CONFIG ?= php-config
PHP_INCLUDES := $(shell $(PHP_CONFIG) --includes)
PHP_LIBDIR := $(shell $(PHP_CONFIG) --prefix)/lib
# Prefer the library matching PHP_CONFIG's version (e.g. libphp8.3.so); with
# several PHP versions installed, a bare "libphp*.so" glob would pick the
# unversioned symlink of whichever version owns it.
PHP_VERSION := $(shell $(PHP_CONFIG) --version | cut -d. -f1-2)
PHP_LIBNAME := $(patsubst lib%.so,%,$(notdir $(firstword $(wildcard \
	$(PHP_LIBDIR)/libphp$(PHP_VERSION).so /usr/lib/libphp$(PHP_VERSION).so /usr/lib/*/libphp$(PHP_VERSION).so \
	$(PHP_LIBDIR)/libphp*.so /usr/lib/libphp*.so /usr/lib/*/libphp*.so))))

PG_CPPFLAGS = $(PHP_INCLUDES)
# Extra flags hook, e.g. ASAN_FLAGS="-fsanitize=address" for sanitizer builds
ASAN_FLAGS ?=
PG_CFLAGS += $(ASAN_FLAGS)
SHLIB_LINK = $(ASAN_FLAGS) -L$(PHP_LIBDIR) -l$(PHP_LIBNAME) $(shell $(PHP_CONFIG) --ldflags)

REGRESS = jsonb_plphp

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
