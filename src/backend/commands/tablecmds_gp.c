/*-------------------------------------------------------------------------
 *
 * tablecmds_gp.c
 *	  Greenplum extensions for ALTER TABLE.
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablecmds_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/partition.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_opclass.h"
#include "cdb/cdbvars.h"
#include "cdb/memquota.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "executor/execPartition.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_utilcmd.h"
#include "partitioning/partdesc.h"
#include <postmaster/autostats.h>
#include "rewrite/rewriteHandler.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/partcache.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Build a datum according to tables partition key based on parse expr. Would
 * have been nice if FormPartitionKeyDatum() was generic and could have been
 * used instead.
 */
static void
FormPartitionKeyDatumFromExpr(Relation rel, Node *expr, Datum *values, bool *isnull)
{
	PartitionKey partkey;
	int 		num_expr;
	ListCell   *lc;
	int			i;

	Assert(rel);
	partkey = RelationGetPartitionKey(rel);
	Assert(partkey != NULL);

	Assert(IsA(expr, List));
	num_expr = list_length((List *) expr);

	if (num_expr > RelationGetDescr(rel)->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("too many columns in boundary specification (%d > %d)",
						num_expr, RelationGetDescr(rel)->natts)));

	if (num_expr > partkey->partnatts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("too many columns in boundary specification (%d > %d)",
						num_expr, partkey->partnatts)));

	i = 0;
	foreach(lc, (List *) expr)
	{
		Const      *result;
		char       *colname;
		Oid			coltype;
		int32		coltypmod;
		Oid			partcollation;
		Node	   *n1 = (Node *) lfirst(lc);

		/* Get column's name in case we need to output an error */
		if (partkey->partattrs[i] != 0)
			colname = get_attname(RelationGetRelid(rel),
								  partkey->partattrs[i], false);
		else
			colname = deparse_expression((Node *) linitial(partkey->partexprs),
										 deparse_context_for(RelationGetRelationName(rel),
															 RelationGetRelid(rel)),
										 false, false);

		coltype = get_partition_col_typid(partkey, i);
		coltypmod = get_partition_col_typmod(partkey, i);
		partcollation = get_partition_col_collation(partkey, i);
		result = transformPartitionBoundValue(make_parsestate(NULL), n1,
											  colname, coltype, coltypmod,
											  partcollation);

		values[i] = result->constvalue;
		isnull[i] = result->constisnull;
		i++;
	}

	for (; i < partkey->partnatts; i++)
	{
		values[i] = 0;
		isnull[i] = true;
	}
}

static Oid
GpFindTargetPartition(Relation parent, GpAlterPartitionId *partid,
					  bool missing_ok)
{
	Oid			target_relid = InvalidOid;

	switch (partid->idtype)
	{
		case AT_AP_IDDefault:
			/* Find default partition */
			target_relid =
				get_default_oid_from_partdesc(RelationGetPartitionDesc(parent));
			if (!OidIsValid(target_relid) && !missing_ok)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("DEFAULT partition of relation \"%s\" does not exist",
								RelationGetRelationName(parent))));
			break;
		case AT_AP_IDName:
		{
			/* Find partition by name */
			RangeVar	*partrv;
			char		*schemaname;
			char		*partname;
			Relation	partRel;
			char partsubstring[NAMEDATALEN];
			List	   *ancestors = get_partition_ancestors(RelationGetRelid(parent));
			int			level = list_length(ancestors) + 1;
			char levelStr[NAMEDATALEN];

			snprintf(levelStr, NAMEDATALEN, "%d", level);
			snprintf(partsubstring, NAMEDATALEN, "prt_%s",
					 strVal(partid->partiddef));

			partname = makeObjectName(RelationGetRelationName(parent),
									  levelStr, partsubstring);

			/*
			 * GPDB_12_MERGE_FIXME: Child can be in different namespace from
			 * parent. So, using parent's namespace to find the relation seems
			 * incorrect. Need to find better way to find the relation. One
			 * option is to use the OIDs of child partitions from parent
			 * relation objects partDesc and then lookup which table matches
			 * the given name. Though that might be expensive lookup.
			 */
			schemaname   = get_namespace_name(parent->rd_rel->relnamespace);
			partrv       = makeRangeVar(schemaname, partname, -1);
			partRel      = table_openrv_extended(partrv, AccessShareLock, missing_ok);
			if (partRel)
			{
				target_relid = RelationGetRelid(partRel);
				table_close(partRel, AccessShareLock);
			}
			break;
		}

		case AT_AP_IDValue:
			{
				Datum		values[PARTITION_MAX_KEYS];
				bool		isnull[PARTITION_MAX_KEYS];
				PartitionDesc partdesc = RelationGetPartitionDesc(parent);
				int partidx;

				FormPartitionKeyDatumFromExpr(parent, partid->partiddef, values, isnull);
				partidx = get_partition_for_tuple(RelationGetPartitionKey(parent),
												  partdesc, values, isnull);

				if (partidx < 0)
				{
					if (missing_ok)
						break;
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("partition for specified value of %s does not exist",
									RelationGetRelationName(parent))));
				}

				if (partdesc->oids[partidx] ==
					get_default_oid_from_partdesc(RelationGetPartitionDesc(parent)))
				{
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("FOR expression matches DEFAULT partition for specified value of relation \"%s\"",
									RelationGetRelationName(parent)),
							 errhint("FOR expression may only specify a non-default partition in this context.")));
				}

				target_relid = partdesc->oids[partidx];
				break;
			}

		case AT_AP_IDNone:
			elog(ERROR, "not expected value");
			break;
	}

	return target_relid;
}

