/*
 * set_user.c
 *
 * Similar to SET ROLE but with added logging and some additional
 * control over allowed actions
 *
 * Joe Conway <joe.conway@crunchydata.com>
 *
 * This code is released under the PostgreSQL license.
 *
 * Copyright 2015 Crunchy Data Solutions, Inc.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice
 * and this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL CRUNCHY DATA SOLUTIONS, INC. BE LIABLE TO ANY PARTY
 * FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
 * INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE CRUNCHY DATA SOLUTIONS, INC. HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE CRUNCHY DATA SOLUTIONS, INC. SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE CRUNCHY DATA SOLUTIONS, INC. HAS NO
 * OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
 * MODIFICATIONS.
 */
#include "postgres.h"

#include "pg_config.h"
#if PG_VERSION_NUM < 90300
#include "access/htup.h"
#else
#include "access/htup_details.h"
#endif

#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

static char					   *save_log_statement = NULL;
static Oid						save_OldUserId = InvalidOid;
static ProcessUtility_hook_type prev_hook = NULL;
static bool	Block_AS = false;
static bool	Block_CP = false;

static void PU_hook(Node *parsetree, const char *queryString,
					ProcessUtilityContext context, ParamListInfo params,
					DestReceiver *dest, char *completionTag);

extern Datum set_user(PG_FUNCTION_ARGS);
void _PG_init(void);
void _PG_fini(void);

#if !defined(PG_VERSION_NUM) || PG_VERSION_NUM < 90100
/* prior to 9.1 */
#error "This extension only builds with PostgreSQL 9.1 or later"
#elif PG_VERSION_NUM < 90500
/* 9.1 - 9.4 */
#define GETUSERNAMEFROMID(ouserid) GetUserNameFromId(ouserid)
#else
/* 9.5 & master */
#define GETUSERNAMEFROMID(ouserid) GetUserNameFromId(ouserid, false)
#endif

PG_FUNCTION_INFO_V1(set_user);
Datum
set_user(PG_FUNCTION_ARGS)
{
	bool			argisnull = PG_ARGISNULL(0);
	int				nargs = PG_NARGS();
	HeapTuple		roleTup;
	Oid				OldUserId = GetUserId();
	char		   *olduser = GETUSERNAMEFROMID(OldUserId);
	bool			OldUser_is_superuser = superuser_arg(OldUserId);
	Oid				NewUserId;
	char		   *newuser;
	bool			NewUser_is_superuser;
	char		   *su = "Superuser ";
	char		   *nsu = "";
	MemoryContext	oldcontext;

	if (nargs == 1 && !argisnull)
	{
		if (save_OldUserId != InvalidOid)
			elog(ERROR, "must reset previous user prior to setting again");

		newuser = text_to_cstring(PG_GETARG_TEXT_PP(0));

		/* Look up the username */
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(newuser));
		if (!HeapTupleIsValid(roleTup))
			elog(ERROR, "role \"%s\" does not exist", newuser);

		NewUserId = HeapTupleGetOid(roleTup);
		NewUser_is_superuser = ((Form_pg_authid) GETSTRUCT(roleTup))->rolsuper;
		ReleaseSysCache(roleTup);

		/* keep track of original userid and value of log_statement */
		save_OldUserId = OldUserId;
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		save_log_statement = pstrdup(GetConfigOption("log_statement",
													 false, false));
		MemoryContextSwitchTo(oldcontext);

		/* force logging of everything, at least initially */
		SetConfigOption("log_statement", "all", PGC_SUSET, PGC_S_SESSION);
	}
	else if (nargs == 0 || (nargs == 1 && argisnull))
	{
		if (save_OldUserId == InvalidOid)
			elog(ERROR, "must set user prior to resetting");

		/* get original userid to whom we will reset */
		NewUserId = save_OldUserId;
		newuser = GETUSERNAMEFROMID(NewUserId);
		NewUser_is_superuser = superuser_arg(NewUserId);

		/* flag that we are now reset */
		save_OldUserId = InvalidOid;

		/* restore original log_statement setting */
		SetConfigOption("log_statement", save_log_statement, PGC_SUSET, PGC_S_SESSION);

		pfree(save_log_statement);
		save_log_statement = NULL;
	}
	else
		/* should not happen */
		elog(ERROR, "unexpected argument combination");

	elog(LOG, "%sRole %s transitioning to %sRole %s",
			  OldUser_is_superuser ? su : nsu,
			  olduser,
			  NewUser_is_superuser ? su : nsu,
			  newuser);

	SetCurrentRoleId(NewUserId, NewUser_is_superuser);

	PG_RETURN_TEXT_P(cstring_to_text("OK"));
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("set_user.block_alter_system",
							 "Block ALTER SYSTEM commands",
							 NULL, &Block_AS, false, PGC_SIGHUP,
							 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("set_user.block_copy_program",
							 "Blocks COPY PROGRAM commands",
							 NULL, &Block_CP, false, PGC_SIGHUP,
							 0, NULL, NULL, NULL);

	/* Install hook */
	prev_hook = ProcessUtility_hook;
	ProcessUtility_hook = PU_hook;
}

void
_PG_fini(void)
{
	ProcessUtility_hook = prev_hook;
}

static void
PU_hook(Node *parsetree, const char *queryString,
		ProcessUtilityContext context, ParamListInfo params,
		DestReceiver *dest, char *completionTag)
{
	/* if set_user has been used to transition, enforce set_user GUCs */
	if (save_OldUserId != InvalidOid)
	{
		switch (nodeTag(parsetree))
		{
			case T_AlterSystemStmt:
				if (Block_AS)
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("ALTER SYSTEM blocked by set_user config")));
				break;
			case T_CopyStmt:
				if (((CopyStmt *) parsetree)->is_program && Block_CP)
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("COPY PROGRAM blocked by set_user config")));
				break;
			default:
				break;
		}

		if (prev_hook)
			prev_hook(parsetree, queryString, context,
					  params, dest, completionTag);
		else
			standard_ProcessUtility(parsetree, queryString,
									context, params,
									dest, completionTag);
	}
}