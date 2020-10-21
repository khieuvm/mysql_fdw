/*-------------------------------------------------------------------------
 *
 * mysql_fdw.c
 * 		Foreign-data wrapper for remote MySQL servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2020, EnterpriseDB Corporation.
 *
 * IDENTIFICATION
 * 		mysql_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

/*
 * Must be included before mysql.h as it has some conflicting definitions like
 * list_length, etc.
 */
#include "mysql_fdw.h"

#include <dlfcn.h>
#include <errmsg.h>
#include <mysql.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/reloptions.h"
#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#endif
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "mysql_query.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#if PG_VERSION_NUM < 120000
#include "optimizer/var.h"
#else
#include "optimizer/optimizer.h"
#endif
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/* Declarations for dynamic loading */
PG_MODULE_MAGIC;

int ((mysql_options) (MYSQL *mysql, enum mysql_option option,
					  const void *arg));
int ((mysql_stmt_prepare) (MYSQL_STMT *stmt, const char *query,
						   unsigned long length));
int ((mysql_stmt_execute) (MYSQL_STMT *stmt));
int ((mysql_stmt_fetch) (MYSQL_STMT *stmt));
int ((mysql_query) (MYSQL *mysql, const char *q));
bool ((mysql_stmt_attr_set) (MYSQL_STMT *stmt,
							 enum enum_stmt_attr_type attr_type,
							 const void *attr));
bool ((mysql_stmt_close) (MYSQL_STMT *stmt));
bool ((mysql_stmt_reset) (MYSQL_STMT *stmt));
bool ((mysql_free_result) (MYSQL_RES *result));
bool ((mysql_stmt_bind_param) (MYSQL_STMT *stmt, MYSQL_BIND *bnd));
bool ((mysql_stmt_bind_result) (MYSQL_STMT *stmt, MYSQL_BIND *bnd));

MYSQL_STMT *((mysql_stmt_init) (MYSQL *mysql));
MYSQL_RES *((mysql_stmt_result_metadata) (MYSQL_STMT *stmt));
int ((mysql_stmt_store_result) (MYSQL *mysql));
MYSQL_ROW((mysql_fetch_row) (MYSQL_RES *result));
MYSQL_FIELD *((mysql_fetch_field) (MYSQL_RES *result));
MYSQL_FIELD *((mysql_fetch_fields) (MYSQL_RES *result));
const char *((mysql_error) (MYSQL *mysql));
void ((mysql_close) (MYSQL *sock));
MYSQL_RES *((mysql_store_result) (MYSQL *mysql));
MYSQL *((mysql_init) (MYSQL *mysql));
bool ((mysql_ssl_set) (MYSQL *mysql, const char *key, const char *cert,
					   const char *ca, const char *capath,
					   const char *cipher));
MYSQL *((mysql_real_connect) (MYSQL *mysql, const char *host, const char *user,
							  const char *passwd, const char *db,
							  unsigned int port, const char *unix_socket,
							  unsigned long clientflag));

const char *((mysql_get_host_info) (MYSQL *mysql));
const char *((mysql_get_server_info) (MYSQL *mysql));
int ((mysql_get_proto_info) (MYSQL *mysql));

unsigned int ((mysql_stmt_errno) (MYSQL_STMT *stmt));
unsigned int ((mysql_errno) (MYSQL *mysql));
unsigned int ((mysql_num_fields) (MYSQL_RES *result));
unsigned int ((mysql_num_rows) (MYSQL_RES *result));
unsigned int ((mysql_warning_count)(MYSQL *mysql));

#define DEFAULTE_NUM_ROWS    1000

/*
 * In PG 9.5.1 the number will be 90501,
 * our version is 2.5.5 so number will be 20505
 */
#define CODE_VERSION   20505

typedef struct MySQLFdwRelationInfo
{
	/* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
	List	   *remote_conds;
	List	   *local_conds;

	/* Bitmap of attr numbers we need to fetch from the remote server. */
	Bitmapset  *attrs_used;

	/* Function pushdown surppot in target list */
	bool		is_tlist_func_pushdown;
} MySQLFdwRelationInfo;

extern PGDLLEXPORT void _PG_init(void);
extern Datum mysql_fdw_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(mysql_fdw_handler);
PG_FUNCTION_INFO_V1(mysql_fdw_version);

/*
 * FDW callback routines
 */
static void mysqlExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void mysqlBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *mysqlIterateForeignScan(ForeignScanState *node);
static void mysqlReScanForeignScan(ForeignScanState *node);
static void mysqlEndForeignScan(ForeignScanState *node);

static List *mysqlPlanForeignModify(PlannerInfo *root, ModifyTable *plan,
									Index resultRelation, int subplan_index);
static void mysqlBeginForeignModify(ModifyTableState *mtstate,
									ResultRelInfo *resultRelInfo,
									List *fdw_private, int subplan_index,
									int eflags);