/*
 * Generate partition key specification of a partitioned table
 */
static PartitionSpec *
generatePartitionSpec(Relation rel)
{
	HeapTuple   tuple;
	Form_pg_partitioned_table form;
	AttrNumber *attrs;
	oidvector  *opclass;
	oidvector  *collation;
	Datum		datum;
	bool		isnull;
	Oid 		relid = RelationGetRelid(rel);
	PartitionSpec* subpart = makeNode(PartitionSpec);

	/*
	 * We cannot get the opclass oids of the partition keys directly from
	 * rel->rd_partkey because it only stores opcfamily oids and opcintype oids,
	 * and there are no syscache entries exists to lookup pg_opclass with those
	 * two values. Therefore we need to lookup pg_partitioned_table to fetch
	 * the opclass names. Since we have just opened the relation and built the
	 * partition descriptor, changes are high we hit the cache.
	 *
	 * We choose to as well use Form_pg_partitioned_table to fetch the partition
	 * attributes, their collations and expressions, even though we do have
	 * access to the same from rel->rd_partkey.
	 */
	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "missing partition key information for oid %d", relid);
	/* Fixed-length attributes */
	form = (Form_pg_partitioned_table) GETSTRUCT(tuple);

	switch (form->partstrat)
	{
		case PARTITION_STRATEGY_RANGE:
			subpart->strategy = psprintf("range");
			break;
		case PARTITION_STRATEGY_LIST:
			subpart->strategy = psprintf("list");
			break;
		case PARTITION_STRATEGY_HASH:
			subpart->strategy = psprintf("hash");
			break;
	}

	subpart->location = -1;

	/*
	 * We can rely on the first variable-length attribute being mapped to the
	 * relevant field of the catalog's C struct, because all previous
	 * attributes are non-nullable and fixed-length.
	 */
	attrs = form->partattrs.values;

	/* But use the hard way to retrieve further variable-length attributes */
	/* Operator class */
	datum = SysCacheGetAttr(PARTRELID, tuple,
							Anum_pg_partitioned_table_partclass, &isnull);
	Assert(!isnull);
	opclass = (oidvector *) DatumGetPointer(datum);

	/* Collation */
	datum = SysCacheGetAttr(PARTRELID, tuple,
							Anum_pg_partitioned_table_partcollation, &isnull);
	Assert(!isnull);
	collation = (oidvector *) DatumGetPointer(datum);

	/* Expression */
	SysCacheGetAttr(PARTRELID, tuple,
							Anum_pg_partitioned_table_partexprs, &isnull);
	if (!isnull)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SUBPARTITION BY contain expressions. Cannot ADD PARTITION if expressions in partition key using legacy syntax"),
				 errhint("Table was created using new partition syntax. Hence, use CREATE TABLE... PARTITION OF instead.")));
	}

	subpart->partParams = NIL;
	for (int i = 0; i < form->partnatts; i++)
	{
		PartitionElem *elem = makeNode(PartitionElem);
		AttrNumber  attno = attrs[i];
		Form_pg_opclass opclassform;
		Form_pg_collation collationform;
		char *opclassname;
		char *collationname = NULL;
		HeapTuple tmptuple;

		Assert(attno > 0);
		Assert(attno <= RelationGetDescr(rel)->natts);
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(rel), attno - 1);
		Assert(attr != NULL);
		Assert(NameStr(attr->attname) != NULL);
		elem->name = pstrdup(NameStr(attr->attname));

		/* Collect opfamily information */
		tmptuple = SearchSysCache1(CLAOID,
								   ObjectIdGetDatum(opclass->values[i]));
		if (!HeapTupleIsValid(tmptuple))
			elog(ERROR, "cache lookup failed for opclass %u", opclass->values[i]);
		opclassform = (Form_pg_opclass) GETSTRUCT(tmptuple);
		opclassname = psprintf("%s", NameStr(opclassform->opcname));
		elem->opclass = list_make1(makeString(opclassname));
		ReleaseSysCache(tmptuple);

		/* Collect collation information */
		if (collation->values[i])
		{
			tmptuple = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation->values[i]));
			if (!HeapTupleIsValid(tmptuple))
				elog(ERROR, "collation with OID %u does not exist", collation->values[i]);
			collationform = (Form_pg_collation) GETSTRUCT(tmptuple);
			collationname = psprintf("%s", NameStr(collationform->collname));
			elem->collation = list_make1(makeString(collationname));
			ReleaseSysCache(tmptuple);
		}

		subpart->partParams = lappend(subpart->partParams, elem);
	}

	ReleaseSysCache(tuple);

	return subpart;
}

