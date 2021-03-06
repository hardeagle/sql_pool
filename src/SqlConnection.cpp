/*
 * Copyright (c) 2012-2016 Devin Smith <devin@devinsmith.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <algorithm> // std::replace
#include <cstring>
#include <stdexcept>

// FreeTDS stuff
#define MSDBLIB 1
#include <sqlfront.h>
#include <sybdb.h>

#include "SqlConnection.h"

namespace drs {

extern "C" int
sql_db_msg_handler(DBPROCESS * dbproc, DBINT msgno, int msgstate,
    int severity, char *msgtext, char *srvname, char *procname, int line)
{
  if (msgno == 5701 || msgno == 5703 || msgno == 5704)
    return 0;

  SqlConnection *conn = reinterpret_cast<SqlConnection *>(dbgetuserdata(dbproc));

  if (conn != nullptr) {
    return conn->MsgHandler(dbproc, msgno, msgstate, severity, msgtext, srvname,
        procname, line);
  }

  // No connection associated??
  return 0;
}

int SqlConnection::MsgHandler(DBPROCESS * dbproc, DBINT msgno, int msgstate,
    int severity, char *msgtext, char *srvname, char *procname, int line)
{
  /*
   * If the severity is something other than 0 or the msg number is
   * 0 (user informational messages).
   */
  _error.clear();

  if (severity >= 0 || msgno == 0) {
    /*
     * If the message was something other than informational, and
     * the severity was greater than 0, then print information to
     * stderr with a little pre-amble information.
     */
    if (msgno > 0 && severity > 0) {
      char *database;

      // Incorporate format?
      _error += "Msg ";
      _error += std::to_string(msgno);
      _error += ", Level ";
      _error += std::to_string(severity);
      _error += ", State ";
      _error += std::to_string(msgstate);
      _error += "\nServer '";
      _error += srvname;
      _error += "'";

      if (procname != NULL && *procname != '\0') {
        _error += ", Procedure '";
        _error += procname;
        _error += "'";
      }
      if (line > 0) {
        _error += ", Line ";
        _error += std::to_string(line);
      }
      _error += "\n";
      database = dbname(dbproc);
      if (database != NULL && *database != '\0') {
        _error += "Database '";
        _error += database;
        _error += "'\n";
      }
      _error += msgtext;
    } else {
      /*
       * Otherwise, it is just an informational (e.g. print) message
       * from the server, so send it to stdout.
       */
      _error += msgtext;
      severity = 0;
    }
  }

  if (msgno == 904) {
    /* Database cannot be autostarted during server shutdown or startup */
    _error += "Database does not exist, returning 0.\n";
    return 0;
  }

  if (msgno == 911) {
    /* Database does not exist */
    _error += "Database does not exist, returning 0.\n";
    return 0;
  }

  if (msgno == 952) {
    /* Database is in transition. */
    _error += "Database is in transition, returning 0.\n";
    return 0;
  }

  if (severity) {
    return 1;
  }
  return 0;
}

extern "C" int
sql_db_err_handler(DBPROCESS *dbproc, int severity, int dberr,
    int oserr, char *dberrstr, char *oserrstr)
{
  /*
   * For server messages, cancel the query and rely on the
   * message handler to spew the appropriate error messages out.
   */

  /* Can we trust that all 537 errors will go to the msg handler? Not sure. */
  if (dberr == SYBESMSG || dberr == 537 || dberr == SYBEVERDOWN)
    return INT_CANCEL;

  if (dberr == SYBEBBCI) {
    /* int batch = bcp_getbatchsize(dbproc);
     * log_msg(1, "%d rows sent to SQL Server.\n", sent += batch);
     */
    // Why cancel here?
    return INT_CANCEL;
  }

  return INT_CANCEL;
}

// FreeTDS DBLib requires some initialization.
void sql_startup()
{
  dbinit();

  dbmsghandle(sql_db_msg_handler);
  dberrhandle(sql_db_err_handler);

  dbsetlogintime(5);
}

void sql_shutdown()
{
  dbexit();
}

SqlConnection::~SqlConnection()
{
  Disconnect();
}

void SqlConnection::Connect()
{
  if (_dbHandle == nullptr || dbdead(_dbHandle)) {
    LOGINREC *login = dblogin();
    DBSETLAPP(login, "Microsoft");
    dbsetlversion(login, DBVERSION_72);
    DBSETLUSER(login, _user.c_str());
    DBSETLPWD(login, _pass.c_str());
    _dbHandle = tdsdbopen(login, fix_server(_server).c_str(), 1);
    dbloginfree(login);

    if (_dbHandle == nullptr || dbdead(_dbHandle))
      throw std::runtime_error("Failed to connect to SQL Server");

    // FreeTDS is so gross. Yep, instead of a void *, it's a BYTE * which
    // is an "unsigned char *"
    dbsetuserdata(_dbHandle, reinterpret_cast<BYTE *>(this));

    dbuse(_dbHandle, _database.c_str());
    run_initial_query();
  }
}