static TupleTableSlot *mysqlExecForeignInsert(EState *estate,
											  ResultRelInfo *resultRelInfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static void mysqlAddForeignUpdateTargets(Query *parsetree,
										 RangeTblEntry *target_rte,
										 Relation target_relation);
static TupleTableSlot *mysqlExecForeignUpdate(EState *estate,
											  ResultRelInfo *resultRelInfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static TupleTableSlot *mysqlExecForeignDelete(EState *estate,
											  ResultRelInfo *resultRelInfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static void mysqlEndForeignModify(EState *estate,
								  ResultRelInfo *resultRelInfo);

static void mysqlGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
								   Oid foreigntableid);
static void mysqlGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
								 Oid foreigntableid);
static bool mysqlAnalyzeForeignTable(Relation relation,
									 AcquireSampleRowsFunc *func,
									 BlockNumber *totalpages);
#if PG_VERSION_NUM >= 90500
static ForeignScan *mysqlGetForeignPlan(PlannerInfo *root,
										RelOptInfo *foreignrel,
										Oid foreigntableid,
										ForeignPath *best_path, List *tlist,
										List *scan_clauses, Plan *outer_plan);
#else
static ForeignScan *mysqlGetForeignPlan(PlannerInfo *root,
										RelOptInfo *foreignrel,
										Oid foreigntableid,
										ForeignPath *best_path, List *tlist,
										List *scan_clauses);
#endif
static void mysqlEstimateCosts(PlannerInfo *root, RelOptInfo *baserel,
							   Cost *startup_cost, Cost *total_cost,
							   Oid foreigntableid);

#if PG_VERSION_NUM >= 90500
static List *mysqlImportForeignSchema(ImportForeignSchemaStmt *stmt,
									  Oid serverOid);
#endif

#if PG_VERSION_NUM >= 110000
static void mysqlBeginForeignInsert(ModifyTableState *mtstate,
									ResultRelInfo *resultRelInfo);
static void mysqlEndForeignInsert(EState *estate,
								  ResultRelInfo *resultRelInfo);
#endif

/*
 * Helper functions
 */
bool mysql_load_library(void);
static void mysql_fdw_exit(int code, Datum arg);
static bool mysql_is_column_unique(Oid foreigntableid);

static void prepare_query_params(PlanState *node,
								 List *fdw_exprs,
								 int numParams,
								 FmgrInfo **param_flinfo,
								 List **param_exprs,
								 const char ***param_values,
								 Oid **param_types);

static void process_query_params(ExprContext *econtext,
								 FmgrInfo *param_flinfo,
								 List *param_exprs,
								 const char **param_values,
								 MYSQL_BIND **mysql_bind_buf,
								 Oid *param_types);

static void bind_stmt_params_and_exec(ForeignScanState *node);

void *mysql_dll_handle = NULL;
static int wait_timeout = WAIT_TIMEOUT;
static int interactive_timeout = INTERACTIVE_TIMEOUT;
static void mysql_error_print(MYSQL *conn);
static void mysql_stmt_error_print(MySQLFdwExecState *festate,
								   const char *msg);
static List *getUpdateTargetAttrs(RangeTblEntry *rte);

/*
 * mysql_load_library function dynamically load the mysql's library
 * libmysqlclient.so. The only reason to load the library using dlopen
 * is that, mysql and postgres both have function with same name like
 * "list_delete", "list_delete" and "list_free" which cause compiler
 * error "duplicate function name" and erroneously linking with a function.
 * This port of the code is used to avoid the compiler error.
 *
 * #define list_delete mysql_list_delete
 * #include <mysql.h>
 * #undef list_delete
 *
 * But system crashed on function mysql_stmt_close function because
 * mysql_stmt_close internally calling "list_delete" function which
 * wrongly binds to postgres' "list_delete" function.
 *
 * The dlopen function provides a parameter "RTLD_DEEPBIND" which
 * solved the binding issue.
 *
 * RTLD_DEEPBIND:
 * Place the lookup scope of the symbols in this library ahead of the
 * global scope. This means that a self-contained library will use its
 * own symbols in preference to global symbols with the same name contained
 * in libraries that have already been loaded.
 */
bool
mysql_load_library(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
	/*
	 * Mac OS/BSD does not support RTLD_DEEPBIND, but it still works without
	 * the RTLD_DEEPBIND
	 */
	mysql_dll_handle = dlopen(_MYSQL_LIBNAME, RTLD_LAZY);
#else
	mysql_dll_handle = dlopen(_MYSQL_LIBNAME, RTLD_LAZY | RTLD_DEEPBIND);
#endif
	if (mysql_dll_handle == NULL)
		return false;

	_mysql_stmt_bind_param = dlsym(mysql_dll_handle, "mysql_stmt_bind_param");
	_mysql_stmt_bind_result = dlsym(mysql_dll_handle, "mysql_stmt_bind_result");
	_mysql_stmt_init = dlsym(mysql_dll_handle, "mysql_stmt_init");
	_mysql_stmt_prepare = dlsym(mysql_dll_handle, "mysql_stmt_prepare");
	_mysql_stmt_execute = dlsym(mysql_dll_handle, "mysql_stmt_execute");
	_mysql_stmt_fetch = dlsym(mysql_dll_handle, "mysql_stmt_fetch");
	_mysql_query = dlsym(mysql_dll_handle, "mysql_query");
	_mysql_stmt_result_metadata = dlsym(mysql_dll_handle, "mysql_stmt_result_metadata");
	_mysql_stmt_store_result = dlsym(mysql_dll_handle, "mysql_stmt_store_result");
	_mysql_fetch_row = dlsym(mysql_dll_handle, "mysql_fetch_row");
	_mysql_fetch_field = dlsym(mysql_dll_handle, "mysql_fetch_field");
	_mysql_fetch_fields = dlsym(mysql_dll_handle, "mysql_fetch_fields");
	_mysql_stmt_close = dlsym(mysql_dll_handle, "mysql_stmt_close");
	_mysql_stmt_reset = dlsym(mysql_dll_handle, "mysql_stmt_reset");
	_mysql_free_result = dlsym(mysql_dll_handle, "mysql_free_result");
	_mysql_error = dlsym(mysql_dll_handle, "mysql_error");
	_mysql_options = dlsym(mysql_dll_handle, "mysql_options");
	_mysql_ssl_set = dlsym(mysql_dll_handle, "mysql_ssl_set");
	_mysql_real_connect = dlsym(mysql_dll_handle, "mysql_real_connect");
	_mysql_close = dlsym(mysql_dll_handle, "mysql_close");
	_mysql_init = dlsym(mysql_dll_handle, "mysql_init");
	_mysql_stmt_attr_set = dlsym(mysql_dll_handle, "mysql_stmt_attr_set");
	_mysql_store_result = dlsym(mysql_dll_handle, "mysql_store_result");
	_mysql_stmt_errno = dlsym(mysql_dll_handle, "mysql_stmt_errno");
	_mysql_errno = dlsym(mysql_dll_handle, "mysql_errno");
	_mysql_num_fields = dlsym(mysql_dll_handle, "mysql_num_fields");
	_mysql_num_rows = dlsym(mysql_dll_handle, "mysql_num_rows");
	_mysql_get_host_info = dlsym(mysql_dll_handle, "mysql_get_host_info");
	_mysql_get_server_info = dlsym(mysql_dll_handle, "mysql_get_server_info");
	_mysql_get_proto_info = dlsym(mysql_dll_handle, "mysql_get_proto_info");
	_mysql_warning_count = dlsym(mysql_dll_handle, "mysql_warning_count");

	if (_mysql_stmt_bind_param == NULL ||
		_mysql_stmt_bind_result == NULL ||
		_mysql_stmt_init == NULL ||
		_mysql_stmt_prepare == NULL ||
		_mysql_stmt_execute == NULL ||
		_mysql_stmt_fetch == NULL ||
		_mysql_query == NULL ||
		_mysql_stmt_result_metadata == NULL ||
		_mysql_stmt_store_result == NULL ||
		_mysql_fetch_row == NULL ||
		_mysql_fetch_field == NULL ||
		_mysql_fetch_fields == NULL ||
		_mysql_stmt_close == NULL ||
		_mysql_stmt_reset == NULL ||
		_mysql_free_result == NULL ||
		_mysql_error == NULL ||
		_mysql_options == NULL ||
		_mysql_ssl_set == NULL ||
		_mysql_real_connect == NULL ||
		_mysql_close == NULL ||
		_mysql_init == NULL ||
		_mysql_stmt_attr_set == NULL ||
		_mysql_store_result == NULL ||
		_mysql_stmt_errno == NULL ||
		_mysql_errno == NULL ||
		_mysql_num_fields == NULL ||
		_mysql_num_rows == NULL ||
		_mysql_get_host_info == NULL ||
		_mysql_get_server_info == NULL ||
		_mysql_get_proto_info == NULL ||
		_mysql_warning_count == NULL)
		return false;

	return true;
}

/*
 * Library load-time initialization, sets on_proc_exit() callback for
 * backend shutdown.
 */
void
_PG_init(void)
{
	if (!mysql_load_library())
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("failed to load the mysql query: \n%s", dlerror()),
				 errhint("Export LD_LIBRARY_PATH to locate the library.")));

	DefineCustomIntVariable("mysql_fdw.wait_timeout",
							"Server-side wait_timeout",
							"Set the maximum wait_timeout"
							"use to set the MySQL session timeout",
							&wait_timeout,
							WAIT_TIMEOUT,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("mysql_fdw.interactive_timeout",
							"Server-side interactive timeout",
							"Set the maximum interactive timeout"
							"use to set the MySQL session timeout",
							&interactive_timeout,
							INTERACTIVE_TIMEOUT,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	on_proc_exit(&mysql_fdw_exit, PointerGetDatum(NULL));
}

/*
 * mysql_fdw_exit
 * 		Exit callback function.
 */
static void
mysql_fdw_exit(int code, Datum arg)
{
	mysql_cleanup_connection();
}

/*
 * Foreign-data wrapper handler function: return
 * a struct with pointers to my callback routines.
 */
Datum
mysql_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	/* Functions for scanning foreign tables */
	fdwroutine->GetForeignRelSize = mysqlGetForeignRelSize;
	fdwroutine->GetForeignPaths = mysqlGetForeignPaths;
	fdwroutine->GetForeignPlan = mysqlGetForeignPlan;
	fdwroutine->BeginForeignScan = mysqlBeginForeignScan;
	fdwroutine->IterateForeignScan = mysqlIterateForeignScan;
	fdwroutine->ReScanForeignScan = mysqlReScanForeignScan;
	fdwroutine->EndForeignScan = mysqlEndForeignScan;

	/* Functions for updating foreign tables */
	fdwroutine->AddForeignUpdateTargets = mysqlAddForeignUpdateTargets;
	fdwroutine->PlanForeignModify = mysqlPlanForeignModify;
	fdwroutine->BeginForeignModify = mysqlBeginForeignModify;
	fdwroutine->ExecForeignInsert = mysqlExecForeignInsert;
	fdwroutine->ExecForeignUpdate = mysqlExecForeignUpdate;
	fdwroutine->ExecForeignDelete = mysqlExecForeignDelete;
	fdwroutine->EndForeignModify = mysqlEndForeignModify;

	/* Support functions for EXPLAIN */
	fdwroutine->ExplainForeignScan = mysqlExplainForeignScan;

	/* Support functions for ANALYZE */
	fdwroutine->AnalyzeForeignTable = mysqlAnalyzeForeignTable;

	/* Support functions for IMPORT FOREIGN SCHEMA */
#if PG_VERSION_NUM >= 90500
	fdwroutine->ImportForeignSchema = mysqlImportForeignSchema;
#endif

#if PG_VERSION_NUM >= 110000
	/* Partition routing and/or COPY from */
	fdwroutine->BeginForeignInsert = mysqlBeginForeignInsert;
	fdwroutine->EndForeignInsert = mysqlEndForeignInsert;
#endif

	PG_RETURN_POINTER(fdwroutine);
}

/*
 * mysqlBeginForeignScan
 * 		Initiate access to the database
 */
