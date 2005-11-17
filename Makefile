# $Id$

MODULE_big = plphp
OBJS = plphp.o plphp_io.o
REGRESS = basic trigger
PG_CPPFLAGS = $(shell php-config --includes)
SHLIB_LINK = -lphp4 -L/usr/lib/apache2/modules

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plphp
#REGRESS = base shared arrayret cargs cset ptrig strig pseudo pspi sspi arr_in bset
REGRESS = base shared trigger

PGXS = $(shell pg_config --pgxs)
include $(PGXS)