void SqlConnection::run_initial_query()
{
  // Heterogeneous queries require the ANSI_NULLS and ANSI_WARNINGS options to
  // be set for the connection.
  ExecDML("SET ANSI_NULLS ON;"
      "SET ANSI_NULL_DFLT_ON ON;"
      "SET ANSI_PADDING ON;"
      "SET ANSI_WARNINGS ON;"
      "SET QUOTED_IDENTIFIER ON;"
      "SET CONCAT_NULL_YIELDS_NULL ON;");
}

void SqlConnection::Disconnect()
{
  if (_dbHandle != nullptr) {
    dbclose(_dbHandle);
    _dbHandle = nullptr;
  }
}

void SqlConnection::Dispose()
{
  // Did we already fetch all result sets?
  if (_fetched_results)
    return;

  // If the current result set has rows, drain them.
  if (!_fetched_rows) {
    while (NextRow());
  }

  // While there are more results, drain those rows as well.
  while (NextResult()) {
    while (NextRow());
  }
  _fetched_results = true;
}

void SqlConnection::ExecDML(const char *sql)
{
  Connect();

  // Dispose of any previous result set (if any).
  Dispose();

  _fetched_rows = false;
  _fetched_results = false;
  if (dbcmd(_dbHandle, sql) == FAIL)
    throw std::runtime_error("Failed to submit command to freetds");

  if (dbsqlexec(_dbHandle) == FAIL)
    throw std::runtime_error("Failed to execute DML");

  Dispose();
}


bool SqlConnection::NextResult()
{
  // In order to advance to the next result set, we need to fetch
  // all rows.
  if (!_fetched_rows) {
    while (NextRow());
  }

  int res = dbresults(_dbHandle);
  if (res == FAIL)
    throw std::runtime_error("Failed to fetch next result");

  if (res == NO_MORE_RESULTS) {
    _fetched_results = true;
    return false;
  }

  return true;
}

bool SqlConnection::NextRow()
{
  int row_code;
  do {
    row_code = dbnextrow(_dbHandle);
    if (row_code == NO_MORE_ROWS) {
      _fetched_rows = true;
      return false;
    }

    if (row_code == REG_ROW)
      return true;

    if (row_code == FAIL) {
      // ERROR ??
      return false;
    }
  } while (row_code == BUF_FULL);

  return false;
}

void SqlConnection::ExecSql(const char *sql)
{
  Connect();

  // Dispose of any previous result set (if any).
  Dispose();

  _fetched_rows = false;
  _fetched_results = false;
  if (dbcmd(_dbHandle, sql) == FAIL)
    throw std::runtime_error("Failed to submit command to freetds");

  if (dbsqlexec(_dbHandle) == FAIL)
    throw std::runtime_error("Failed to execute SQL");

  int res = dbresults(_dbHandle);
  if (res == FAIL)
    throw std::runtime_error("Failed to fetch results SQL");

  if (res == NO_MORE_RESULTS)
    _fetched_results = true;
}

int
SqlConnection::GetOrdinal(const char *colName)
{
  int i;
  char errorStr[2048];
  int total_cols = dbnumcols(_dbHandle);

  for (i = 0; i < total_cols; i++) {
    if (strcmp(dbcolname(_dbHandle, i + 1), colName) == 0) {
      break;
    }
  }
  if (i == total_cols) {
    snprintf(errorStr, sizeof(errorStr),
        "Requested column '%s' but does not exist.", colName);
    throw std::runtime_error(errorStr);
  }

  return i;
}

std::string
SqlConnection::GetStringCol(int col)
{
  DBINT srclen;
  int coltype;

  if (col > dbnumcols(_dbHandle))
    throw std::runtime_error("Requested string on non existant column");

  coltype = dbcoltype(_dbHandle, col + 1);
  srclen = dbdatlen(_dbHandle, col + 1);

  if (coltype == SYBDATETIME) {
    DBDATETIME data;
    DBDATEREC output;
    char date_string[64];

    memcpy(&data, dbdata(_dbHandle, col + 1), srclen);
    dbdatecrack(_dbHandle, &output, &data);
    snprintf(date_string, sizeof(date_string),
        "%04d-%02d-%02d %02d:%02d:%02d.%03d", output.year, output.month,
        output.day, output.hour, output.minute, output.second,
        output.millisecond);

    return date_string;
  } else if (coltype != SYBCHAR && coltype != SYBTEXT) {
    char nonstr[4096];
    int dest_size;

    dest_size = dbconvert(_dbHandle, coltype, dbdata(_dbHandle, col + 1),
        srclen, SYBCHAR, (BYTE *)nonstr, sizeof(nonstr));
    if (dest_size == -1) {
      throw std::runtime_error("Could not convert source to string.");
    }
    nonstr[dest_size] = '\0';
    return nonstr;
  }
  return std::string((const char *)dbdata(_dbHandle, col + 1), srclen);
}

std::string
SqlConnection::GetStringColByName(const char *colName)
{
  return GetStringCol(GetOrdinal(colName));
}

int
SqlConnection::GetInt32ColByName(const char *colName)
{
  return GetInt32Col(GetOrdinal(colName));
}