static void
mysqlBeginForeignScan(ForeignScanState *node, int eflags)
{
	TupleTableSlot *tupleSlot = node->ss.ss_ScanTupleSlot;
	TupleDesc	tupleDescriptor = tupleSlot->tts_tupleDescriptor;
	MYSQL	   *conn;
	RangeTblEntry *rte;
	MySQLFdwExecState *festate;
	EState	   *estate = node->ss.ps.state;
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	mysql_opt  *options;
	ListCell   *lc;
	int			atindex = 0;
	unsigned long prefetch_rows = MYSQL_PREFETCH_ROWS;
	unsigned long type = (unsigned long) CURSOR_TYPE_READ_ONLY;
	Oid			userid;
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;
	char		timeout[255];
	int			numParams;
	int               rtindex;

	/*
	 * We'll save private state in node->fdw_state.
	 */
	festate = (MySQLFdwExecState *) palloc0(sizeof(MySQLFdwExecState));
	node->fdw_state = (void *) festate;

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.
	 */
	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
		rtindex = bms_next_member(fsplan->fs_relids, -1);
	rte = exec_rt_fetch(rtindex, estate);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(rte->relid);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/* Fetch the options */
	options = mysql_get_options(rte->relid);

	/*
	 * Get the already connected connection, otherwise connect and get the
	 * connection handle.
	 */
	conn = mysql_get_connection(server, user, options);

	/* Stash away the state info we have already */
	festate->query = strVal(list_nth(fsplan->fdw_private, 0));
	festate->retrieved_attrs = list_nth(fsplan->fdw_private, 1);
	festate->conn = conn;
	festate->query_executed = false;

#if PG_VERSION_NUM >= 110000
	festate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "mysql_fdw temporary data",
											  ALLOCSET_DEFAULT_SIZES);
#else
	festate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "mysql_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);
#endif

	if (wait_timeout > 0)
	{
		/* Set the session timeout in seconds */
		sprintf(timeout, "SET wait_timeout = %d", wait_timeout);
		mysql_query(festate->conn, timeout);
	}

	if (interactive_timeout > 0)
	{
		/* Set the session timeout in seconds */
		sprintf(timeout, "SET interactive_timeout = %d", interactive_timeout);
		mysql_query(festate->conn, timeout);
	}

	/* Change sql_mode to TRADITIONAL to catch warning "Division by 0" */
	mysql_query(festate->conn, "SET sql_mode='TRADITIONAL'");

	/* Initialize the MySQL statement */
	festate->stmt = mysql_stmt_init(festate->conn);
	if (festate->stmt == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("failed to initialize the mysql query: \n%s",
						mysql_error(festate->conn))));

	/* Prepare MySQL statement */
	if (mysql_stmt_prepare(festate->stmt, festate->query,
						   strlen(festate->query)) != 0)
		mysql_stmt_error_print(festate, "failed to prepare the MySQL query");

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Prepare for output conversion of parameters used in remote query. */
	numParams = list_length(fsplan->fdw_exprs);
	festate->numParams = numParams;
	if (numParams > 0)
		prepare_query_params((PlanState *) node,
							 fsplan->fdw_exprs,
							 numParams,
							 &festate->param_flinfo,
							 &festate->param_exprs,
							 &festate->param_values,
							 &festate->param_types);

	/* int column_count = mysql_num_fields(festate->meta); */

	/* Set the statement as cursor type */
	mysql_stmt_attr_set(festate->stmt, STMT_ATTR_CURSOR_TYPE, (void *) &type);

	/* Set the pre-fetch rows */
	mysql_stmt_attr_set(festate->stmt, STMT_ATTR_PREFETCH_ROWS,
						(void *) &prefetch_rows);

	festate->table = (mysql_table *) palloc0(sizeof(mysql_table));
	festate->table->column = (mysql_column *) palloc0(sizeof(mysql_column) * tupleDescriptor->natts);
	festate->table->mysql_bind = (MYSQL_BIND *) palloc0(sizeof(MYSQL_BIND) * tupleDescriptor->natts);

	festate->table->mysql_res = mysql_stmt_result_metadata(festate->stmt);
	if (NULL == festate->table->mysql_res)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("failed to retrieve query result set metadata: \n%s",
						mysql_error(festate->conn))));

	festate->table->mysql_fields = mysql_fetch_fields(festate->table->mysql_res);

	foreach(lc, festate->retrieved_attrs)
	{
		int			attnum = lfirst_int(lc) - 1;
		Oid			pgtype = TupleDescAttr(tupleDescriptor, attnum)->atttypid;
		int32		pgtypmod = TupleDescAttr(tupleDescriptor, attnum)->atttypmod;

		if (TupleDescAttr(tupleDescriptor, attnum)->attisdropped)
			continue;

		festate->table->column[atindex].mysql_bind = &festate->table->mysql_bind[atindex];

		mysql_bind_result(pgtype, pgtypmod,
						  &festate->table->mysql_fields[atindex],
						  &festate->table->column[atindex]);
		atindex++;
	}

	/* Bind the results pointers for the prepare statements */
	if (mysql_stmt_bind_result(festate->stmt, festate->table->mysql_bind) != 0)
		mysql_stmt_error_print(festate, "failed to bind the MySQL query");
}

/*
 * mysqlIterateForeignScan
 * 		Iterate and get the rows one by one from  MySQL and placed in tuple
 * 		slot
 */
static TupleTableSlot *
mysqlIterateForeignScan(ForeignScanState *node)
{
	MySQLFdwExecState *festate = (MySQLFdwExecState *) node->fdw_state;
	TupleTableSlot *tupleSlot = node->ss.ss_ScanTupleSlot;
	TupleDesc	tupleDescriptor = tupleSlot->tts_tupleDescriptor;
	int			attid;
	ListCell   *lc;
	int			rc = 0;

	memset(tupleSlot->tts_values, 0, sizeof(Datum) * tupleDescriptor->natts);
	memset(tupleSlot->tts_isnull, true, sizeof(bool) * tupleDescriptor->natts);

	ExecClearTuple(tupleSlot);

	/*
	 * If this is the first call after Begin or ReScan, we need to bind the
	 * params and execute the query.
	 */
	if (!festate->query_executed)
		bind_stmt_params_and_exec(node);

	attid = 0;
	rc = mysql_stmt_fetch(festate->stmt);
	if (rc == 0)
	{
		foreach(lc, festate->retrieved_attrs)
		{
			int			attnum = lfirst_int(lc) - 1;
			Oid			pgtype = TupleDescAttr(tupleDescriptor, attnum)->atttypid;
			int32		pgtypmod = TupleDescAttr(tupleDescriptor, attnum)->atttypmod;

			tupleSlot->tts_isnull[attnum] = festate->table->column[attid].is_null;
			if (!festate->table->column[attid].is_null)
				tupleSlot->tts_values[attnum] = mysql_convert_to_pg(pgtype,
																	pgtypmod,
																	&festate->table->column[attid]);

			attid++;
		}

		ExecStoreVirtualTuple(tupleSlot);
	}
	else if (rc == 1)
	{
		/*
		 * Error occurred. Error code and message can be obtained by calling
		 * mysql_stmt_errno() and mysql_stmt_error().
		 */
	}
	else if (rc == MYSQL_NO_DATA)
	{
		/*
		 * No more rows/data exists
		 */
	}
	else if (rc == MYSQL_DATA_TRUNCATED)
	{
		/* Data truncation occurred */
		/*
		 * MYSQL_DATA_TRUNCATED is returned when truncation reporting is
		 * enabled. To determine which column values were truncated when this
		 * value is returned, check the error members of the MYSQL_BIND
		 * structures used for fetching values. Truncation reporting is
		 * enabled by default, but can be controlled by calling
		 * mysql_options() with the MYSQL_REPORT_DATA_TRUNCATION option.
		 */
	}

	return tupleSlot;
}


/*
 * mysqlExplainForeignScan
 * 		Produce extra output for EXPLAIN
 */
static void
mysqlExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	MySQLFdwExecState *festate = (MySQLFdwExecState *) node->fdw_state;
	mysql_opt  *options;
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	int rtindex;
	RangeTblEntry *rte;
	EState *estate = node->ss.ps.state;

	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
		rtindex = bms_next_member(fsplan->fs_relids, -1);
	rte = exec_rt_fetch(rtindex, estate);

	/* Fetch options */
	options = mysql_get_options(rte->relid);

	/* Give some possibly useful info about startup costs */
	if (es->verbose)
	{
		if (strcmp(options->svr_address, "127.0.0.1") == 0 ||
			strcmp(options->svr_address, "localhost") == 0)
#if PG_VERSION_NUM >= 110000
			ExplainPropertyInteger("Local server startup cost", NULL, 10, es);
#else
			ExplainPropertyLong("Local server startup cost", 10, es);
#endif
		else
#if PG_VERSION_NUM >= 110000
			ExplainPropertyInteger("Remote server startup cost", NULL, 25, es);
#else
			ExplainPropertyLong("Remote server startup cost", 25, es);
#endif
		ExplainPropertyText("Remote query", festate->query, es);
	}
}

