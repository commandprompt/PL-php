--
--testing trigger support
--prepearing
--
INSERT INTO test (i, v) VALUES (20,'spi_20');
INSERT INTO test (i, v) VALUES (21,'spi_21');
INSERT INTO test (i, v) VALUES (22,'spi_22');
INSERT INTO test (i, v) VALUES (23,'spi_23');
INSERT INTO test (i, v) VALUES (24,'spi_24');
INSERT INTO test (i, v) VALUES (25,'spi_25');
INSERT INTO test (i, v) VALUES (26,'spi_26');
INSERT INTO test (i, v) VALUES (27,'spi_27');
INSERT INTO test (i, v) VALUES (28,'spi_28');
INSERT INTO test (i, v) VALUES (29,'spi_29');
INSERT INTO test (i, v) VALUES (30,'spi_30');

CREATE OR REPLACE FUNCTION php_spi() RETURNS SETOF test AS '
    $query="select * from test where i>19";
    $res=spi_exec_query($query, 5);
    $i=0;
    while ($row=spi_fetch_row($res)){
	$rv[$i]=$row;
	$i++;
    }
return $rv;

' LANGUAGE 'plphp';