int
SqlConnection::GetInt32Col(int col)
{
  int ret;
  DBINT srclen;
  int coltype;

  if (col > dbnumcols(_dbHandle))
    return 0;

  srclen = dbdatlen(_dbHandle, col + 1);
  coltype = dbcoltype(_dbHandle, col + 1);

  if (coltype == SYBINT4) {
    memcpy(&ret, dbdata(_dbHandle, col + 1), srclen);
  } else if (coltype == SYBNUMERIC) {
    dbconvert(NULL, SYBNUMERIC, (BYTE *)dbdata(_dbHandle, col + 1), -1,
      SYBINT4, (BYTE *)&ret, 4);
  } else {
    dbconvert(NULL, coltype, (BYTE *)dbdata(_dbHandle, col + 1), -1,
      SYBINT4, (BYTE *)&ret, 4);
  }

  return ret;
}

int
SqlConnection::GetMoneyCol(int col, int *dol, int *cen)
{
  int64_t t64;
  BYTE *src;

  if (col > dbnumcols(_dbHandle))
    return 0;

  src = dbdata(_dbHandle, col + 1);
  t64 = (int64_t)src[4] | (int64_t)src[5] << 8 |
      (int64_t)src[6] << 16 | (int64_t)src[7] << 24 |
      (int64_t)src[0] << 32 | (int64_t)src[1] << 40 |
      (int64_t)src[2] << 48 | (int64_t)src[3] << 56;

  *dol = (int)(t64 / 10000);
  if (t64 < 0) t64 = -t64;
  *cen = (int)(t64 % 10000);

  return 1;
}

bool
SqlConnection::IsNullCol(int col)
{
  DBINT srclen;

  if (col > dbnumcols(_dbHandle))
    return true;

  srclen = dbdatlen(_dbHandle, col + 1);
  return srclen <= 0;
}

void SqlConnection::ExecStoredProc(const char *proc, struct db_params *params,
    size_t parm_count)
{
  int res, i;

  Connect();

  Dispose();

  _fetched_rows = false;
  _fetched_results = false;

  if (dbrpcinit(_dbHandle, proc, 0) == FAIL)
    throw std::runtime_error("Failed to init stored procedure");

  for (i = 0; i < parm_count; i++) {
    int real_type = 0;

    switch (params[i].type) {
    case BIT_TYPE:
      real_type = SYBBITN;
    case INT32_TYPE:
      real_type = SYBINT4;
      break;
    case STRING_TYPE:
      real_type = SYBCHAR;
      break;
    default:
      throw std::runtime_error("Unknown stored procedure parameter type");
    }
    if (dbrpcparam(_dbHandle, params[i].name, params[i].status,
        real_type, params[i].maxlen, params[i].datalen,
        (BYTE *)params[i].value) == FAIL) {
      throw std::runtime_error("Failed to set parameter");
    }
  }

  if (dbrpcsend(_dbHandle) == FAIL)
    throw std::runtime_error("Failed to send RPC");

  /* Wait for the server to return */
  if (dbsqlok(_dbHandle) == FAIL)
    throw std::runtime_error(_error);

  res = dbresults(_dbHandle);
  if (res == FAIL)
    throw std::runtime_error("Failed to get results");

  if (res == NO_MORE_RESULTS)
    _fetched_results = true;
}

void SqlConnection::ExecNonQuery(const char *proc, struct db_params *params,
    size_t parm_count)
{
  int i;

  Connect();

  Dispose();

  _fetched_rows = false;
  _fetched_results = false;

  if (dbrpcinit(_dbHandle, proc, 0) == FAIL)
    throw std::runtime_error("Failed to init stored procedure");

  for (i = 0; i < parm_count; i++) {
    int real_type = 0;

    switch (params[i].type) {
    case BIT_TYPE:
      real_type = SYBBITN;
    case INT32_TYPE:
      real_type = SYBINT4;
      break;
    case STRING_TYPE:
      real_type = SYBCHAR;
      break;
    default:
      throw std::runtime_error("Unknown stored procedure parameter type");
    }
    if (dbrpcparam(_dbHandle, params[i].name, params[i].status,
        real_type, params[i].maxlen, params[i].datalen,
        (BYTE *)params[i].value) == FAIL) {
      throw std::runtime_error("Failed to set parameter");
    }
  }

  if (dbrpcsend(_dbHandle) == FAIL)
    throw std::runtime_error("Failed to send RPC");

  /* Wait for the server to return */
  if (dbsqlok(_dbHandle) == FAIL)
    throw std::runtime_error("Failed to get SQL");

  Dispose();
}


std::string SqlConnection::fix_server(const std::string& str)
{
  std::string clean_server = str;

  // A server contains a host and port, but users may be providing
  // a server string with properties that aren't supported by FreeTDS.

  // FreeTDS doesn't need the leading "tcp:" in some connection strings.
  // Remove it if it exists.
  std::string tcp_prefix("tcp:");
  if (clean_server.compare(0, tcp_prefix.size(), tcp_prefix) == 0)
    clean_server = clean_server.substr(tcp_prefix.size());

  // Some people use commas instead of colon to seperate the port number.
  std::replace(clean_server.begin(), clean_server.end(), ',', ':');

  return clean_server;
}


}