/*
 * mysqlEndForeignScan
 * 		Finish scanning foreign table and dispose objects used for this scan
 */
static void
mysqlEndForeignScan(ForeignScanState *node)
{
	MySQLFdwExecState *festate = (MySQLFdwExecState *) node->fdw_state;

	if (festate->table && festate->table->mysql_res)
	{
		mysql_free_result(festate->table->mysql_res);
		festate->table->mysql_res = NULL;
	}

	if (festate->stmt)
	{
		mysql_stmt_close(festate->stmt);
		festate->stmt = NULL;
	}
}

/*
 * mysqlReScanForeignScan
 * 		Rescan table, possibly with new parameters
 */
static void
mysqlReScanForeignScan(ForeignScanState *node)
{
	MySQLFdwExecState *festate = (MySQLFdwExecState *) node->fdw_state;

	/*
	 * Set the query_executed flag to false so that the query will be executed
	 * in mysqlIterateForeignScan().
	 */
	festate->query_executed = false;

}

/*
 * mysqlGetForeignRelSize
 * 		Create a FdwPlan for a scan on the foreign table
 */
static void
mysqlGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
					   Oid foreigntableid)
{
	double		rows = 0;
	double		filtered = 0;
	MYSQL	   *conn;
	MYSQL_ROW	row;
	Bitmapset  *attrs_used = NULL;
	mysql_opt  *options;
	Oid			userid = GetUserId();
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;
	MySQLFdwRelationInfo *fpinfo;
	ListCell   *lc;

	fpinfo = (MySQLFdwRelationInfo *) palloc0(sizeof(MySQLFdwRelationInfo));
	baserel->fdw_private = (void *) fpinfo;

	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/* Fetch options */
	options = mysql_get_options(foreigntableid);

	/* Connect to the server */
	conn = mysql_get_connection(server, user, options);

	mysql_query(conn, "SET sql_mode='ANSI_QUOTES'");

#if PG_VERSION_NUM >= 90600
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &attrs_used);
#else
	pull_varattnos((Node *) baserel->reltargetlist, baserel->relid,
				   &attrs_used);
#endif

	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (mysql_is_foreign_expr(root, baserel, ri->clause))
			fpinfo->remote_conds = lappend(fpinfo->remote_conds, ri);
		else
			fpinfo->local_conds = lappend(fpinfo->local_conds, ri);
	}

#if PG_VERSION_NUM >= 90600
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &fpinfo->attrs_used);
#else
	pull_varattnos((Node *) baserel->reltargetlist, baserel->relid,
				   &fpinfo->attrs_used);
#endif

	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &fpinfo->attrs_used);
	}

	if (options->use_remote_estimate)
	{
		StringInfoData sql;
		MYSQL_RES  *result = NULL;
		List	   *retrieved_attrs = NULL;
		List	   *params_list = NULL;

		initStringInfo(&sql);
		appendStringInfo(&sql, "EXPLAIN ");

		mysql_deparse_select(&sql, root, baserel, fpinfo->attrs_used,
							 options->svr_table, &retrieved_attrs, NULL);

		if (fpinfo->remote_conds)
			mysql_append_where_clause(&sql, root, baserel,
									  fpinfo->remote_conds, true,
									  &params_list);

		if (mysql_query(conn, sql.data) != 0)
			mysql_error_print(conn);

		result = mysql_store_result(conn);
		if (result)
		{
			int			num_fields;

			/*
			 * MySQL provide numbers of rows per table invole in the statement,
			 * but we don't have problem with it because we are sending
			 * separate query per table in FDW.
			 */
			row = mysql_fetch_row(result);
			num_fields = mysql_num_fields(result);
			if (row)
			{
				MYSQL_FIELD *field;
				int			i;

				for (i = 0; i < num_fields; i++)
				{
					field = mysql_fetch_field(result);
					if (!row[i])
						continue;
					else if (strcmp(field->name, "rows") == 0)
						rows = atof(row[i]);
					else if (strcmp(field->name, "filtered") == 0)
						filtered = atof(row[i]);
				}
			}
			mysql_free_result(result);
		}
	}
	if (rows > 0)
		rows = ((rows + 1) * filtered) / 100;
	else
		rows = DEFAULTE_NUM_ROWS;

	baserel->rows = rows;
	baserel->tuples = rows;
}

static bool
mysql_is_column_unique(Oid foreigntableid)
{
	StringInfoData sql;
	MYSQL	   *conn;
	MYSQL_RES  *result;
	mysql_opt  *options;
	Oid			userid = GetUserId();
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;

	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/* Fetch the options */
	options = mysql_get_options(foreigntableid);

	/* Connect to the server */
	conn = mysql_get_connection(server, user, options);

	/* Build the query */
	initStringInfo(&sql);

	/*
	 * Construct the query by prefixing the database name so that it can lookup
	 * in correct database.
	 */
	appendStringInfo(&sql, "EXPLAIN %s.%s", options->svr_database,
					 options->svr_table);
	if (mysql_query(conn, sql.data) != 0)
		mysql_error_print(conn);

	result = mysql_store_result(conn);
	if (result)
	{
		int			num_fields = mysql_num_fields(result);
		MYSQL_ROW	row;

		row = mysql_fetch_row(result);
		if (row && num_fields > 3)
		{
			if ((strcmp(row[3], "PRI") == 0) || (strcmp(row[3], "UNI")) == 0)
			{
				mysql_free_result(result);
				return true;
			}
		}
		mysql_free_result(result);
	}

	return false;
}

/*
 * mysqlEstimateCosts
 * 		Estimate the remote query cost
 */
static void
mysqlEstimateCosts(PlannerInfo *root, RelOptInfo *baserel, Cost *startup_cost,
				   Cost *total_cost, Oid foreigntableid)
{
	mysql_opt  *options;

	/* Fetch options */
	options = mysql_get_options(foreigntableid);

	/* Local databases are probably faster */
	if (strcmp(options->svr_address, "127.0.0.1") == 0 ||
		strcmp(options->svr_address, "localhost") == 0)
		*startup_cost = 10;
	else
		*startup_cost = 25;

	*total_cost = baserel->rows + *startup_cost;
}

/*
 * mysqlGetForeignPaths
 * 		Get the foreign paths
 */
static void
mysqlGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
					 Oid foreigntableid)
{
	Cost		startup_cost;
	Cost		total_cost;

	/* Estimate costs */
	mysqlEstimateCosts(root, baserel, &startup_cost, &total_cost,
					   foreigntableid);

	/* Create a ForeignPath node and add it as only possible path */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
#if PG_VERSION_NUM >= 90600
									 NULL,	/* default pathtarget */
#endif
									 baserel->rows,
									 startup_cost,
									 total_cost,
									 NIL,	/* no pathkeys */
									 baserel->lateral_relids,
#if PG_VERSION_NUM >= 90500
									 NULL,	/* no extra plan */
#endif
									 NULL));	/* no fdw_private data */
}


/*
 * mysqlGetForeignPlan
 * 		Get a foreign scan plan node
 */
#if PG_VERSION_NUM >= 90500
static ForeignScan *
mysqlGetForeignPlan(PlannerInfo *root, RelOptInfo *foreignrel,
					Oid foreigntableid, ForeignPath *best_path,
					List *tlist, List *scan_clauses, Plan *outer_plan)
#else
static ForeignScan *
mysqlGetForeignPlan(PlannerInfo *root, RelOptInfo *foreignrel,
					Oid foreigntableid, ForeignPath *best_path,
					List *tlist, List *scan_clauses)
