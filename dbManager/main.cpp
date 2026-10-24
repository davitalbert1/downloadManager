#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "db_backend.h"
#include "file_dialog.h"

#include <d3d11.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

// DirectX 11 Globals
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static HWND                     g_hWnd = nullptr;

// Drag & Drop transfer
static std::string              g_pendingDroppedFile;

// D3D11 Helpers
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Application State
struct AppUIState {
    DatabaseManager db;
    std::string currentDbPath;
    std::string selectedTable;
    int selectedTableIndex = -1;

    // View Options
    int activeTab = 0; // 0: Tabela/Dados, 1: Estrutura/Esquema, 2: Consulta SQL Livre, 3: Info do DB

    // Pagination & Filter for current table
    int currentPage = 0;
    int rowsPerPage = 50;
    char filterBuffer[512] = "";
    std::string currentSortCol = "";
    bool currentSortAsc = true;
    QueryResult tableData;
    bool needsReloadTable = false;

    // Freeform SQL Editor
    char sqlQueryBuffer[4096] = "SELECT * FROM sqlite_master LIMIT 50;";
    QueryResult customQueryResult;
    int maxQueryRows = 500;

    // Status / Messages
    std::string statusMessage;
    bool statusIsError = false;

    void loadTable(const std::string& tblName) {
        selectedTable = tblName;
        currentPage = 0;
        currentSortCol = "";
        currentSortAsc = true;
        filterBuffer[0] = '\0';
        refreshCurrentTable();
    }

    void refreshCurrentTable() {
        if (selectedTable.empty() || !db.isOpen()) {
            tableData = QueryResult();
            return;
        }
        int offset = currentPage * rowsPerPage;
        tableData = db.fetchTableData(selectedTable, offset, rowsPerPage, currentSortCol, currentSortAsc, filterBuffer);
        if (!tableData.success) {
            statusMessage = "Erro ao carregar tabela: " + tableData.errorMessage;
            statusIsError = true;
        } else {
            statusMessage = "Carregados " + std::to_string(tableData.rows.size()) + " registros em " +
                            std::to_string(tableData.executionTimeMs) + " ms";
            statusIsError = false;
        }
    }

    void openDatabase(const std::string& path) {
        if (path.empty()) return;
        if (db.open(path)) {
            currentDbPath = path;
            statusMessage = "Banco de dados aberto com sucesso: " + path;
            statusIsError = false;
            const auto& tables = db.getTables();
            if (!tables.empty()) {
                selectedTableIndex = 0;
                loadTable(tables[0].name);
            } else {
                selectedTable = "";
                selectedTableIndex = -1;
                tableData = QueryResult();
            }
        } else {
            statusMessage = "Erro ao abrir banco de dados: " + db.getLastError();
            statusIsError = true;
        }
    }
};

static AppUIState g_app;

