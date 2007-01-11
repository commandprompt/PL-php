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

MODULE_big = plphp
OBJS = plphp.o plphp_io.o plphp_spi.o
REGRESS = basic trigger
PG_CPPFLAGS = -I/usr/include/php5 -I/usr/include/php5/main -I/usr/include/php5/TSRM -I/usr/include/php5/Zend -I/usr/include/php5/ext 


#
# You should not have to modify anything below this line
#

SHLIB_LINK =  -lphp5 -lxml2 

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plphp
REGRESS = base shared trigger spi raise cargs pseudo srf validator

all: all-lib
install: install-lib


PGXS = /home/alexk/pgsql/lib/pgxs/src/makefiles/pgxs.mk
include $(PGXS)