#endif
{
	MySQLFdwRelationInfo *fpinfo = (MySQLFdwRelationInfo *) foreignrel->fdw_private;
	Index		scan_relid = foreignrel->relid;
	List	   *fdw_private;
	List	   *local_exprs = NIL;
	List	   *remote_exprs = NIL;
	List	   *params_list = NIL;
	List       *fdw_scan_tlist = NIL;
	List	   *remote_conds = NIL;
	StringInfoData sql;
	mysql_opt  *options;
	List	   *retrieved_attrs;
	ListCell   *lc;

	/* Fetch options */
	options = mysql_get_options(foreigntableid);

	/* Decide to execute function pushdown support in the target list. */
	fpinfo->is_tlist_func_pushdown = mysql_is_foreign_function_tlist(root, foreignrel, tlist);

	/*
	 * Build the query string to be sent for execution, and identify
	 * expressions to be sent as parameters.
	 */

	/* Build the query */
	initStringInfo(&sql);

	/*
	 * Separate the scan_clauses into those that can be executed remotely and
	 * those that can't.  baserestrictinfo clauses that were previously
	 * determined to be safe or unsafe by classifyConditions are shown in
	 * fpinfo->remote_conds and fpinfo->local_conds.  Anything else in the
	 * scan_clauses list will be a join clause, which we have to check for
	 * remote-safety.
	 *
	 * Note: the join clauses we see here should be the exact same ones
	 * previously examined by postgresGetForeignPaths.  Possibly it'd be worth
	 * passing forward the classification work done then, rather than
	 * repeating it here.
	 *
	 * This code must match "extract_actual_clauses(scan_clauses, false)"
	 * except for the additional decision about remote versus local execution.
	 * Note however that we only strip the RestrictInfo nodes from the
	 * local_exprs list, since appendWhereClause expects a list of
	 * RestrictInfos.
	 */
	foreach(lc, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(IsA(rinfo, RestrictInfo));

		/* Ignore any pseudoconstants, they're dealt with elsewhere */
		if (rinfo->pseudoconstant)
			continue;

		if (list_member_ptr(fpinfo->remote_conds, rinfo))
		{
			remote_conds = lappend(remote_conds, rinfo);
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		}
		else if (list_member_ptr(fpinfo->local_conds, rinfo))
			local_exprs = lappend(local_exprs, rinfo->clause);
		else if (mysql_is_foreign_expr(root, foreignrel, rinfo->clause))
		{
			remote_conds = lappend(remote_conds, rinfo);
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		}
		else
			local_exprs = lappend(local_exprs, rinfo->clause);
	}

	if (fpinfo->is_tlist_func_pushdown == true)
	{
		/*
		 * Join relation or upper relation - set scan_relid to 0.
		 */
		scan_relid = 0;
		
		fdw_scan_tlist = copyObject(tlist);
		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			fdw_scan_tlist = add_to_flat_tlist(fdw_scan_tlist,
											   pull_var_clause((Node *) rinfo->clause,
															   PVC_RECURSE_PLACEHOLDERS));
		}
	}

	/* Cannot compile this code for plain PostgreSQL, which doesn't have is_tlist_pushdown member */
	mysql_deparse_select(&sql, root, foreignrel, fpinfo->attrs_used,
						 options->svr_table, &retrieved_attrs, fdw_scan_tlist);

	if (remote_conds)
		mysql_append_where_clause(&sql, root, foreignrel, remote_conds,
								  true, &params_list);

	if (foreignrel->relid == root->parse->resultRelation &&
		(root->parse->commandType == CMD_UPDATE ||
		 root->parse->commandType == CMD_DELETE))
	{
		/* Relation is UPDATE/DELETE target, so use FOR UPDATE */
		appendStringInfoString(&sql, " FOR UPDATE");
	}

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwScanPrivateIndex, above.
	 */

	fdw_private = list_make3(makeString(sql.data), retrieved_attrs, fdw_scan_tlist);

	/*
	 * Create the ForeignScan node from target list, local filtering
	 * expressions, remote parameter expressions, and FDW private information.
	 *
	 * Note that the remote parameter expressions are stored in the fdw_exprs
	 * field of the finished plan node; we can't keep them in private state
	 * because then they wouldn't be subject to later planner processing.
	 */
#if PG_VERSION_NUM >= 90500
	return make_foreignscan(tlist, local_exprs, scan_relid, params_list,
							fdw_private, fdw_scan_tlist, NIL, outer_plan);
#else
	return make_foreignscan(tlist, local_exprs, scan_relid, params_list,
							fdw_private);
#endif
}

/*
 * mysqlAnalyzeForeignTable
 * 		Implement stats collection
 */
static bool
mysqlAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func,
						 BlockNumber *totalpages)
{
	StringInfoData sql;
	double		table_size = 0;
	MYSQL	   *conn;
	MYSQL_RES  *result;
	Oid			foreignTableId = RelationGetRelid(relation);
	mysql_opt  *options;
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;

	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(relation->rd_rel->relowner, server->serverid);

	/* Fetch options */
	options = mysql_get_options(foreignTableId);
	Assert(options->svr_database != NULL && options->svr_table != NULL);

	/* Connect to the server */
	conn = mysql_get_connection(server, user, options);

	/* Build the query */
	initStringInfo(&sql);
	mysql_deparse_analyze(&sql, options->svr_database, options->svr_table);

	if (mysql_query(conn, sql.data) != 0)
		mysql_error_print(conn);

	result = mysql_store_result(conn);

	/*
	 * To get the table size in ANALYZE operation, we run a SELECT query by
	 * passing the database name and table name.  So if the remote table is not
	 * present, then we end up getting zero rows.  Throw an error in that case.
	 */
	if (mysql_num_rows(result) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
				 errmsg("relation %s.%s does not exist", options->svr_database,
						options->svr_table)));

	if (result)
	{
		MYSQL_ROW	row;

		row = mysql_fetch_row(result);
		table_size = atof(row[0]);
		mysql_free_result(result);
	}

	*totalpages = table_size / MYSQL_BLKSIZ;

	return false;
}

static List *
mysqlPlanForeignModify(PlannerInfo *root,
					   ModifyTable *plan,
					   Index resultRelation,
					   int subplan_index)
{

	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	List	   *targetAttrs = NIL;
	StringInfoData sql;
	char	   *attname;
	Oid			foreignTableId;

	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
#if PG_VERSION_NUM < 130000
	rel = heap_open(rte->relid, NoLock);
#else
	rel = table_open(rte->relid, NoLock);
#endif

	foreignTableId = RelationGetRelid(rel);

	if (!mysql_is_column_unique(foreignTableId))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("first column of remote table must be unique for INSERT/UPDATE/DELETE operation")));

	/*
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, if there are BEFORE ROW UPDATE triggers on the
	 * foreign table, we transmit all columns like INSERT; else we transmit
	 * only columns that were explicitly targets of the UPDATE, so as to avoid
	 * unnecessary data transmission.  (We can't do that for INSERT since we
	 * would miss sending default values for columns not listed in the source
	 * statement, and for UPDATE if there are BEFORE ROW UPDATE triggers since
	 * those triggers might change values for non-target columns, in which
	 * case we would miss sending changed values for those columns.)
	 */
	if (operation == CMD_INSERT ||
		(operation == CMD_UPDATE &&
		 rel->trigdesc &&
		 rel->trigdesc->trig_update_before_row))
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		/*
		 * If it is an UPDATE operation, check for row identifier column in
		 * target attribute list by calling getUpdateTargetAttrs().
		 */
		if (operation == CMD_UPDATE)
			getUpdateTargetAttrs(rte);

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		targetAttrs = getUpdateTargetAttrs(rte);
		/* We also want the rowid column to be available for the update */
		targetAttrs = lcons_int(1, targetAttrs);
	}
	else
		targetAttrs = lcons_int(1, targetAttrs);

#if PG_VERSION_NUM >= 110000
	attname = get_attname(foreignTableId, 1, false);
#else
	attname = get_relid_attribute_name(foreignTableId, 1);
#endif

	/*
	 * Construct the SQL command string.
	 */
	switch (operation)
	{
		case CMD_INSERT:
			mysql_deparse_insert(&sql, root, resultRelation, rel, targetAttrs);
			break;
		case CMD_UPDATE:
			mysql_deparse_update(&sql, root, resultRelation, rel, targetAttrs,
								 attname);
			break;
		case CMD_DELETE:
			mysql_deparse_delete(&sql, root, resultRelation, rel, attname);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	if (plan->returningLists)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("RETURNING is not supported by this FDW")));

#if PG_VERSION_NUM < 130000
	heap_close(rel, NoLock);
#else
	table_close(rel, NoLock);
#endif

	return list_make2(makeString(sql.data), targetAttrs);
}

/*
 * mysqlBeginForeignModify
 * 		Begin an insert/update/delete operation on a foreign table
 */