// UI Rendering Functions
static void RenderTopBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Arquivo")) {
            if (ImGui::MenuItem("Abrir Banco de Dados (.db, .sqlite)...", "Ctrl+O")) {
                std::string path = OpenFileDialog(g_hWnd);
                if (!path.empty()) g_app.openDatabase(path);
            }
            if (ImGui::MenuItem("Recarregar Dados", "F5", false, g_app.db.isOpen())) {
                g_app.db.refreshSchema();
                g_app.refreshCurrentTable();
            }
            if (ImGui::MenuItem("Fechar Banco de Dados", nullptr, false, g_app.db.isOpen())) {
                g_app.db.close();
                g_app.currentDbPath.clear();
                g_app.selectedTable.clear();
                g_app.selectedTableIndex = -1;
                g_app.tableData = QueryResult();
                g_app.statusMessage = "Banco de dados fechado.";
                g_app.statusIsError = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Sair", "Alt+F4")) ::PostMessage(g_hWnd, WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Ajuda")) {
            if (ImGui::MenuItem("Sobre o dbManager")) ImGui::OpenPopup("AboutPopup");
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

static void RenderTableDataView() {
    if (g_app.selectedTable.empty()) {
        ImGui::TextDisabled("Selecione uma tabela ou view a esquerda para ver os dados.");
        return;
    }

    const TableInfo* tbl = g_app.db.getTable(g_app.selectedTable);
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Tabela: %s", g_app.selectedTable.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(Tipo: %s | Total aprox: %lld linhas)", tbl ? tbl->type.c_str() : "table", (long long)(tbl ? tbl->rowCount : 0));

    ImGui::Spacing();

    // Filtro WHERE e Ações
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Filtro (WHERE):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 280);
    bool filterSubmitted = ImGui::InputTextWithHint("##filter", "Ex: id > 100 AND nome LIKE '%teste%'", g_app.filterBuffer, sizeof(g_app.filterBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Filtrar") || filterSubmitted) {
        g_app.currentPage = 0;
        g_app.refreshCurrentTable();
    }
    ImGui::SameLine();
    if (ImGui::Button("Limpar")) {
        g_app.filterBuffer[0] = '\0';
        g_app.currentPage = 0;
        g_app.refreshCurrentTable();
    }
    ImGui::SameLine();
    if (ImGui::Button("Atualizar")) g_app.refreshCurrentTable();

    ImGui::Separator();

    // Barra de Paginação
    int totalRows = (int)g_app.tableData.totalRows;
    int maxPages = (totalRows + g_app.rowsPerPage - 1) / g_app.rowsPerPage;
    if (maxPages < 1) maxPages = 1;

    ImGui::BeginDisabled(g_app.currentPage <= 0);
    if (ImGui::Button("<< Primeiro")) {
        g_app.currentPage = 0;
        g_app.refreshCurrentTable();
    }
    ImGui::SameLine();
    if (ImGui::Button("< Anterior")) {
        g_app.currentPage--;
        g_app.refreshCurrentTable();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Text("Pagina %d de %d (Total: %d registros)", g_app.currentPage + 1, maxPages, totalRows);

    ImGui::SameLine();
    ImGui::BeginDisabled(g_app.currentPage >= maxPages - 1);
    if (ImGui::Button("Proximo >")) {
        g_app.currentPage++;
        g_app.refreshCurrentTable();
    }
    ImGui::SameLine();
    if (ImGui::Button("Ultimo >>")) {
        g_app.currentPage = maxPages - 1;
        g_app.refreshCurrentTable();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* rowLimits[] = { "25", "50", "100", "200", "500" };
    static int currentLimitIdx = 1; // 50
    if (ImGui::Combo("Por pag.", &currentLimitIdx, rowLimits, IM_ARRAYSIZE(rowLimits))) {
        g_app.rowsPerPage = std::atoi(rowLimits[currentLimitIdx]);
        g_app.currentPage = 0;
        g_app.refreshCurrentTable();
    }

    ImGui::Spacing();

    // Tabela de Dados Genérica
    if (!g_app.tableData.success) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Erro na consulta: %s", g_app.tableData.errorMessage.c_str());
        return;
    }

    const auto& cols = g_app.tableData.columnNames;
    const auto& rows = g_app.tableData.rows;

    if (cols.empty()) {
        ImGui::TextDisabled("Nenhuma coluna encontrada.");
        return;
    }

    ImVec2 outer_size = ImVec2(0.0f, ImGui::GetContentRegionAvail().y - 28.0f);
    ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                 ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_Reorderable |
                                 ImGuiTableFlags_ScrollX |
                                 ImGuiTableFlags_ScrollY |
                                 ImGuiTableFlags_Sortable;

    if (ImGui::BeginTable("GenericTableViewer", (int)cols.size() + 1, table_flags, outer_size)) {
        // Coluna fixa de índice #
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 45.0f);

        for (size_t i = 0; i < cols.size(); ++i) {
            ImGuiTableColumnFlags cflags = ImGuiTableColumnFlags_None;
            if (g_app.currentSortCol == cols[i]) {
                cflags |= ImGuiTableColumnFlags_PreferSortAscending;
            }
            ImGui::TableSetupColumn(cols[i].c_str(), cflags);
        }
        ImGui::TableHeadersRow();

        // Checagem de ordenação clicada no header
        ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs();
        if (sorts_specs && sorts_specs->SpecsDirty) {
            if (sorts_specs->SpecsCount > 0) {
                int colIdx = sorts_specs->Specs[0].ColumnIndex - 1; // desconta a coluna '#'
                if (colIdx >= 0 && colIdx < (int)cols.size()) {
                    g_app.currentSortCol = cols[colIdx];
                    g_app.currentSortAsc = (sorts_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                    g_app.currentPage = 0;
                    g_app.needsReloadTable = true;
                }
            }
            sorts_specs->SpecsDirty = false;
        }

        int rowOffset = g_app.currentPage * g_app.rowsPerPage;
        for (size_t r = 0; r < rows.size(); ++r) {
            ImGui::TableNextRow();
            // Coluna de índice
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", (int)(rowOffset + r + 1));

            // Colunas de dados
            for (size_t c = 0; c < cols.size(); ++c) {
                ImGui::TableSetColumnIndex((int)c + 1);
                if (c < rows[r].size()) {
                    const std::string& val = rows[r][c];
                    ImGui::PushID((int)(r * 10000 + c));
                    if (val == "<NULL>") {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 0.7f));
                    } else if (val.rfind("<BLOB", 0) == 0) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
                    }
                    if (ImGui::Selectable(val.empty() ? " " : val.c_str(), false)) {
                    }
                    ImGui::PopStyleColor();

                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Copiar Valor")) ImGui::SetClipboardText(val.c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndTable();
    }
}

static void RenderSchemaView() {
    if (g_app.selectedTable.empty()) {
        ImGui::TextDisabled("Selecione uma tabela para inspecionar o esquema de colunas.");
        return;
    }

    const TableInfo* tbl = g_app.db.getTable(g_app.selectedTable);
    if (!tbl) return;

    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Esquema da Tabela: %s", tbl->name.c_str());
    ImGui::TextDisabled("Total de Colunas: %d | Total de Registros: %lld", (int)tbl->columns.size(), (long long)tbl->rowCount);
    ImGui::Spacing();

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("SchemaTable", 6, flags)) {
        ImGui::TableSetupColumn("CID", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Nome da Coluna", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Tipo", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Not Null?", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Padrao", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Chave Primaria?", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (const auto& col : tbl->columns) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", col.cid);

            ImGui::TableSetColumnIndex(1);
            if (col.isPrimaryKey) {
                ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "* %s", col.name.c_str());
            } else {
                ImGui::TextUnformatted(col.name.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.6f, 1.0f), "%s", col.type.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(col.notNull ? "SIM" : "NAO");

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(col.defaultValue.empty() ? "-" : col.defaultValue.c_str());

            ImGui::TableSetColumnIndex(5);
            if (col.isPrimaryKey) {
                ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "PK (PRIMARIA)");
            } else {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
}

static void RenderSqlConsole() {
    ImGui::Text("Execute qualquer declaracao SQL (SELECT, PRAGMA, EXPLAIN, etc):");
    ImGui::InputTextMultiline("##sqlEditor", g_app.sqlQueryBuffer, sizeof(g_app.sqlQueryBuffer),
                             ImVec2(-1.0f, 120.0f), ImGuiInputTextFlags_AllowTabInput);

    if (ImGui::Button("Executar Consulta SQL (Ctrl+Enter)") ||
        (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        g_app.customQueryResult = g_app.db.executeQuery(g_app.sqlQueryBuffer, g_app.maxQueryRows);
        if (g_app.customQueryResult.success) {
            g_app.statusMessage = "Consulta executada: " + std::to_string(g_app.customQueryResult.rows.size()) +
                                  " registros retornados em " + std::to_string(g_app.customQueryResult.executionTimeMs) + " ms";
            g_app.statusIsError = false;
        } else {
            g_app.statusMessage = "Erro SQL: " + g_app.customQueryResult.errorMessage;
            g_app.statusIsError = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Limpar SQL")) g_app.sqlQueryBuffer[0] = '\0';

    ImGui::SameLine();
    ImGui::TextDisabled("| Limite maximo:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##maxRows", &g_app.maxQueryRows);
    if (g_app.maxQueryRows < 1) g_app.maxQueryRows = 1;

    ImGui::Separator();

    if (!g_app.customQueryResult.errorMessage.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Erro: %s", g_app.customQueryResult.errorMessage.c_str());
    } else if (g_app.customQueryResult.success) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Sucesso: %d registros exibidos (Tempo: %.2f ms)",
                           (int)g_app.customQueryResult.rows.size(), g_app.customQueryResult.executionTimeMs);

        const auto& cols = g_app.customQueryResult.columnNames;
        const auto& rows = g_app.customQueryResult.rows;

        if (!cols.empty()) {
            ImVec2 outer_size = ImVec2(0.0f, ImGui::GetContentRegionAvail().y - 28.0f);
            ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                         ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_Resizable |
                                         ImGuiTableFlags_ScrollX |
                                         ImGuiTableFlags_ScrollY;

            if (ImGui::BeginTable("CustomQueryResultTable", (int)cols.size() + 1, table_flags, outer_size)) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                for (size_t i = 0; i < cols.size(); ++i) {
                    ImGui::TableSetupColumn(cols[i].c_str());
                }
                ImGui::TableHeadersRow();

                for (size_t r = 0; r < rows.size(); ++r) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%d", (int)(r + 1));

                    for (size_t c = 0; c < cols.size(); ++c) {
                        ImGui::TableSetColumnIndex((int)c + 1);
                        if (c < rows[r].size()) {
                            const std::string& val = rows[r][c];
                            ImGui::PushID((int)(r * 10000 + c));
                            if (val == "<NULL>") {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 0.7f));
                            } else if (val.rfind("<BLOB", 0) == 0) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));
                            } else {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
                            }
                            if (ImGui::Selectable(val.empty() ? " " : val.c_str(), false)) {
                            }
                            ImGui::PopStyleColor();

                            if (ImGui::BeginPopupContextItem()) {
                                if (ImGui::MenuItem("Copiar Valor")) {
                                    ImGui::SetClipboardText(val.c_str());
                                }
                                ImGui::EndPopup();
                            }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndTable();
            }
        }
    }
}