static RenameStmt *
generateRenameStmt(RangeVar *rv, const char *newname)
{
	RenameStmt *rstmt = makeNode(RenameStmt);

	rstmt->renameType = OBJECT_TABLE;
	rstmt->relation = rv;
	rstmt->subname = NULL;
	rstmt->newname = pstrdup(newname);
	rstmt->missing_ok = false;

	return rstmt;
}

static AlterObjectSchemaStmt *
generateAlterSchema(RangeVar *rv, const char *newname)
{
	AlterObjectSchemaStmt *shstmt = makeNode(AlterObjectSchemaStmt);
	shstmt->objectType = OBJECT_TABLE;
	shstmt->relation = rv;
	shstmt->newschema = pstrdup(newname);
	shstmt->missing_ok = false;

	return shstmt;
}


/*
 * ------------------------------
 * Implementation for command:
 *
 * ALTER TABLE <tab> EXCHANGE PARTITION <oldpart> with table <newpart>
 *
 * Above alter is converted into following sequence of cmds on QD:
 *
 * 1. ALTER TABLE <tab> DETACH PARTITION <oldpart>;
 * 2. ALTER TABLE <tab> ATTACH PARTITION <oldpart>;
 * 3. ALTER TABLE <oldpart> RENAME TO tmp;
 * 4. if namespace differ between oldpart and newpart
 *      a. ALTER TABLE <tmp> SET SCHEMA <newpart_schema>
 *      b. ALTER TABLE <newpart> RENAME TO <tmp2>
 *      c. ALTER TABLE <tmp2> SET SCHEMA <oldpart_schema>
 * 5. ALTER TABLE <newpart> RENAME TO <oldpart>;
 * 6. ALTER TABLE tmp RENAME TO <newpart>
 * ------------------------------
 */
