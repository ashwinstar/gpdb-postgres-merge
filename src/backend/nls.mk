<<<<<<< HEAD
# $PostgreSQL: pgsql/src/backend/nls.mk,v 1.27 2009/06/26 19:33:43 petere Exp $
=======
# $PostgreSQL: pgsql/src/backend/nls.mk,v 1.22 2008/03/24 18:08:47 tgl Exp $
>>>>>>> f260edb144c1e3f33d5ecc3d00d5359ab675d238
CATALOG_NAME	:= postgres
AVAIL_LANGUAGES	:= de es fr ja pt_BR tr
GETTEXT_FILES	:= + gettext-files
<<<<<<< HEAD
GETTEXT_TRIGGERS:= _ errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext write_stderr yyerror
=======
# you can add "elog:2" and "errmsg_internal" to this list if you want to
# include internal messages in the translation list.
GETTEXT_TRIGGERS:= _ errmsg errdetail errdetail_log errhint errcontext write_stderr yyerror
>>>>>>> f260edb144c1e3f33d5ecc3d00d5359ab675d238

gettext-files: distprep
	find $(srcdir)/ $(srcdir)/../port/ -name '*.c' -print >$@

my-maintainer-clean:
	rm -f gettext-files

.PHONY: my-maintainer-clean
maintainer-clean: my-maintainer-clean
