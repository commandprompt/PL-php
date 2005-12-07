# $Id$

MODULE_big = plphp
OBJS = plphp.o plphp_io.o plphp_spi.o
REGRESS = basic trigger
PG_CPPFLAGS = $(shell php-config --includes)
SHLIB_LINK = -lphp4 -L/usr/lib/apache2/modules

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plphp
REGRESS = base shared trigger spi raise pseudo validator

install: install-php

install-php:
	$(LN_S) -f $(shell apxs2 -q libexecdir)/libphp4.so $(libdir)

PGXS = $(shell pg_config --pgxs)
include $(PGXS)
