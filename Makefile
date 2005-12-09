# $Id$

MODULE_big = plphp
OBJS = plphp.o plphp_io.o plphp_spi.o
REGRESS = basic trigger
PG_CPPFLAGS = $(shell php-config --includes)
APXS = $(shell which apxs apxs2)
SHLIB_LINK = -lphp4 -L$(shell $(APXS) -q libexecdir)

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plphp
REGRESS = base shared trigger spi raise cargs pseudo srf validator

install: install-php

install-php:
	$(LN_S) -f $(shell $(APXS) -q libexecdir)/libphp4.so $(libdir)

PGXS = $(shell pg_config --pgxs)
include $(PGXS)