static void
mysqlBeginForeignModify(ModifyTableState *mtstate,
						ResultRelInfo *resultRelInfo,
						List *fdw_private,
						int subplan_index,
						int eflags)
{
	MySQLFdwExecState *fmstate;
	EState	   *estate = mtstate->ps.state;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	AttrNumber	n_params;
	Oid			typefnoid = InvalidOid;
	bool		isvarlena = false;
	ListCell   *lc;
	Oid			foreignTableId = InvalidOid;
	RangeTblEntry *rte;
	Oid			userid;
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;

	rte = rt_fetch(resultRelInfo->ri_RangeTableIndex, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	foreignTableId = RelationGetRelid(rel);

	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case. resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Begin constructing MySQLFdwExecState. */
	fmstate = (MySQLFdwExecState *) palloc0(sizeof(MySQLFdwExecState));

	fmstate->rel = rel;
	fmstate->mysqlFdwOptions = mysql_get_options(foreignTableId);
	fmstate->conn = mysql_get_connection(server, user,
										 fmstate->mysqlFdwOptions);

	fmstate->query = strVal(list_nth(fdw_private, 0));
	fmstate->retrieved_attrs = (List *) list_nth(fdw_private, 1);

	n_params = list_length(fmstate->retrieved_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;
#if PG_VERSION_NUM >= 110000
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "mysql_fdw temporary data",
											  ALLOCSET_DEFAULT_SIZES);
#else
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "mysql_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);
#endif

	/* Set up for remaining transmittable parameters */
	foreach(lc, fmstate->retrieved_attrs)
	{
		int			attnum = lfirst_int(lc);
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(rel),
											   attnum - 1);

		Assert(!attr->attisdropped);

		getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
		fmstate->p_nums++;
	}
	Assert(fmstate->p_nums <= n_params);

	n_params = list_length(fmstate->retrieved_attrs);

	/* Initialize mysql statment */
	fmstate->stmt = mysql_stmt_init(fmstate->conn);
	if (!fmstate->stmt)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("failed to initialize the MySQL query: \n%s",
						mysql_error(fmstate->conn))));

	/* Prepare mysql statment */
	if (mysql_stmt_prepare(fmstate->stmt, fmstate->query,
						   strlen(fmstate->query)) != 0)
		mysql_stmt_error_print(fmstate, "failed to prepare the MySQL query");

	resultRelInfo->ri_FdwState = fmstate;
}

/*
 * mysqlExecForeignInsert
 * 		Insert one row into a foreign table
 */
static TupleTableSlot *
mysqlExecForeignInsert(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	MySQLFdwExecState *fmstate;
	MYSQL_BIND *mysql_bind_buffer;
	ListCell   *lc;
	int			n_params;
	MemoryContext oldcontext;
	bool	   *isnull;

	fmstate = (MySQLFdwExecState *) resultRelInfo->ri_FdwState;
	n_params = list_length(fmstate->retrieved_attrs);

	oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

	mysql_bind_buffer = (MYSQL_BIND *) palloc0(sizeof(MYSQL_BIND) * n_params);
	isnull = (bool *) palloc0(sizeof(bool) * n_params);

	mysql_query(fmstate->conn, "SET sql_mode='ANSI_QUOTES'");

	foreach(lc, fmstate->retrieved_attrs)
	{
		int			attnum = lfirst_int(lc) - 1;
		Oid			type = TupleDescAttr(slot->tts_tupleDescriptor, attnum)->atttypid;
		Datum		value;

		value = slot_getattr(slot, attnum + 1, &isnull[attnum]);

		mysql_bind_sql_var(type, attnum, value, mysql_bind_buffer,
						   &isnull[attnum]);
	}

	/* Bind values */
	if (mysql_stmt_bind_param(fmstate->stmt, mysql_bind_buffer) != 0)
		mysql_stmt_error_print(fmstate, "failed to bind the MySQL query");

	/* Execute the query */
	if (mysql_stmt_execute(fmstate->stmt) != 0)
		mysql_stmt_error_print(fmstate, "failed to execute the MySQL query");

	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(fmstate->temp_cxt);
	return slot;
}

static TupleTableSlot *
mysqlExecForeignUpdate(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	MySQLFdwExecState *fmstate = (MySQLFdwExecState *) resultRelInfo->ri_FdwState;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	MYSQL_BIND *mysql_bind_buffer;
	Oid			foreignTableId = RelationGetRelid(rel);
	bool		is_null = false;
	ListCell   *lc;
	int			bindnum = 0;
	Oid			typeoid;
	Datum		value;
	int			n_params;
	bool	   *isnull;
	Datum		new_value;
	HeapTuple	tuple;
	Form_pg_attribute attr;
	bool		found_row_id_col = false;

	n_params = list_length(fmstate->retrieved_attrs);

	mysql_bind_buffer = (MYSQL_BIND *) palloc0(sizeof(MYSQL_BIND) * n_params);
	isnull = (bool *) palloc0(sizeof(bool) * n_params);

	/* Bind the values */
	foreach(lc, fmstate->retrieved_attrs)
	{
		int			attnum = lfirst_int(lc);
		Oid			type;

		/*
		 * The first attribute cannot be in the target list attribute.  Set the
		 * found_row_id_col to true once we find it so that we can fetch the
		 * value later.
		 */
		if (attnum == 1)
		{
			found_row_id_col = true;
			continue;
		}

		type = TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1)->atttypid;
		value = slot_getattr(slot, attnum, (bool *) (&isnull[bindnum]));

		mysql_bind_sql_var(type, bindnum, value, mysql_bind_buffer,
						   &isnull[bindnum]);
		bindnum++;
	}

	/*
	 * Since we add a row identifier column in the target list always, so
	 * found_row_id_col flag should be true.
	 */
	if (!found_row_id_col)
		elog(ERROR, "missing row identifier column value in UPDATE");

	new_value = slot_getattr(slot, 1, &is_null);

	/*
	 * Get the row identifier column value that was passed up as a resjunk
	 * column and compare that value with the new value to identify if that
	 * value is changed.
	 */
	value = ExecGetJunkAttribute(planSlot, 1, &is_null);

	tuple = SearchSysCache2(ATTNUM,
							ObjectIdGetDatum(foreignTableId),
							Int16GetDatum(1));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 1, foreignTableId);

	attr = (Form_pg_attribute) GETSTRUCT(tuple);
	typeoid = attr->atttypid;

	if (DatumGetPointer(new_value) != NULL && DatumGetPointer(value) != NULL)
	{
		Datum		n_value = new_value;
		Datum 		o_value = value;

		/* If the attribute type is varlena then need to detoast the datums. */
		if (attr->attlen == -1)
		{
			n_value = PointerGetDatum(PG_DETOAST_DATUM(new_value));
			o_value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}

		if (!datumIsEqual(o_value, n_value, attr->attbyval, attr->attlen))
			ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					 errmsg("row identifier column update is not supported")));

		/* Free memory if it's a copy made above */
		if (DatumGetPointer(n_value) != DatumGetPointer(new_value))
			pfree(DatumGetPointer(n_value));
		if (DatumGetPointer(o_value) != DatumGetPointer(value))
			pfree(DatumGetPointer(o_value));
	}
	else if (!(DatumGetPointer(new_value) == NULL &&
			   DatumGetPointer(value) == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("row identifier column update is not supported")));

	ReleaseSysCache(tuple);

	/* Bind qual */
	mysql_bind_sql_var(typeoid, bindnum, value, mysql_bind_buffer, &is_null);

	if (mysql_stmt_bind_param(fmstate->stmt, mysql_bind_buffer) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("failed to bind the MySQL query: %s",
						mysql_error(fmstate->conn))));

	/* Execute the query */
	if (mysql_stmt_execute(fmstate->stmt) != 0)
		mysql_stmt_error_print(fmstate, "failed to execute the MySQL query");

	/* Return NULL if nothing was updated on the remote end */
	return slot;
}

/*
 * mysqlAddForeignUpdateTargets
 * 		Add column(s) needed for update/delete on a foreign table, we are
 * 		using first column as row identification column, so we are adding
 * 		that into target list.
 */
static void
mysqlAddForeignUpdateTargets(Query *parsetree,
							 RangeTblEntry *target_rte,
							 Relation target_relation)
{
	Var		   *var;
	const char *attrname;
	TargetEntry *tle;

	/*
	 * What we need is the rowid which is the first column
	 */
	Form_pg_attribute attr =
		TupleDescAttr(RelationGetDescr(target_relation), 0);

	/* Make a Var representing the desired value */
	var = makeVar(parsetree->resultRelation,
				  1,
				  attr->atttypid,
				  attr->atttypmod,
				  InvalidOid,
				  0);

	/* Wrap it in a TLE with the right name ... */
	attrname = NameStr(attr->attname);

	tle = makeTargetEntry((Expr *) var,
						  list_length(parsetree->targetList) + 1,
						  pstrdup(attrname), true);

	/* ... and add it to the query's targetlist */
	parsetree->targetList = lappend(parsetree->targetList, tle);
}

/*
 * mysqlExecForeignDelete
 * 		Delete one row from a foreign table
 */
