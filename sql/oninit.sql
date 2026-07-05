-- plphp.on_init failure: the first call of the session fails cleanly; after
-- clearing the GUC things work (this file must be its own test so it gets a
-- fresh session).
CREATE FUNCTION oninit_victim() RETURNS int LANGUAGE plphp AS $$
	return 41 + 1;
$$;
SET plphp.on_init = 'throw new Exception("on_init boom");';
SELECT oninit_victim();
RESET plphp.on_init;
SELECT oninit_victim();
DROP FUNCTION oninit_victim();
