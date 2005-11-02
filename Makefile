# $Id$

MODULE_big = plphp
OBJS = plphp.o
REGRESS = basic trigger
PG_CPPFLAGS = $(shell php-config --includes)
SHLIB_LINK = -lphp4 -L/usr/lib/apache2/modules

PGXS = $(shell pg_config --pgxs)
include $(PGXS)