static TupleTableSlot *
mysqlExecForeignDelete(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	MySQLFdwExecState *fmstate = (MySQLFdwExecState *) resultRelInfo->ri_FdwState;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	MYSQL_BIND *mysql_bind_buffer;
	Oid			foreignTableId = RelationGetRelid(rel);
	bool		is_null = false;
	Oid			typeoid;
	Datum		value;

	mysql_bind_buffer = (MYSQL_BIND *) palloc(sizeof(MYSQL_BIND));

	/* Get the id that was passed up as a resjunk column */
	value = ExecGetJunkAttribute(planSlot, 1, &is_null);
	typeoid = get_atttype(foreignTableId, 1);

	/* Bind qual */
	mysql_bind_sql_var(typeoid, 0, value, mysql_bind_buffer, &is_null);

	if (mysql_stmt_bind_param(fmstate->stmt, mysql_bind_buffer) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("failed to execute the MySQL query: %s",
						mysql_error(fmstate->conn))));

	/* Execute the query */
	if (mysql_stmt_execute(fmstate->stmt) != 0)
		mysql_stmt_error_print(fmstate, "failed to execute the MySQL query");

	/* Return NULL if nothing was updated on the remote end */
	return slot;
}

/*
 * mysqlEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
mysqlEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo)
{
	MySQLFdwExecState *festate = resultRelInfo->ri_FdwState;

	if (festate && festate->stmt)
	{
		mysql_stmt_close(festate->stmt);
		festate->stmt = NULL;
	}
}

/*
 * mysqlImportForeignSchema
 * 		Import a foreign schema (9.5+)
 */
#if PG_VERSION_NUM >= 90500
static List *
mysqlImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	List	   *commands = NIL;
	bool		import_default = false;
	bool		import_not_null = true;
	ForeignServer *server;
	UserMapping *user;
	mysql_opt  *options;
	MYSQL	   *conn;
	StringInfoData buf;
	MYSQL_RES  *volatile res = NULL;
	MYSQL_ROW	row;
	ListCell   *lc;

	/* Parse statement options */
	foreach(lc, stmt->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "import_default") == 0)
			import_default = defGetBoolean(def);
		else if (strcmp(def->defname, "import_not_null") == 0)
			import_not_null = defGetBoolean(def);
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname)));
	}

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	server = GetForeignServer(serverOid);
	user = GetUserMapping(GetUserId(), server->serverid);
	options = mysql_get_options(serverOid);
	conn = mysql_get_connection(server, user, options);

	/* Create workspace for strings */
	initStringInfo(&buf);

	/* Check that the schema really exists */
	appendStringInfo(&buf,
					 "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = '%s'",
					 stmt->remote_schema);

	if (mysql_query(conn, buf.data) != 0)
		mysql_error_print(conn);

	res = mysql_store_result(conn);
	if (!res || mysql_num_rows(res) < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_SCHEMA_NOT_FOUND),
				 errmsg("schema \"%s\" is not present on foreign server \"%s\"",
						stmt->remote_schema, server->servername)));

	mysql_free_result(res);
	res = NULL;
	resetStringInfo(&buf);

	/*
	 * Fetch all table data from this schema, possibly restricted by EXCEPT or
	 * LIMIT TO.
	 */
	appendStringInfo(&buf,
					 " SELECT"
					 "  t.TABLE_NAME,"
					 "  c.COLUMN_NAME,"
					 "  CASE"
					 "    WHEN c.DATA_TYPE = 'enum' THEN LOWER(CONCAT(t.TABLE_NAME, '_', c.COLUMN_NAME, '_t'))"
					 "    WHEN c.DATA_TYPE = 'tinyint' THEN 'smallint'"
					 "    WHEN c.DATA_TYPE = 'mediumint' THEN 'integer'"
					 "    WHEN c.DATA_TYPE = 'tinyint unsigned' THEN 'smallint'"
					 "    WHEN c.DATA_TYPE = 'smallint unsigned' THEN 'integer'"
					 "    WHEN c.DATA_TYPE = 'mediumint unsigned' THEN 'integer'"
					 "    WHEN c.DATA_TYPE = 'int unsigned' THEN 'bigint'"
					 "    WHEN c.DATA_TYPE = 'bigint unsigned' THEN 'numeric(20)'"
					 "    WHEN c.DATA_TYPE = 'double' THEN 'double precision'"
					 "    WHEN c.DATA_TYPE = 'float' THEN 'real'"
					 "    WHEN c.DATA_TYPE = 'datetime' THEN 'timestamp'"
					 "    WHEN c.DATA_TYPE = 'longtext' THEN 'text'"
					 "    WHEN c.DATA_TYPE = 'mediumtext' THEN 'text'"
					 "    WHEN c.DATA_TYPE = 'tinytext' THEN 'text'"
					 "    WHEN c.DATA_TYPE = 'blob' THEN 'bytea'"
					 "    WHEN c.DATA_TYPE = 'mediumblob' THEN 'bytea'"
					 "    WHEN c.DATA_TYPE = 'longblob' THEN 'bytea'"
					 "    ELSE c.DATA_TYPE"
					 "  END,"
					 "  c.COLUMN_TYPE,"
					 "  IF(c.IS_NULLABLE = 'NO', 't', 'f'),"
					 "  c.COLUMN_DEFAULT"
					 " FROM"
					 "  information_schema.TABLES AS t"
					 " JOIN"
					 "  information_schema.COLUMNS AS c"
					 " ON"
					 "  t.TABLE_CATALOG <=> c.TABLE_CATALOG AND t.TABLE_SCHEMA <=> c.TABLE_SCHEMA AND t.TABLE_NAME <=> c.TABLE_NAME"
					 " WHERE"
					 "  t.TABLE_SCHEMA = '%s'",
					 stmt->remote_schema);

	/* Apply restrictions for LIMIT TO and EXCEPT */
	if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO ||
		stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
	{
		bool		first_item = true;

		appendStringInfoString(&buf, " AND t.TABLE_NAME ");
		if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
			appendStringInfoString(&buf, "NOT ");
		appendStringInfoString(&buf, "IN (");

		/* Append list of table names within IN clause */
		foreach(lc, stmt->table_list)
		{
			RangeVar   *rv = (RangeVar *) lfirst(lc);

			if (first_item)
				first_item = false;
			else
				appendStringInfoString(&buf, ", ");

			appendStringInfo(&buf, "'%s'", rv->relname);
		}
		appendStringInfoChar(&buf, ')');
	}

	/* Append ORDER BY at the end of query to ensure output ordering */
	appendStringInfo(&buf, " ORDER BY t.TABLE_NAME, c.ORDINAL_POSITION");

	/* Fetch the data */
	if (mysql_query(conn, buf.data) != 0)
		mysql_error_print(conn);

	res = mysql_store_result(conn);
	row = mysql_fetch_row(res);
	while (row)
	{
		char	   *tablename = row[0];
		bool		first_item = true;

		resetStringInfo(&buf);
		appendStringInfo(&buf, "CREATE FOREIGN TABLE %s (\n",
						 quote_identifier(tablename));

		/* Scan all rows for this table */
		do
		{
			char	   *attname;
			char	   *typename;
			char	   *typedfn;
			char	   *attnotnull;
			char	   *attdefault;

			/* If table has no columns, we'll see nulls here */
			if (row[1] == NULL)
				continue;

			attname = row[1];
			typename = row[2];

			if (strcmp(typename, "char") == 0 || strcmp(typename, "varchar") == 0)
				typename = row[3];

			typedfn = row[3];
			attnotnull = row[4];
			attdefault = row[5] == NULL ? (char *) NULL : row[5];

			if (strncmp(typedfn, "enum(", 5) == 0)
				ereport(NOTICE,
						(errmsg("error while generating the table definition"),
						 errhint("If you encounter an error, you may need to execute the following first:\nDO $$BEGIN IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_type WHERE typname = '%s') THEN CREATE TYPE %s AS %s; END IF; END$$;\n",
								 typename, typename, typedfn)));

			if (first_item)
				first_item = false;
			else
				appendStringInfoString(&buf, ",\n");

			/* Print column name and type */
			appendStringInfo(&buf, "  %s %s", quote_identifier(attname),
							 typename);

			/* Add DEFAULT if needed */
			if (import_default && attdefault != NULL)
				appendStringInfo(&buf, " DEFAULT %s", attdefault);

			/* Add NOT NULL if needed */
			if (import_not_null && attnotnull[0] == 't')
				appendStringInfoString(&buf, " NOT NULL");
		}
		while ((row = mysql_fetch_row(res)) &&
			   (strcmp(row[0], tablename) == 0));

		/*
		 * Add server name and table-level options.  We specify remote
		 * database and table name as options (the latter to ensure that
		 * renaming the foreign table doesn't break the association).
		 */
		appendStringInfo(&buf,
						 "\n) SERVER %s OPTIONS (dbname '%s', table_name '%s');\n",
						 quote_identifier(server->servername),
						 stmt->remote_schema,
						 tablename);

		commands = lappend(commands, pstrdup(buf.data));
	}

	/* Clean up */
	mysql_free_result(res);
	res = NULL;
	resetStringInfo(&buf);

	mysql_release_connection(conn);

	return commands;
}
#endif

