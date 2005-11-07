--
--testing trigger support
--prepearing
--

CREATE OR REPLACE FUNCTION valid_id() RETURNS trigger AS '
        if (($_TD["new"]["i"]>=100) || ($_TD["new"]["i"]<=0)){
                return "SKIP";
        } else return;
' LANGUAGE 'plphp';

CREATE OR REPLACE FUNCTION immortal() RETURNS trigger AS '
        if ($_TD["old"]["v"]==$_TD["args"][0]){
                return "SKIP";
        } else
                return;
' LANGUAGE 'plphp';

CREATE TRIGGER "test_valid_id_trig" BEFORE INSERT OR UPDATE ON test
FOR EACH ROW EXECUTE PROCEDURE "valid_id"();

CREATE TRIGGER "immortal_trig" BEFORE DELETE ON test
FOR EACH ROW EXECUTE PROCEDURE immortal('immortal');

INSERT INTO test (i, v) VALUES (1,'first line');
INSERT INTO test (i, v) VALUES (2,'second line');
INSERT INTO test (i, v) VALUES (3,'immortal');
INSERT INTO test (i, v) VALUES (200,'fake');

DELETE FROM test WHERE i < 100;