static List *
AtExecGPExchangePartition(Relation rel, AlterTableCmd *cmd)
{
	GpAlterPartitionCmd *pc = castNode(GpAlterPartitionCmd, cmd->def);
	RangeVar *newpartrv = (RangeVar *) pc->arg;
	PartitionBoundSpec *boundspec;
	RangeVar *oldpartrv;
	RangeVar *tmprv;
	List	 *stmts = NIL;
	char	 *newpartname = pstrdup(newpartrv->relname);
	char	 tmpname2[NAMEDATALEN];

	/* detach oldpart partition cmd construction */
	{
		AlterTableStmt *atstmt = makeNode(AlterTableStmt);
		GpAlterPartitionId *pid = (GpAlterPartitionId *) pc->partid;
		AlterTableCmd *atcmd = makeNode(AlterTableCmd);
		PartitionCmd *pcmd = makeNode(PartitionCmd);
		char tmpname[NAMEDATALEN];
		Oid partrelid;
		Relation partrel;
		HeapTuple tuple;

		if (pid->idtype == AT_AP_IDDefault &&
			!gp_enable_exchange_default_partition)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot exchange DEFAULT partition")));
		}

		partrelid = GpFindTargetPartition(rel, pid, false);
		Assert(OidIsValid(partrelid));
		partrel = table_open(partrelid, AccessShareLock);

		if (partrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot EXCHANGE PARTITION for relation \"%s\" -- partition has children",
							RelationGetRelationName(rel))));

		oldpartrv = makeRangeVar(get_namespace_name(RelationGetNamespace(partrel)),
								 pstrdup(RelationGetRelationName(partrel)),
								 pc->location);

		snprintf(tmpname, sizeof(tmpname), "pg_temp_%u", partrelid);
		tmprv = makeRangeVar(get_namespace_name(RelationGetNamespace(partrel)),
							 pstrdup(tmpname),
							 pc->location);

		/*
		 * Get the oldpart's boundspec which will be used for attaching
		 * newpart. Fetch the tuple from the catcache, for speed. Relcache
		 * doesn't store the part bound hence need to fetch from catalog.
		 */
		tuple = SearchSysCache1(RELOID, partrelid);
		if (HeapTupleIsValid(tuple))
		{
			Datum		datum;
			bool		isnull;

			datum = SysCacheGetAttr(RELOID, tuple,
									Anum_pg_class_relpartbound,
									&isnull);
			if (!isnull)
				boundspec = stringToNode(TextDatumGetCString(datum));
			else
				boundspec = NULL;
			ReleaseSysCache(tuple);
		}
		else
			elog(ERROR, "pg_class tuple not found for relation %s",
				 RelationGetRelationName(partrel));

		table_close(partrel, AccessShareLock);

		pcmd->name = oldpartrv;
		pcmd->bound = NULL;
		atcmd->subtype = AT_DetachPartition;
		atcmd->def = (Node *) pcmd;

		atstmt->relation = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
										pstrdup(RelationGetRelationName(rel)),
										pc->location);
		atstmt->relkind = OBJECT_TABLE;
		atstmt->missing_ok = false;
		atstmt->cmds = list_make1(atcmd);
		atstmt->is_internal = true; /* set this to avoid transform */

		stmts = lappend(stmts, (Node *) atstmt);
	}

	/* attach newpart partition cmd construction */
	{
		AlterTableStmt *atstmt = makeNode(AlterTableStmt);
		AlterTableCmd  *atcmd  = makeNode(AlterTableCmd);
		PartitionCmd   *pcmd   = makeNode(PartitionCmd);

		Relation newpartrel = table_openrv(newpartrv, AccessShareLock);
		newpartrv->schemaname = get_namespace_name(RelationGetNamespace(newpartrel));
		snprintf(tmpname2, sizeof(tmpname2), "pg_temp_%u", RelationGetRelid(newpartrel));
		table_close(newpartrel, AccessShareLock);

		pcmd->name = newpartrv;
		pcmd->bound = boundspec;
		atcmd->subtype = AT_AttachPartition;
		atcmd->def = (Node *) pcmd;

		atstmt->relation = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
										pstrdup(RelationGetRelationName(rel)),
										pc->location);
		atstmt->relkind = OBJECT_TABLE;
		atstmt->missing_ok = false;
		atstmt->cmds = list_make1(atcmd);
		atstmt->is_internal = true; /* set this to avoid transform */

		stmts = lappend(stmts, (Node *) atstmt);
	}

	/* rename oldpart to tmp */
	stmts = lappend(stmts, (Node *) generateRenameStmt(oldpartrv, tmprv->relname));

	/*
	 * if schema is different for tables, need to swap the schema
	 */
	if (strcmp(oldpartrv->schemaname, newpartrv->schemaname) != 0)
	{
		/* change schema for tmp to newpart schema */
		stmts = lappend(stmts, (Node *) generateAlterSchema(tmprv, newpartrv->schemaname));

		/* reflect new reality in temprv for later use */
		tmprv = makeRangeVar(pstrdup(newpartrv->schemaname),
							 pstrdup(tmprv->relname),
							 pc->location);

		/* rename newpart to tmp2 */
		stmts = lappend(stmts, (Node *) generateRenameStmt(newpartrv, tmpname2));

		/* change schema for temp2 to oldpart schema */
		stmts = lappend(stmts, (Node *) generateAlterSchema(
							makeRangeVar(pstrdup(newpartrv->schemaname),
										 pstrdup(tmpname2),
										 pc->location),
							oldpartrv->schemaname));

		/* reflect new reality in newpartrv for later use */
		newpartrv = makeRangeVar(pstrdup(oldpartrv->schemaname),
								 pstrdup(tmpname2),
								 pc->location);
	}

	/* rename newpart to oldpart */
	stmts = lappend(stmts, (Node *) generateRenameStmt(newpartrv, oldpartrv->relname));
	/* rename tmp to newpart */
	stmts = lappend(stmts, (Node *) generateRenameStmt(tmprv, newpartname));

	return stmts;
}

