INSERT INTO pg_pltemplate (tmplname, tmpltrusted, tmpldbacreate, tmplhandler, tmplvalidator, tmpllibrary) VALUES
('plphp', 't', 't', 'plphp_call_handler', 'plphp_validator', '$libdir/plphp');

INSERT INTO pg_pltemplate(tmplname, tmpltrusted, tmpldbacreate, tmplhandler, tmplvalidator, tmpllibrary) VALUES
('plphpu', 'f', 'f', 'plphp_call_handler', 'plphp_validator', '$libdir/plphp');
