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
MODULE_big = plphp
OBJS = plphp.o plphp_io.o plphp_spi.o
REGRESS = basic trigger
PG_CPPFLAGS = $(shell php-config --includes)

#
# Place the fully qualified path to you apxs or apxs2 application
#
# Ex:
# APXS = /usr/sbin/apxs2
#
APXS = apxs2

#
# Set your php library version: php4 for php version 4, php5 for php5
#
#Ex: PHP_VERSION = php4
PHP_VERSION = php5

#
# Set path to your php apache module
#
# Ex: PHP_LIB_PATH = /usr/lib/apache2/modules/libphp5.so
PHP_LIB_PATH = /usr/lib/apache2/modules/libphp5.so

#
# You should not have to modify anything below this line
#

SHLIB_LINK = -l$(PHP_VERSION) -L$(shell $(APXS) -q libexecdir)

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plphp
REGRESS = base shared trigger spi raise cargs pseudo srf validator

install: install-php

install-php:
	$(LN_S) -f $(shell $(APXS) -q $(PHP_LIB_PATH)) $(libdir)

PGXS = $(shell pg_config --pgxs)
include $(PGXS)
