#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sqlite3.h>

struct ColumnInfo {
    int cid = 0;
    std::string name;
    std::string type;
    bool notNull = false;
    std::string defaultValue;
    bool isPrimaryKey = false;
};

struct TableInfo {
    std::string name;
    std::string type; // "table" or "view"
    int64_t rowCount = 0;
    std::vector<ColumnInfo> columns;
};

struct QueryResult {
    bool success = false;
    std::string errorMessage;
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> rows;
    int64_t totalRows = 0;
    double executionTimeMs = 0.0;
};

class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    bool open(const std::string& filepath);
    void close();
    bool isOpen() const {
        return m_db != nullptr;
    }
    const std::string& getFilePath() const {
        return m_filepath;
    }
    const std::string& getLastError() const {
        return m_lastError;
    }

    // Schema inspection
    bool refreshSchema();
    const std::vector<TableInfo>& getTables() const {
        return m_tables;
    }
    const TableInfo* getTable(const std::string& tableName) const;

    // Generic generic data viewing
    QueryResult fetchTableData(const std::string& tableName,
                               int offset = 0,
                               int limit = 100,
                               const std::string& sortColumn = "",
                               bool sortAsc = true,
                               const std::string& filterExpr = "");

    // Freeform SQL Execution
    QueryResult executeQuery(const std::string& sql, int maxRows = 1000);

private:
    sqlite3* m_db = nullptr;
    std::string m_filepath;
    std::string m_lastError;
    std::vector<TableInfo> m_tables;

    void updateRowCount(TableInfo& table);
};
