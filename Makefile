# $Id$
#
# (1) Fill in the blanks
# (2) Make sure pg_config is in your path ($PATH:/usr/local/pgsql/bin make) for example.
# (3) Make sure that php-config is installed and in your path
# (4) Enter path to apxs or apxs2 (depending on apache version installed)
# (5) Enter the PHP version you would like to use php4 or php5
# (6) Enter the path to the PHP apache Module
# (7) Make
# (8) See projects.commandprompt.com/public/plphp for docs, tests, features
#
#

#
# Set your php library version: 4 for php version 4, 5 for php5
#
#Ex: PHP_VERSION = 4
PHP_VERSION = 5

MODULE_big = plphp
OBJS = plphp.o plphp_io.o plphp_spi.o
REGRESS = basic trigger
PG_CPPFLAGS = $(shell php-config$(PHP_VERSION) --includes) -g


#
# Set path to your php static library
# PHP_LIB_PATH = /usr/lib/php

PHP_LIB_PATH = /usr/lib

#
# You should not have to modify anything below this line
#

SHLIB_LINK = -L$(PHP_LIB_PATH) -static -lphp$(PHP_VERSION) -static -lxml2

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plphp
REGRESS = base shared trigger spi raise cargs pseudo srf validator

all:


PGXS = $(shell pg_config --pgxs)
include $(PGXS)