#if PG_VERSION_NUM >= 110000
/*
 * mysqlBeginForeignInsert
 * 		Prepare for an insert operation triggered by partition routing
 * 		or COPY FROM.
 *
 * This is not yet supported, so raise an error.
 */
static void
mysqlBeginForeignInsert(ModifyTableState *mtstate,
						ResultRelInfo *resultRelInfo)
{
	ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			 errmsg("COPY and foreign partition routing not supported in mysql_fdw")));
}

/*
 * mysqlEndForeignInsert
 * 		BeginForeignInsert() is not yet implemented, hence we do not
 * 		have anything to cleanup as of now. We throw an error here just
 * 		to make sure when we do that we do not forget to cleanup
 * 		resources.
 */
static void
mysqlEndForeignInsert(EState *estate, ResultRelInfo *resultRelInfo)
{
	ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			 errmsg("COPY and foreign partition routing not supported in mysql_fdw")));
}
#endif

/*
 * Prepare for processing of parameters used in remote query.
 */
static void
prepare_query_params(PlanState *node,
					 List *fdw_exprs,
					 int numParams,
					 FmgrInfo **param_flinfo,
					 List **param_exprs,
					 const char ***param_values,
					 Oid **param_types)
{
	int			i;
	ListCell   *lc;

	Assert(numParams > 0);

	/* Prepare for output conversion of parameters used in remote query. */
	*param_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * numParams);

	*param_types = (Oid *) palloc0(sizeof(Oid) * numParams);

	i = 0;
	foreach(lc, fdw_exprs)
	{
		Node	   *param_expr = (Node *) lfirst(lc);
		Oid			typefnoid;
		bool		isvarlena;

		(*param_types)[i] = exprType(param_expr);

		getTypeOutputInfo(exprType(param_expr), &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &(*param_flinfo)[i]);
		i++;
	}

	/*
	 * Prepare remote-parameter expressions for evaluation.  (Note: in
	 * practice, we expect that all these expressions will be just Params, so
	 * we could possibly do something more efficient than using the full
	 * expression-eval machinery for this.  But probably there would be little
	 * benefit, and it'd require postgres_fdw to know more than is desirable
	 * about Param evaluation.)
	 */
#if PG_VERSION_NUM >= 100000
	*param_exprs = ExecInitExprList(fdw_exprs, node);
#else
	*param_exprs = (List *) ExecInitExpr((Expr *) fdw_exprs, node);
#endif

	/* Allocate buffer for text form of query parameters. */
	*param_values = (const char **) palloc0(numParams * sizeof(char *));
}

/*
 * Construct array of query parameter values in text format.
 */
static void
process_query_params(ExprContext *econtext,
					 FmgrInfo *param_flinfo,
					 List *param_exprs,
					 const char **param_values,
					 MYSQL_BIND **mysql_bind_buf,
					 Oid *param_types)
{
	int			i;
	ListCell   *lc;

	i = 0;
	foreach(lc, param_exprs)
	{
		ExprState  *expr_state = (ExprState *) lfirst(lc);
		Datum		expr_value;
		bool		isNull;

		/* Evaluate the parameter expression */
#if PG_VERSION_NUM >= 100000
		expr_value = ExecEvalExpr(expr_state, econtext, &isNull);
#else
		expr_value = ExecEvalExpr(expr_state, econtext, &isNull, NULL);
#endif
		mysql_bind_sql_var(param_types[i], i, expr_value, *mysql_bind_buf,
						   &isNull);

		/*
		 * Get string representation of each parameter value by invoking
		 * type-specific output function, unless the value is null.
		 */
		if (isNull)
			param_values[i] = NULL;
		else
			param_values[i] = OutputFunctionCall(&param_flinfo[i], expr_value);
		i++;
	}
}

/*
 * Process the query params and bind the same with the statement, if any.
 * Also, execute the statement.
 */
static void
bind_stmt_params_and_exec(ForeignScanState *node)
{
	MySQLFdwExecState *festate = (MySQLFdwExecState *) node->fdw_state;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			numParams = festate->numParams;
	const char **values = festate->param_values;
	MYSQL_BIND *mysql_bind_buffer = NULL;

	/*
	 * Construct array of query parameter values in text format.  We do the
	 * conversions in the short-lived per-tuple context, so as not to cause a
	 * memory leak over repeated scans.
	 */
	if (numParams > 0)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		mysql_bind_buffer = (MYSQL_BIND *) palloc0(sizeof(MYSQL_BIND) * numParams);

		process_query_params(econtext,
							 festate->param_flinfo,
							 festate->param_exprs,
							 values,
							 &mysql_bind_buffer,
							 festate->param_types);

		mysql_stmt_bind_param(festate->stmt, mysql_bind_buffer);

		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Finally, execute the query. The result will be placed in the array we
	 * already bind.
	 */
	if (mysql_stmt_execute(festate->stmt) != 0)
	{
		mysql_stmt_error_print(festate, "failed to execute the MySQL query");
	}
	else
	{
		/*
		 * Finally execute the query and result will be placed in the array we
		 * already bind
		 */
		if (mysql_stmt_execute(festate->stmt) != 0)
		{
			mysql_stmt_error_print(festate, "failed to execute the MySQL query");
		}
		else
		{
			/* Check the results of query has warning or not */
			if(mysql_warning_count(festate->conn) > 0)
			{
				MYSQL_RES	*result = NULL;

				if (mysql_query(festate->conn, "SHOW WARNINGS"))
				{
					mysql_error_print(festate->conn);
				}
				result = mysql_store_result(festate->conn);
				if (result)
				{
					/*
					* MySQL provide numbers of rows per table invole in
					* the statment, but we don't have problem with it
					* because we are sending separate query per table
					* in FDW.
					*/
					MYSQL_ROW		row;
					unsigned int	num_fields;
					unsigned int	i;

					num_fields = mysql_num_fields(result);
					while ((row = mysql_fetch_row(result)))
					{
						for(i = 0; i < num_fields; i++)
						{
							/* Check warning of query */
							if (strcmp(row[i], "Division by 0") == 0)
								ereport(ERROR,
											(errcode(ERRCODE_DIVISION_BY_ZERO),
											errmsg("division by zero")));
						}
					}
					mysql_free_result(result);
				}
			}
		}
	}
	

	/* Mark the query as executed */
	festate->query_executed = true;
}

Datum
mysql_fdw_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(CODE_VERSION);
}

static void
mysql_error_print(MYSQL *conn)
{
	switch (mysql_errno(conn))
	{
		case CR_NO_ERROR:
			/* Should not happen, though give some message */
			elog(ERROR, "unexpected error code");
			break;
		case CR_OUT_OF_MEMORY:
		case CR_SERVER_GONE_ERROR:
		case CR_SERVER_LOST:
		case CR_UNKNOWN_ERROR:
			mysql_release_connection(conn);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					 errmsg("failed to execute the MySQL query: \n%s",
							mysql_error(conn))));
			break;
		case CR_COMMANDS_OUT_OF_SYNC:
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					 errmsg("failed to execute the MySQL query: \n%s",
							mysql_error(conn))));
	}
}

static void
mysql_stmt_error_print(MySQLFdwExecState *festate, const char *msg)
{
	switch (mysql_stmt_errno(festate->stmt))
	{
		case CR_NO_ERROR:
			/* Should not happen, though give some message */
			elog(ERROR, "unexpected error code");
			break;
		case CR_OUT_OF_MEMORY:
		case CR_SERVER_GONE_ERROR:
		case CR_SERVER_LOST:
		case CR_UNKNOWN_ERROR:
			mysql_release_connection(festate->conn);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					 errmsg("%s: \n%s", msg, mysql_error(festate->conn))));
			break;
		case CR_COMMANDS_OUT_OF_SYNC:
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					 errmsg("%s: \n%s", msg, mysql_error(festate->conn))));
			break;
	}
}

/*
 * getUpdateTargetAttrs
 * 		Returns the list of attribute numbers of the columns being updated.
 */
static List *
getUpdateTargetAttrs(RangeTblEntry *rte)
{
	List	   *targetAttrs = NIL;

#if PG_VERSION_NUM >= 90500
	Bitmapset  *tmpset = bms_copy(rte->updatedCols);
#else
	Bitmapset  *tmpset = bms_copy(rte->modifiedCols);
#endif
	AttrNumber	col;

	while ((col = bms_first_member(tmpset)) >= 0)
	{
		col += FirstLowInvalidHeapAttributeNumber;
		if (col <= InvalidAttrNumber)	/* shouldn't happen */
			elog(ERROR, "system-column update is not supported");

		/* We also disallow updates to the first column */
		if (col == 1)
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("row identifier column update is not supported")));

		targetAttrs = lappend_int(targetAttrs, col);
	}

	return targetAttrs;
}
