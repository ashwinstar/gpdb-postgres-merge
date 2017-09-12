/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *	  Declarations for routines exported from lexer and parser files.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
<<<<<<< HEAD
 * $PostgreSQL: pgsql/src/include/parser/gramparse.h,v 1.44 2009/06/11 14:49:11 momjian Exp $
=======
 * $PostgreSQL: pgsql/src/include/parser/gramparse.h,v 1.41 2008/04/04 11:47:19 mha Exp $
>>>>>>> f260edb144c1e3f33d5ecc3d00d5359ab675d238
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H

#include "nodes/parsenodes.h"

/*
 * We track token locations in terms of byte offsets from the start of the
 * source string, not the column number/line number representation that
 * bison uses by default.  Also, to minimize overhead we track only one
 * location (usually the first token location) for each construct, not
 * the beginning and ending locations as bison does by default.  It's
 * therefore sufficient to make YYLTYPE an int.
 */
#define YYLTYPE  int

typedef enum
{
	BACKSLASH_QUOTE_OFF,
	BACKSLASH_QUOTE_ON,
	BACKSLASH_QUOTE_SAFE_ENCODING
} BackslashQuoteType;

/* GUC variables in scan.l (every one of these is a bad idea :-() */
<<<<<<< HEAD
extern int	backslash_quote;
=======
extern int backslash_quote;
>>>>>>> f260edb144c1e3f33d5ecc3d00d5359ab675d238
extern bool escape_string_warning;
extern bool standard_conforming_strings;


/* from parser.c */
extern int	filtered_base_yylex(void);

/* from scan.l */
extern void scanner_init(const char *str);
extern void scanner_finish(void);
extern int	base_yylex(void);
extern int	scanner_errposition(int location);
extern void base_yyerror(const char *message);

/* from gram.y */
extern void parser_init(void);
extern int	base_yyparse(void);
extern List *SystemFuncName(char *name);
extern TypeName *SystemTypeName(char *name);

#endif   /* GRAMPARSE_H */
