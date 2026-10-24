#include "db_backend.h"
#include <chrono>
#include <utility>

namespace {
std::string quoteIdentifier(const std::string& id) {
    std::string out = "\"";
    for (char c : id) {
        out += c;
        if (c == '"') out += '"';
    }
    out += '"';
    return out;
}

void appendRow(sqlite3_stmt* stmt, int colCount, QueryResult& res) {
    std::vector<std::string> row;
    row.reserve(colCount);
    for (int i = 0; i < colCount; ++i) {
        const int type = sqlite3_column_type(stmt, i);
        if (type == SQLITE_NULL) {
            row.push_back("<NULL>");
        } else if (type == SQLITE_BLOB) {
            row.push_back("<BLOB " + std::to_string(sqlite3_column_bytes(stmt, i)) + " bytes>");
        } else {
            const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row.push_back(txt ? txt : "");
        }
    }
    res.rows.push_back(std::move(row));
}
} // namespace

DatabaseManager::DatabaseManager() = default;

DatabaseManager::~DatabaseManager() {
    close();
}

bool DatabaseManager::open(const std::string& filepath) {
    close();
    m_filepath = filepath;

    int rc = sqlite3_open_v2(filepath.c_str(), &m_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        if (m_db) {
            m_lastError = sqlite3_errmsg(m_db);
            sqlite3_close(m_db);
            m_db = nullptr;
        } else {
            m_lastError = "Falha ao alocar memoria para conexao SQLite";
        }
        return false;
    }

    // Configura tempo limite para locks
    sqlite3_busy_timeout(m_db, 3000);

    return refreshSchema();
}

void DatabaseManager::close() {
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
    m_tables.clear();
    m_filepath.clear();
    m_lastError.clear();
}

bool DatabaseManager::refreshSchema() {
    if (!m_db) return false;

    m_tables.clear();
    m_lastError.clear();

    const char* listSql =
        "SELECT type, name FROM sqlite_master "
        "WHERE type IN ('table', 'view') AND name NOT LIKE 'sqlite_%' "
        "ORDER BY type, name;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, listSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    std::vector<std::pair<std::string, std::string>> tableNames;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* typeStr = (const char*)sqlite3_column_text(stmt, 0);
        const char* nameStr = (const char*)sqlite3_column_text(stmt, 1);
        tableNames.push_back({ typeStr ? typeStr : "", nameStr ? nameStr : "" });
    }
    sqlite3_finalize(stmt);

    for (const auto& item : tableNames) {
        TableInfo t;
        t.type = item.first;
        t.name = item.second;

        // Inspeciona colunas com PRAGMA table_info("nome")
        std::string pragmaSql = "PRAGMA table_info(" + quoteIdentifier(t.name) + ");";
        sqlite3_stmt* colStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, pragmaSql.c_str(), -1, &colStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(colStmt) == SQLITE_ROW) {
                ColumnInfo col;
                col.cid = sqlite3_column_int(colStmt, 0);
                const char* cName = (const char*)sqlite3_column_text(colStmt, 1);
                const char* cType = (const char*)sqlite3_column_text(colStmt, 2);
                col.name = cName ? cName : "";
                col.type = cType ? cType : "TEXT";
                col.notNull = sqlite3_column_int(colStmt, 3) != 0;
                const char* cDef = (const char*)sqlite3_column_text(colStmt, 4);
                col.defaultValue = cDef ? cDef : "";
                col.isPrimaryKey = sqlite3_column_int(colStmt, 5) != 0;
                t.columns.push_back(col);
            }
            sqlite3_finalize(colStmt);
        }

        updateRowCount(t);
        m_tables.push_back(t);
    }

    return true;
}

void DatabaseManager::updateRowCount(TableInfo& table) {
    if (!m_db) return;
    std::string countSql = "SELECT COUNT(*) FROM " + quoteIdentifier(table.name) + ";";
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, countSql.c_str(), -1, &countStmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            table.rowCount = sqlite3_column_int64(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
    }
}

const TableInfo* DatabaseManager::getTable(const std::string& tableName) const {
    for (const auto& t : m_tables) {
        if (t.name == tableName) return &t;
    }
    return nullptr;
}

QueryResult DatabaseManager::fetchTableData(const std::string& tableName,
                                           int offset,
                                           int limit,
                                           const std::string& sortColumn,
                                           bool sortAsc,
                                           const std::string& filterExpr) {
    QueryResult res;
    if (!m_db) {
        res.errorMessage = "None database of data open.";
        return res;
    }

    auto start = std::chrono::high_resolution_clock::now();

    // Monta query genérica segura
    std::string baseSql = "FROM " + quoteIdentifier(tableName);
    if (!filterExpr.empty()) baseSql += " WHERE " + filterExpr;

    // Pega contagem total com o filtro aplicado
    std::string countSql = "SELECT COUNT(*) " + baseSql + ";";
    sqlite3_stmt* cStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, countSql.c_str(), -1, &cStmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(cStmt) == SQLITE_ROW) {
            res.totalRows = sqlite3_column_int64(cStmt, 0);
        }
        sqlite3_finalize(cStmt);
    } else {
        res.errorMessage = "Erro no filtro/WHERE: " + std::string(sqlite3_errmsg(m_db));
        return res;
    }

    // Monta query de dados paginada
    std::string querySql = "SELECT * " + baseSql;
    if (!sortColumn.empty()) {
        querySql += " ORDER BY " + quoteIdentifier(sortColumn) + " " + (sortAsc ? "ASC" : "DESC");
    }
    querySql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset) + ";";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, querySql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        res.errorMessage = sqlite3_errmsg(m_db);
        return res;
    }

    int colCount = sqlite3_column_count(stmt);
    for (int i = 0; i < colCount; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        res.columnNames.push_back(name ? name : ("Col" + std::to_string(i)));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        appendRow(stmt, colCount, res);
    }
    sqlite3_finalize(stmt);

    auto end = std::chrono::high_resolution_clock::now();
    res.executionTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
    res.success = true;
    return res;
}

QueryResult DatabaseManager::executeQuery(const std::string& sql, int maxRows) {
    QueryResult res;
    if (!m_db) {
        res.errorMessage = "None database of data open.";
        return res;
    }

    auto start = std::chrono::high_resolution_clock::now();

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        res.errorMessage = sqlite3_errmsg(m_db);
        return res;
    }

    int colCount = sqlite3_column_count(stmt);
    for (int i = 0; i < colCount; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        res.columnNames.push_back(name ? name : ("Col" + std::to_string(i)));
    }

    int fetched = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (fetched < maxRows) appendRow(stmt, colCount, res);
        fetched++;
    }

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        res.errorMessage = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return res;
    }

    sqlite3_finalize(stmt);

    res.totalRows = fetched;
    auto end = std::chrono::high_resolution_clock::now();
    res.executionTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
    res.success = true;
    return res;
}