static void RenderDatabaseInfoView() {
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Informacoes do Banco de Dados");
    ImGui::Separator();
    ImGui::Text("Arquivo: %s", g_app.currentDbPath.c_str());
    ImGui::Text("Versao SQLite da Engine: %s", sqlite3_libversion());

    QueryResult pageCountRes = g_app.db.executeQuery("PRAGMA page_count;");
    QueryResult pageSizeRes = g_app.db.executeQuery("PRAGMA page_size;");
    QueryResult encodingRes = g_app.db.executeQuery("PRAGMA encoding;");

    int64_t pageCount = 0;
    int64_t pageSize = 0;
    if (!pageCountRes.rows.empty() && !pageCountRes.rows[0].empty()) {
        pageCount = std::atoll(pageCountRes.rows[0][0].c_str());
    }
    if (!pageSizeRes.rows.empty() && !pageSizeRes.rows[0].empty()) {
        pageSize = std::atoll(pageSizeRes.rows[0][0].c_str());
    }

    double sizeMb = (double)(pageCount * pageSize) / (1024.0 * 1024.0);
    ImGui::Text("Tamanho Estimado: %.2f MB (%lld bytes)", sizeMb, (long long)(pageCount * pageSize));
    if (!encodingRes.rows.empty() && !encodingRes.rows[0].empty()) {
        ImGui::Text("Codificacao de Caracteres: %s", encodingRes.rows[0][0].c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Estatisticas de Objetos:");
    int tblCount = 0, viewCount = 0;
    for (const auto& t : g_app.db.getTables()) {
        if (t.type == "view") viewCount++;
        else tblCount++;
    }
    ImGui::BulletText("Tabelas Normais: %d", tblCount);
    ImGui::BulletText("Views: %d", viewCount);
}

static void RenderMainWindow() {
    // Process Drag & Drop
    if (!g_pendingDroppedFile.empty()) {
        std::string dropped = g_pendingDroppedFile;
        g_pendingDroppedFile.clear();
        g_app.openDatabase(dropped);
    }

    if (g_app.needsReloadTable) {
        g_app.needsReloadTable = false;
        g_app.refreshCurrentTable();
    }

    RenderTopBar();

    // Central Dockspace / Full Window layout
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y));
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::Begin("MainWindow", nullptr, window_flags);
    ImGui::PopStyleVar();

    if (!g_app.db.isOpen()) {
        // Welcome / Open DB prompt screen
        ImVec2 center = ImVec2(viewport->WorkSize.x * 0.5f, viewport->WorkSize.y * 0.5f);
        ImGui::SetCursorPos(ImVec2(std::max(20.0f, center.x - 280.0f), std::max(20.0f, center.y - 120.0f)));
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Gerenciador Visual de Banco de Dados SQLite (dbManager)");
        ImGui::Spacing();
        ImGui::Text("Nenhum banco de dados carregado no momento.");
        ImGui::Text("Voce pode:");
        ImGui::BulletText("Clicar no botao abaixo para escolher um arquivo .db via janela do Windows");
        ImGui::BulletText("OU simplesmente ARRASTAR e SOLTAR (Drag & Drop) qualquer arquivo .db nesta janela!");
        ImGui::Spacing();

        if (ImGui::Button("Escolher Arquivo .db no Computador...", ImVec2(320, 36))) {
            std::string path = OpenFileDialog(g_hWnd);
            if (!path.empty()) g_app.openDatabase(path);
        }
        ImGui::EndGroup();
    } else {
        // Layout: Split 2 colunas: Lista de Tabelas (Esquerda) e Conteúdo (Direita)
        float sidebarWidth = 260.0f;
        float contentHeight = ImGui::GetContentRegionAvail().y - 30.0f;

        // Painel Esquerdo: Tabelas
        ImGui::BeginChild("TablesSidebar", ImVec2(sidebarWidth, contentHeight), true);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "TABELAS & VIEWS (%d)", (int)g_app.db.getTables().size());
        ImGui::Separator();

        static char searchTable[128] = "";
        ImGui::InputTextWithHint("##searchtbl", "Buscar tabela...", searchTable, sizeof(searchTable));
        ImGui::Spacing();

        const auto& tables = g_app.db.getTables();
        for (int i = 0; i < (int)tables.size(); ++i) {
            const auto& t = tables[i];
            if (searchTable[0] != '\0') {
                std::string s1 = t.name;
                std::string s2 = searchTable;
                std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower);
                std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
                if (s1.find(s2) == std::string::npos) continue;
            }

            char label[256];
            snprintf(label, sizeof(label), "%s %s (%lld)",
                     (t.type == "view" ? "[V]" : "[T]"),
                     t.name.c_str(),
                     (long long)t.rowCount);

            bool isSelected = (g_app.selectedTable == t.name);
            if (ImGui::Selectable(label, isSelected)) {
                g_app.selectedTableIndex = i;
                g_app.loadTable(t.name);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Painel Direito: Abas de Visualização
        ImGui::BeginChild("MainContentArea", ImVec2(0, contentHeight), false);
        if (ImGui::BeginTabBar("DatabaseTabs")) {
            if (ImGui::BeginTabItem("Visualizador de Dados")) {
                RenderTableDataView();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Estrutura / Esquema")) {
                RenderSchemaView();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Consulta SQL Livre")) {
                RenderSqlConsole();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Informacoes do BD")) {
                RenderDatabaseInfoView();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
    }

    // Barra de Status Inferior
    ImGui::Separator();
    if (g_app.statusIsError) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[!] %s", g_app.statusMessage.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "[OK] %s", g_app.statusMessage.c_str());
    }
    if (g_app.db.isOpen()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 350);
        ImGui::TextDisabled("BD: %s", g_app.currentDbPath.c_str());
    }

    // Modal popup About
    if (ImGui::BeginPopupModal("AboutPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "dbManager - SQLite Visualizer");
        ImGui::Separator();
        ImGui::Text("Aplicacao 100%% Generica em C++ e Dear ImGui.");
        ImGui::BulletText("Abertura via seletor nativo Windows ou Drag & Drop.");
        ImGui::BulletText("Inspecao automatica de tabelas, colunas, tipos e chaves.");
        ImGui::BulletText("Paginacao rapida, ordenacao por coluna e filtros WHERE.");
        ImGui::BulletText("Console de consultas SQL livres em tempo real.");
        ImGui::Spacing();
        if (ImGui::Button("Fechar", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

int main(int argc, char** argv) {
    // DPI Awareness
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    // Create application window
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"dbManagerWindowClass", nullptr
    };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"dbManager - Visualizador Generico de Bancos de Dados SQLite",
        WS_OVERLAPPEDWINDOW,
        100, 100,
        (int)(1280 * main_scale),
        (int)(800 * main_scale),
        nullptr, nullptr, wc.hInstance, nullptr
    );
    g_hWnd = hwnd;

    // Habilita Drag and Drop no Windows para a janela
    ::DragAcceptFiles(hwnd, TRUE);

    // Inicializa Direct3D 11
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Tema moderno Dark
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    // Ajuste de cores para tema visual elegante
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.32f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.36f, 0.48f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.40f, 0.58f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.36f, 0.48f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.48f, 0.65f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.32f, 0.38f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.36f, 0.48f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.32f, 0.44f, 1.00f);

    // Setup Backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Carrega fonte se disponível ou usa fonte padrão escalada
    io.Fonts->AddFontDefault();

    // Se passou argumento de linha de comando com arquivo .db, já abre direto
    if (argc > 1 && argv[1] != nullptr) {
        g_app.openDatabase(argv[1]);
    }

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Resize buffer se necessário
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Novo frame ImGui
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Renderiza Aplicação
        RenderMainWindow();

        // Renderização Direct3D 11
        ImGui::Render();
        const float clear_color[4] = { 0.08f, 0.09f, 0.11f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0); // VSync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Shutdown
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helpers D3D11
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
        );
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        char filePath[MAX_PATH] = "";
        if (DragQueryFileA(hDrop, 0, filePath, MAX_PATH)) {
            g_pendingDroppedFile = filePath;
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