void
ATExecGPPartCmds(Relation origrel, AlterTableCmd *cmd)
{
	Relation rel = origrel;
	PlannedStmt *pstmt;
	List *stmts = NIL;
	ListCell *l;

	Assert(Gp_role != GP_ROLE_EXECUTE);
	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	while (cmd->subtype == AT_PartAlter)
	{
		GpAlterPartitionCmd *pc;
		GpAlterPartitionId  *pid;
		Oid                 partrelid;

		pc  = castNode(GpAlterPartitionCmd, cmd->def);
		pid = (GpAlterPartitionId *) pc->partid;
		cmd = (AlterTableCmd *) pc->arg;

		if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("table \"%s\" is not partitioned",
							RelationGetRelationName(rel))));
		}

		partrelid = GpFindTargetPartition(rel, pid, false);
		Assert(OidIsValid(partrelid));

		if (rel != origrel)
			table_close(rel, AccessShareLock);
		rel = table_open(partrelid, AccessShareLock);
	}

	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("table \"%s\" is not partitioned",
						RelationGetRelationName(rel))));
	}

	switch (cmd->subtype)
	{
		case AT_PartTruncate:
		{
			GpAlterPartitionCmd *pc = castNode(GpAlterPartitionCmd, cmd->def);
			GpAlterPartitionId *pid = (GpAlterPartitionId *) pc->partid;
			TruncateStmt *truncstmt = (TruncateStmt *) pc->arg;
			Oid partrelid;
			RangeVar *rv;
			Relation partrel;

			partrelid = GpFindTargetPartition(rel, pid, false);
			Assert(OidIsValid(partrelid));
			partrel = table_open(partrelid, AccessShareLock);
			rv = makeRangeVar(get_namespace_name(RelationGetNamespace(partrel)),
							  pstrdup(RelationGetRelationName(partrel)),
							  pc->location);
			truncstmt->relations = list_make1(rv);
			table_close(partrel, AccessShareLock);
			stmts = lappend(stmts, (Node *) truncstmt);
		}
		break;

		case AT_PartAdd:			/* Add */
		{
			GpAlterPartitionCmd		*add_cmd = castNode(GpAlterPartitionCmd, cmd->def);
			GpPartitionDefinition	*gpPartDef = makeNode(GpPartitionDefinition);
			GpPartDefElem			*elem = castNode(GpPartDefElem, add_cmd->arg);
			PartitionSpec			*subpart = NULL;
			Relation 				temprel = rel;
			PartitionSpec 			*tempsubpart = NULL;
			ListCell 				*l;

			gpPartDef->partDefElems = list_make1(elem);

			/*
			 * Populate PARTITION BY spec for each level of the parents
			 * in the partitioning hierarchy. The PartitionSpec or a chain of
			 * PartitionSpecs if subpartitioning exists, are generated based on
			 * the first existing partition of each partition depth.
			 */
			do
			{
				PartitionDesc	partdesc;
				PartitionSpec *temptempsubpart = NULL;
				Oid firstchildoid;

				Assert(temprel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
				partdesc = RelationGetPartitionDesc(temprel);

				if (partdesc->nparts == 0)
					elog(ERROR, "GPDB add partition syntax needs at least one sibling to exist");

				if (partdesc->is_leaf[0])
				{
					if (temprel != rel)
						table_close(temprel, AccessShareLock);
					break;
				}

				firstchildoid = partdesc->oids[0];
				if (temprel != rel)
					table_close(temprel, AccessShareLock);

				temprel = table_open(firstchildoid, AccessShareLock);

				temptempsubpart = generatePartitionSpec(temprel);
				if (tempsubpart == NULL)
					subpart = tempsubpart = temptempsubpart;
				else
				{
					tempsubpart->subPartSpec = temptempsubpart;
					tempsubpart = tempsubpart->subPartSpec;
				}
			} while (1);

			List *cstmts = generatePartitions(RelationGetRelid(rel),
											  gpPartDef, subpart, NULL,
											  NIL, NULL);
			foreach(l, cstmts)
			{
				Node *stmt = (Node *) lfirst(l);
				stmts = lappend(stmts, (Node *) stmt);
			}
		}
		break;

		case AT_PartDrop:			/* Drop */
		{
			GpDropPartitionCmd *pc = castNode(GpDropPartitionCmd, cmd->def);
			GpAlterPartitionId *pid = (GpAlterPartitionId *) pc->partid;
			DropStmt *dropstmt = makeNode(DropStmt);
			Oid partrelid;
			Relation partrel;

			partrelid = GpFindTargetPartition(rel, pid, pc->missing_ok);
			if (!OidIsValid(partrelid))
				break;
			partrel = table_open(partrelid, AccessShareLock);
			dropstmt->objects = list_make1(
				list_make2(makeString(
							   get_namespace_name(RelationGetNamespace(partrel))),
						   makeString(pstrdup(RelationGetRelationName(partrel)))));
			dropstmt->removeType = OBJECT_TABLE;
			dropstmt->behavior = pc->behavior;
			dropstmt->missing_ok = pc->missing_ok;
			table_close(partrel, AccessShareLock);
			stmts = lappend(stmts, (Node *)dropstmt);
		}
		break;

		case AT_PartExchange:
		{
			List *returnstmt = AtExecGPExchangePartition(rel, cmd);
			stmts = list_concat(stmts, returnstmt);
		}
		break;

		case AT_PartSplit:
		{
			GpSplitPartitionCmd *pc = castNode(GpSplitPartitionCmd, cmd->def);
			RangeVar *oldpartrv;
			RangeVar *tmprv;
			PartitionBoundSpec *boundspec;

			/* TODO later optimize to avoid detach for default partition */

			/* detach current partition */
			{
				GpAlterPartitionId *pid = (GpAlterPartitionId *) pc->partid;
				AlterTableStmt *atstmt = makeNode(AlterTableStmt);
				AlterTableCmd *atcmd = makeNode(AlterTableCmd);
				PartitionCmd *pcmd = makeNode(PartitionCmd);
				char tmpname[NAMEDATALEN];
				Oid partrelid;
				Relation partrel;
				HeapTuple tuple;

				partrelid = GpFindTargetPartition(rel, pid, false);
				Assert(OidIsValid(partrelid));
				partrel = table_open(partrelid, AccessShareLock);

				if (partrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot SPLIT PARTITION for relation \"%s\" -- partition has children",
									RelationGetRelationName(rel))));

				oldpartrv = makeRangeVar(get_namespace_name(RelationGetNamespace(partrel)),
										 pstrdup(RelationGetRelationName(partrel)),
										 pc->location);

				snprintf(tmpname, sizeof(tmpname), "pg_temp_%u", partrelid);
				tmprv = makeRangeVar(get_namespace_name(RelationGetNamespace(partrel)),
									 pstrdup(tmpname),
									 pc->location);

				/*
				 * Get the part's boundspec. Fetch the tuple from the
				 * catcache, for speed. Relcache doesn't store the part bound
				 * hence need to fetch from catalog.
				 */
				tuple = SearchSysCache1(RELOID, partrelid);
				if (HeapTupleIsValid(tuple))
				{
					Datum		datum;
					bool		isnull;

					datum = SysCacheGetAttr(RELOID, tuple,
											Anum_pg_class_relpartbound,
											&isnull);
					if (!isnull)
						boundspec = stringToNode(TextDatumGetCString(datum));
					else
						boundspec = NULL;
					ReleaseSysCache(tuple);
				}
				else
					elog(ERROR, "pg_class tuple not found for relation %s",
						 RelationGetRelationName(partrel));

				table_close(partrel, AccessShareLock);

				pcmd->name = oldpartrv;
				pcmd->bound = NULL;
				atcmd->subtype = AT_DetachPartition;
				atcmd->def = (Node *) pcmd;

				atstmt->relation = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
												pstrdup(RelationGetRelationName(rel)),
												pc->location);
				atstmt->relkind = OBJECT_TABLE;
				atstmt->missing_ok = false;
				atstmt->cmds = list_make1(atcmd);
				atstmt->is_internal = true; /* set this to avoid transform */

				stmts = lappend(stmts, (Node *) atstmt);
			}

			/* rename old part to temp */
			stmts = lappend(stmts, (Node *) generateRenameStmt(oldpartrv, tmprv->relname));

			/* create new partitions */
			{
				List *start = linitial(pc->arg1);
				List *end   = lsecond(pc->arg1);
				partname_comp partcomp = {.tablename=NULL, .level=0, .partnum=0};
				List       *ancestors = get_partition_ancestors(RelationGetRelid(rel));
				GpPartDefElem *elem;
				PartitionBoundSpec *boundspec1;
				PartitionBoundSpec *boundspec2 = boundspec;
				PartitionKey partkey = RelationGetPartitionKey(rel);
				ParseState *pstate = make_parsestate(NULL);
				char *part_col_name;
				Oid			part_col_typid;
				int32		part_col_typmod;
				Oid			part_col_collation;

				Assert(partkey->partnatts == 1);

				part_col_name = NameStr(TupleDescAttr(RelationGetDescr(rel), partkey->partattrs[0] - 1)->attname);
				part_col_typid = get_partition_col_typid(partkey, 0);
				part_col_typmod = get_partition_col_typmod(partkey, 0);
				part_col_collation = get_partition_col_collation(partkey, 0);

				boundspec1 = makeNode(PartitionBoundSpec);
				boundspec1->strategy = PARTITION_STRATEGY_RANGE;
				boundspec1->is_default = false;

				Assert(end != NULL);
				if (end)
				{
					Const	   *endConst;
					if (list_length(end) != partkey->partnatts)
						elog(ERROR, "invalid number of end values"); // GPDB_12_MERGE_FIXME: improve message

					endConst = transformPartitionBoundValue(pstate,
															linitial(end),
															part_col_name,
															part_col_typid,
															part_col_typmod,
															part_col_collation);
					if (endConst->constisnull)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("cannot use NULL with range partition specification"),
								 parser_errposition(pstate, pc->location)));

					boundspec1->upperdatums = list_make1(makeConst(partkey->parttypid[0],
																   partkey->parttypmod[0],
																   partkey->parttypcoll[0],
																   partkey->parttyplen[0],
																   datumCopy(endConst->constvalue,
																			 partkey->parttypbyval[0],
																			 partkey->parttyplen[0]),
																   false,
																   partkey->parttypbyval[0]));
				}

				if (start)
				{
					Const	   *startConst;

					if (list_length(start) != partkey->partnatts)
						elog(ERROR, "invalid number of start values"); // GPDB_12_MERGE_FIXME: improve message

					startConst = transformPartitionBoundValue(pstate,
															  linitial(start),
															  part_col_name,
															  part_col_typid,
															  part_col_typmod,
															  part_col_collation);
					if (startConst->constisnull)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("cannot use NULL with range partition specification"),
								 parser_errposition(pstate, pc->location)));

					boundspec1->lowerdatums = list_make1(makeConst(partkey->parttypid[0],
																   partkey->parttypmod[0],
																   partkey->parttypcoll[0],
																   partkey->parttyplen[0],
																   datumCopy(startConst->constvalue,
																			 partkey->parttypbyval[0],
																			 partkey->parttyplen[0]),
																   false,
																   partkey->parttypbyval[0]));
				}
				else
				{
					/*
					 * Start not-specified means splitting non-default
					 * partition. Use existing partitions lowerdatum as start
					 * for this partition.
					 */
					boundspec1->lowerdatums = boundspec2->lowerdatums;
					boundspec2->lowerdatums = copyObject(boundspec1->upperdatums);
				}

				boundspec1->location = -1;

				partcomp.level = list_length(ancestors) + 1;
				elem = makeNode(GpPartDefElem);
				//elem->tablespacename = NULL;
				//elem->accessMethod = NULL;

				/* create first partition stmt */
				stmts = lappend(stmts, makePartitionCreateStmt(rel, NULL, boundspec1, NULL, elem, &partcomp));
				/* create second partition stmt */
				stmts = lappend(stmts, makePartitionCreateStmt(rel, NULL, boundspec2, NULL, elem, &partcomp));
			}

			foreach (l, stmts)
			{
				/* No planning needed, just make a wrapper PlannedStmt */
				Node *stmt = (Node *) lfirst(l);
				pstmt = makeNode(PlannedStmt);
				pstmt->commandType = CMD_UTILITY;
				pstmt->canSetTag = false;
				pstmt->utilityStmt = stmt;
				pstmt->stmt_location = -1;
				pstmt->stmt_len = 0;
				ProcessUtility(pstmt,
							   synthetic_sql,
							   PROCESS_UTILITY_SUBCOMMAND,
							   NULL,
							   NULL,
							   None_Receiver,
							   NULL);
			}

			/* insert into parent select * from tmp */
			{
				InsertStmt *i = makeNode(InsertStmt);
				SelectStmt *s = makeNode(SelectStmt);
				RangeVar   *parentrv;
				Query *q;
				List *rewritten;
				PlannedStmt *stmt;
				DestReceiver *dest;
				QueryDesc *queryDesc;
				AutoStatsCmdType cmdType;
				Oid relationOid;
				TupleDesc	tupdesc;
				Relation tmprel;

				parentrv = makeRangeVar(get_namespace_name(rel->rd_rel->relnamespace),
										pstrdup(RelationGetRelationName(rel)), -1);
				parentrv->relpersistence = rel->rd_rel->relpersistence;
				tmprel = table_openrv(tmprv, AccessShareLock);

				s->fromClause = list_make1(tmprv);
				i->relation = parentrv;
				i->selectStmt = (Node *)s;

				tupdesc = RelationGetDescr(tmprel);

				for (int attno = 0; attno < tupdesc->natts; attno++)
				{
					Form_pg_attribute att = TupleDescAttr(tupdesc, attno);
					ResTarget  *t;

					t = makeNode(ResTarget);

					if (!att->attisdropped)
					{
						ColumnRef 		   *c;

						c = makeNode(ColumnRef);
						c->location = -1;
						c->fields = lappend(c->fields, makeString(pstrdup(get_namespace_name(RelationGetNamespace(tmprel)))));
						c->fields = lappend(c->fields, makeString(pstrdup(RelationGetRelationName(tmprel))));
						c->fields = lappend(c->fields, makeString(pstrdup(NameStr(att->attname))));
						t->val = (Node *) c;
					}
					else
					{
						/* Use a dummy NULL::int4 column to stand in for any dropped columns. */
						t->val = (Node *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), (Datum) 0, true, true);
					}
					t->location = -1;

					s->targetList = lappend(s->targetList, t);
				}
				table_close(tmprel, AccessShareLock);

				RawStmt *rawstmt = makeNode(RawStmt);
				rawstmt->stmt = (Node *) i;
				rawstmt->stmt_location = -1;
				rawstmt->stmt_len = 0;

				q = parse_analyze(rawstmt, synthetic_sql, NULL, 0, NULL);

				AcquireRewriteLocks(q, true, false);

				/* Rewrite through rule system */
				rewritten = QueryRewrite(q);

				/* We don't expect more or less than one result query */
				Assert(list_length(rewritten) == 1);

				q = (Query *) linitial(rewritten);
				Assert(q->commandType == CMD_SELECT || q->commandType == CMD_INSERT);

				/* plan the query */
				stmt = planner(q, 0, NULL);
				//stmt->intoClause = into;

				/*
				 * Update snapshot command ID to ensure this query sees results of any
				 * previously executed queries.
				 */
				PushCopiedSnapshot(GetActiveSnapshot());
				UpdateActiveSnapshotCommandId();

				/* Create dest receiver for COPY OUT */
				dest = CreateDestReceiver(DestIntoRel);

				/* Create a QueryDesc requesting no output */
				queryDesc = CreateQueryDesc(stmt, debug_query_string,
											GetActiveSnapshot(), InvalidSnapshot,
											dest, NULL, NULL, GP_INSTRUMENT_OPTS);
				PopActiveSnapshot();

				/*
				 * We need to update our snapshot here to make sure we see all
				 * committed work. We have an exclusive lock on the table so no one
				 * will be able to access the table now.
				 */
				PushActiveSnapshot(GetLatestSnapshot());

				/* Step (c) - run on all nodes */
				queryDesc->ddesc = makeNode(QueryDispatchDesc);
				queryDesc->ddesc->useChangedAOOpts = false;

				queryDesc->plannedstmt->query_mem =
					ResourceManagerGetQueryMemoryLimit(queryDesc->plannedstmt);

				elog(NOTICE, "Calling ExecutorStart()");
				ExecutorStart(queryDesc, 0);
				ExecutorRun(queryDesc, ForwardScanDirection, 0L, true);

				autostats_get_cmdtype(queryDesc, &cmdType, &relationOid);

				queryDesc->dest->rDestroy(queryDesc->dest);
				ExecutorFinish(queryDesc);
				ExecutorEnd(queryDesc);

				auto_stats(cmdType,
						   relationOid,
						   queryDesc->es_processed,
						   false);

				FreeQueryDesc(queryDesc);

				/* Restore the old snapshot */
				PopActiveSnapshot();

				CommandCounterIncrement(); /* see the effects of the command */
			}

			/* drop tmp table */
			{
				DropStmt *dropstmt = makeNode(DropStmt);
				dropstmt->objects = list_make1(
					list_make2(makeString(tmprv->schemaname),
							   makeString(tmprv->relname)));
				dropstmt->removeType = OBJECT_TABLE;
				dropstmt->behavior = DROP_CASCADE;
				dropstmt->missing_ok = false;
				stmts = NIL;
				stmts = lappend(stmts, (Node *)dropstmt);
			}
		}
		break;

		default:
			elog(ERROR, "Not implemented");
			break;
	}

	if (rel != origrel)
		table_close(rel, AccessShareLock);

	foreach (l, stmts)
	{
		/* No planning needed, just make a wrapper PlannedStmt */
		Node *stmt = (Node *) lfirst(l);
		pstmt = makeNode(PlannedStmt);
		pstmt->commandType = CMD_UTILITY;
		pstmt->canSetTag = false;
		pstmt->utilityStmt = stmt;
		pstmt->stmt_location = -1;
		pstmt->stmt_len = 0;
		ProcessUtility(pstmt,
					   synthetic_sql,
					   PROCESS_UTILITY_SUBCOMMAND,
					   NULL,
					   NULL,
					   None_Receiver,
					   NULL);
	}
}
