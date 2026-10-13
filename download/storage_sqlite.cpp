#include "storage_sqlite.h"
#include "utils.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

void executar_sql(sqlite3* conn, const char* sql) {
    char* erro = nullptr;
    if (sqlite3_exec(conn, sql, nullptr, nullptr, &erro) != SQLITE_OK) {
        std::string msg = erro ? erro : "erro desconhecido";
        sqlite3_free(erro);
        throw std::runtime_error("SQLite: " + msg);
    }
}

void executar_sql(sqlite3* conn, const std::string& sql) {
    executar_sql(conn, sql.c_str());
}

sqlite3* get_connection_para(const std::string& db_path) {
    static thread_local std::map<std::string, sqlite3*> conns;
    auto it = conns.find(db_path);
    if (it != conns.end() && it->second) return it->second;
    // Cria a pasta-pai especificada no .ini (ex: "dados/pokemon.db" -> "dados/").
    // Sem isso o sqlite3_open falha com "unable to open database file".
    garantir_pasta_pai(db_path);
    sqlite3* conn = nullptr;
    if (sqlite3_open(db_path.c_str(), &conn) != SQLITE_OK) {
        std::string msg = conn ? sqlite3_errmsg(conn) : "falha ao abrir";
        if (conn) sqlite3_close(conn);
        throw std::runtime_error("SQLite: nao foi possivel abrir " + db_path + ": " + msg);
    }
    executar_sql(conn, "PRAGMA journal_mode=WAL");
    executar_sql(conn, "PRAGMA synchronous=NORMAL");
    executar_sql(conn, "PRAGMA busy_timeout=60000");
    conns[db_path] = conn;
    return conn;
}

std::vector<std::string> colunas_chave(const DatasetCfg& ds) {
    if (!ds.chave_unica.empty()) {
        std::vector<std::string> out;
        for (auto& c : dividir(ds.chave_unica, ',')) {
            if (c.empty()) continue;
            out.push_back(normalizar_nome(c));
        }
        return out;
    }
    // Se o dataset não iterar datas (ex: PokeAPI records), tenta inferir chave primária lógica
    // como 'id', 'name' ou 'nome', se disponível, para permitir UPSERT e evitar duplicação
    if (!ds.iterar_datas) return {};
    std::vector<std::string> chave;
    if (!ds.chave_data.empty()) chave.push_back(ds.chave_data);
    if (ds.salvar_hora && !ds.chave_hora.empty() && ds.chave_hora != ds.chave_data)
        chave.push_back(ds.chave_hora);
    if (ds.usar_coordenadas) {
        chave.push_back("latitude");
        chave.push_back("longitude");
    }
    for (const auto& e : ds.eixos) {
        if (e == "coordenadas" || e == "coordinates") continue;
        std::string c = normalizar_nome(e);
        if (std::find(chave.begin(), chave.end(), c) == chave.end()) chave.push_back(c);
    }
    return chave;
}

static std::string inferir_tipo_coluna(const std::string& declarado, const std::vector<Registro>& regs, const std::string& coluna) {
    if (declarado == "integer") return "INTEGER";
    if (declarado == "real") return "REAL";
    if (declarado == "text") return "TEXT";
    bool viu_real = false, viu_texto = false, viu_int = false, viu_algo = false;
    size_t n = 0;
    for (const auto& r : regs) {
        if (n++ > 200) break;
        auto it = r.find(coluna);
        if (it == r.end() || std::holds_alternative<std::monostate>(it->second)) continue;
        viu_algo = true;
        if (std::holds_alternative<std::string>(it->second)) {
            viu_texto = true;
            break;
        }
        double v = std::get<double>(it->second);
        if (std::floor(v) == v && std::fabs(v) < 9e15) viu_int = true;
        else viu_real = true;
    }
    if (viu_texto) return "TEXT";
    if (viu_real) return "REAL";
    if (viu_int) return "INTEGER";
    if (viu_algo) return "REAL";
    if (coluna == "data" || coluna == "hora" || coluna == "timezone") return "TEXT";
    if (coluna == "latitude" || coluna == "longitude") return "REAL";
    return "TEXT";
}

std::vector<std::string> colunas_da_tabela(sqlite3* conn, const std::string& tabela) {
    std::vector<std::string> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = "PRAGMA table_info(\"" + tabela + "\")";
    if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_finalize(st);
        return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* nome = sqlite3_column_text(st, 1);
        if (nome) out.emplace_back(reinterpret_cast<const char*>(nome));
    }
    sqlite3_finalize(st);
    return out;
}

void garantir_tabela(const DatasetCfg& ds, const std::vector<Registro>& amostra) {
    sqlite3* conn = get_connection_para(ds.db);
    std::vector<std::string> ordem;
    if (!ds.chave_data.empty() && ds.iterar_datas) ordem.push_back(ds.chave_data);
    if (ds.salvar_hora && !ds.chave_hora.empty() && ds.chave_hora != ds.chave_data)
        ordem.push_back(ds.chave_hora);
    if (ds.usar_coordenadas) {
        ordem.push_back("latitude");
        ordem.push_back("longitude");
    }
    for (const auto& c : colunas_chave(ds)) {
        if (std::find(ordem.begin(), ordem.end(), c) == ordem.end()) ordem.push_back(c);
    }
    for (const auto& c : ds.colunas) {
        if (std::find(ordem.begin(), ordem.end(), c.coluna) == ordem.end()) ordem.push_back(c.coluna);
    }
    for (const auto& r : amostra) {
        for (const auto& kv : r) {
            if (kv.first == "created_at") continue;
            if (std::find(ordem.begin(), ordem.end(), kv.first) == ordem.end())
                ordem.push_back(kv.first);
        }
    }
    if (ds.salvar_timezone &&
        std::find(ordem.begin(), ordem.end(), "timezone") == ordem.end())
        ordem.push_back("timezone");

    std::map<std::string, std::string> tipo_decl;
    for (const auto& c : ds.colunas) tipo_decl[c.coluna] = c.tipo;

    std::vector<std::string> existentes = colunas_da_tabela(conn, ds.tabela);
    if (existentes.empty()) {
        std::string sql = "CREATE TABLE IF NOT EXISTS \"" + ds.tabela + "\" (\n";
        bool prim = true;
        for (const auto& col : ordem) {
            std::string decl = "auto";
            auto it = tipo_decl.find(col);
            if (it != tipo_decl.end()) decl = it->second;
            sql += (prim ? "  \"" : ", \"") + col + "\" " + inferir_tipo_coluna(decl, amostra, col) + "\n";
            prim = false;
        }
        sql += ", created_at TEXT";
        std::vector<std::string> chave = colunas_chave(ds);
        if (!chave.empty()) {
            sql += ",\n  UNIQUE (\"" + juntar(chave, "\", \"") + "\")\n)";
        } else if (!ds.iterar_datas) {
            // Se for dataset de registros sem data (ex: API REST), tenta UNIQUE em 'name', 'id' ou 'url'
            std::string ucol;
            for (const auto& c : {"id", "name", "nome", "url"}) {
                if (std::find(ordem.begin(), ordem.end(), c) != ordem.end()) {
                    ucol = c;
                    break;
                }
            }
            if (!ucol.empty()) sql += ",\n  UNIQUE (\"" + ucol + "\")\n)";
            else sql += "\n)";
        } else {
            sql += "\n)";
        }
        executar_sql(conn, sql);
        log("[DB] Tabela criada: " + ds.db + "." + ds.tabela + " (" +
            std::to_string(ordem.size()) + " colunas)");
    } else {
        for (const auto& col : ordem) {
            if (std::find(existentes.begin(), existentes.end(), col) == existentes.end()) {
                std::string decl = "auto";
                auto it = tipo_decl.find(col);
                if (it != tipo_decl.end()) decl = it->second;
                executar_sql(conn, "ALTER TABLE \"" + ds.tabela + "\" ADD COLUMN \"" + col +
                                       "\" " + inferir_tipo_coluna(decl, amostra, col));
                log("[DB] Coluna adicionada: " + ds.tabela + "." + col);
            }
        }
        if (std::find(existentes.begin(), existentes.end(), "created_at") == existentes.end())
            executar_sql(conn, "ALTER TABLE \"" + ds.tabela + "\" ADD COLUMN created_at TEXT");
        // Migracao: datasets sem data criados antes do fix tem coluna "data"
        // fantasma (toda NULL) + UNIQUE(data). Remove para nao poluir.
        if (!ds.iterar_datas && !ds.chave_data.empty() &&
            std::find(existentes.begin(), existentes.end(), ds.chave_data) != existentes.end()) {
            bool so_nulos = true;
            {
                std::string q = "SELECT COUNT(*) FROM \"" + ds.tabela + "\" WHERE \"" +
                                ds.chave_data + "\" IS NOT NULL";
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(conn, q.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0)
                        so_nulos = false;
                }
                sqlite3_finalize(st);
            }
            if (so_nulos) {
                executar_sql(conn, "DROP INDEX IF EXISTS idx_" + ds.tabela + "_chave");
                executar_sql(conn, "DROP INDEX IF EXISTS idx_" + ds.tabela + "_date");
                std::string tmp = ds.tabela + "_mig";
                std::vector<std::string> keep;
                for (const auto& c : existentes)
                    if (c != ds.chave_data) keep.push_back("\"" + c + "\"");
                if (!keep.empty()) {
                    executar_sql(conn, "DROP TABLE IF EXISTS \"" + tmp + "\"");
                    executar_sql(conn, "CREATE TABLE \"" + tmp + "\" AS SELECT " +
                                           juntar(keep, ", ") + " FROM \"" + ds.tabela + "\"");
                    executar_sql(conn, "DROP TABLE \"" + ds.tabela + "\"");
                    executar_sql(conn, "ALTER TABLE \"" + tmp + "\" RENAME TO \"" + ds.tabela + "\"");
                    log("[DB] Coluna fantasma removida: " + ds.tabela + "." + ds.chave_data);
                }
            }
        }
    }
    auto chave_idx = colunas_chave(ds);
    if (!chave_idx.empty()) {
        executar_sql(conn, "CREATE INDEX IF NOT EXISTS idx_" + ds.tabela + "_chave ON \"" +
                               ds.tabela + "\"(" + juntar(chave_idx, ", ") + ")");
    }
    if (!ds.chave_data.empty() && ds.iterar_datas)
        executar_sql(conn, "CREATE INDEX IF NOT EXISTS idx_" + ds.tabela + "_date ON \"" +
                               ds.tabela + "\"(\"" + ds.chave_data + "\")");
}

void criar_tabelas_datasets() {
    for (const auto& ds : CFG.datasets) {
        if (ds.tipo_saida == "arquivo") continue;
        if (ds.tipo_saida == "wikipedia" || ds.tipo_saida == "wiki") continue; // esquema proprio (paginas/progresso/...)
        if (ds.tipo_saida == "drive" || ds.tipo_saida == "google_drive" || !ds.drive_folder_id.empty()) continue;
        try {
            std::vector<Registro> vazio;
            garantir_tabela(ds, vazio);
        } catch (const std::exception& e) {
            log(std::string("[WARNING] ") + ds.db + "." + ds.tabela + ": " + e.what());
        }
    }
}

void salvar_dataset(const DatasetCfg& ds, const std::vector<Registro>& registros) {
    if (registros.empty()) return;
    garantir_tabela(ds, registros);
    sqlite3* conn = get_connection_para(ds.db);
    std::vector<std::string> campos = colunas_da_tabela(conn, ds.tabela);
    campos.erase(std::remove(campos.begin(), campos.end(), "created_at"), campos.end());

    std::vector<std::string> pref;
    auto pref_add = [&](const std::string& c) {
        if (c.empty() || std::find(pref.begin(), pref.end(), c) != pref.end()) return;
        if (std::find(campos.begin(), campos.end(), c) != campos.end()) pref.push_back(c);
    };
    pref_add(ds.chave_data);
    if (ds.salvar_hora) pref_add(ds.chave_hora);
    if (ds.usar_coordenadas) {
        pref_add("latitude");
        pref_add("longitude");
    }
    pref_add("timezone");
    for (const auto& c : campos) {
        if (std::find(pref.begin(), pref.end(), c) == pref.end()) pref.push_back(c);
    }
    campos.swap(pref);
    if (campos.empty()) return;

    std::vector<std::string> chave = colunas_chave(ds);

    std::string sql;
    if (!ds.iterar_datas && chave.empty()) {
        // Sem data e sem chave declarada:
        // Verifica se a tabela possui UNIQUE na coluna 'name', 'id' ou 'url' para evitar registros duplicados
        std::string fallback_unique;
        for (const std::string& col_candidata : {"id", "name", "nome", "url"}) {
            if (std::find(campos.begin(), campos.end(), col_candidata) != campos.end()) {
                fallback_unique = col_candidata;
                break;
            }
        }
        std::vector<std::string> campos_aspas;
        for (const auto& c : campos) campos_aspas.push_back("\"" + c + "\"");

        std::vector<std::string> marc(campos.size() + 1, "?");
        sql = "INSERT INTO \"" + ds.tabela + "\" (" + juntar(campos_aspas, ",") +
              ", created_at) VALUES (" + juntar(marc, ",") + ")";
        if (!fallback_unique.empty()) {
            std::vector<std::string> upd;
            for (const auto& c : campos) {
                if (c != fallback_unique) upd.push_back("\"" + c + "\"=excluded.\"" + c + "\"");
            }
            if (!upd.empty()) {
                sql += " ON CONFLICT(\"" + fallback_unique + "\") DO UPDATE SET " + juntar(upd, ",");
            } else {
                sql += " ON CONFLICT DO NOTHING";
            }
        }
    } else {
        std::vector<std::string> campos_aspas;
        for (const auto& c : campos) campos_aspas.push_back("\"" + c + "\"");

        std::vector<std::string> marc(campos.size() + 1, "?");
        sql = "INSERT INTO \"" + ds.tabela + "\" (" + juntar(campos_aspas, ",") +
              ", created_at) VALUES (" + juntar(marc, ",") + ")";
        std::vector<std::string> upd;
        for (const auto& c : campos) {
            if (std::find(chave.begin(), chave.end(), c) == chave.end())
                upd.push_back("\"" + c + "\"=excluded.\"" + c + "\"");
        }
        if (!upd.empty() && !chave.empty())
            sql += " ON CONFLICT(\"" + juntar(chave, "\",\"") + "\") DO UPDATE SET " + juntar(upd, ",");
        else
            sql += " ON CONFLICT DO NOTHING";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(conn);
        sqlite3_finalize(stmt);
        throw std::runtime_error("SQLite: " + err);
    }
    const std::string agora = agora_iso();
    executar_sql(conn, "BEGIN");
    size_t lote = 0;
    for (const auto& r : registros) {
        int idx = 1;
        for (const auto& c : campos) {
            auto it = r.find(c);
            if (it == r.end() || std::holds_alternative<std::monostate>(it->second)) {
                sqlite3_bind_null(stmt, idx);
            } else if (std::holds_alternative<double>(it->second)) {
                sqlite3_bind_double(stmt, idx, std::get<double>(it->second));
            } else {
                const std::string& s = std::get<std::string>(it->second);
                sqlite3_bind_text(stmt, idx, s.c_str(), static_cast<int>(s.size()),
                                  SQLITE_TRANSIENT);
            }
            ++idx;
        }
        sqlite3_bind_text(stmt, idx, agora.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE)
            log("[ERROR] " + ds.tabela + ": " + sqlite3_errmsg(conn));
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (++lote >= static_cast<size_t>(CFG.max_sql_dates_por_lote)) {
            executar_sql(conn, "COMMIT");
            executar_sql(conn, "BEGIN");
            lote = 0;
        }
    }
    executar_sql(conn, "COMMIT");
    sqlite3_finalize(stmt);
}

long long contar_linhas_tabela(const DatasetCfg& ds) {
    sqlite3* conn = get_connection_para(ds.db);
    std::vector<std::string> cols = colunas_da_tabela(conn, ds.tabela);
    if (cols.empty()) return 0;
    std::string sql = "SELECT COUNT(*) FROM \"" + ds.tabela + "\"";
    sqlite3_stmt* st = nullptr;
    long long n = 0;
    if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

ExistentesGen carregar_existentes_generico(const std::vector<std::string>& datas) {
    ExistentesGen out;
    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
        const DatasetCfg& ds = CFG.datasets[di];
        if (ds.tipo_saida == "arquivo") continue;
        if (ds.tipo_saida == "drive" || ds.tipo_saida == "google_drive" || !ds.drive_folder_id.empty()) continue;
        sqlite3* conn = get_connection_para(ds.db);
        std::vector<std::string> cols = colunas_da_tabela(conn, ds.tabela);
        bool tem_data =
            !ds.chave_data.empty() &&
            std::find(cols.begin(), cols.end(), ds.chave_data) != cols.end();
        auto& mapa = out[di];

        if (ds.usar_coordenadas || CFG.coordenadas.empty()) {
            std::vector<std::pair<double, double>> lista_coords = CFG.coordenadas;
            if (lista_coords.empty()) lista_coords.push_back({0.0, 0.0});
            for (const auto& c : lista_coords) {
                mapa[num_str(c.first) + "," + num_str(c.second)] = ConjuntoDatas();
            }

            bool tem_lat = std::find(cols.begin(), cols.end(), "latitude") != cols.end();
            bool tem_lon = std::find(cols.begin(), cols.end(), "longitude") != cols.end();
            if (!tem_data || !tem_lat || !tem_lon || datas.empty()) continue;

            std::string sql = "SELECT latitude, longitude, \"" + ds.chave_data + "\" FROM \"" +
                              ds.tabela + "\" WHERE \"" + ds.chave_data +
                              "\" BETWEEN ? AND ?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                sqlite3_finalize(st);
                continue;
            }
            sqlite3_bind_text(st, 1, datas.front().c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, datas.back().c_str(), -1, SQLITE_TRANSIENT);
            ConjuntoDatas periodo(datas.begin(), datas.end());
            while (sqlite3_step(st) == SQLITE_ROW) {
                double la = sqlite3_column_double(st, 0);
                double lo = sqlite3_column_double(st, 1);
                const unsigned char* tx = sqlite3_column_text(st, 2);
                if (!tx) continue;
                std::string dt(reinterpret_cast<const char*>(tx));
                if (!periodo.count(dt)) continue;
                std::string k = num_str(la) + "," + num_str(lo);
                auto it = mapa.find(k);
                if (it != mapa.end()) {
                    it->second.insert(dt);
                }
            }
            sqlite3_finalize(st);
        } else {
            // Eixos/Dimensões genéricas
            auto contextos = expandir_contextos(ds);
            for (const auto& ctx : contextos) {
                std::string k_repr;
                for (const auto& kv : ctx) {
                    if (chave_ctx_interna(kv.first)) continue;
                    if (!k_repr.empty()) k_repr += "|";
                    k_repr += kv.first + "=" + kv.second;
                }
                mapa[k_repr] = ConjuntoDatas();
            }

            if (!tem_data || datas.empty()) continue;

            // Determinar quais colunas de dimensão estão na tabela
            std::vector<std::string> cols_dim;
            for (const auto& e : ds.eixos) {
                std::string c_norm = normalizar_nome(e);
                if (std::find(cols.begin(), cols.end(), c_norm) != cols.end()) {
                    cols_dim.push_back(c_norm);
                }
            }

            std::string sql = "SELECT ";
            for (const auto& c : cols_dim) sql += "\"" + c + "\", ";
            sql += "\"" + ds.chave_data + "\" FROM \"" + ds.tabela + "\" WHERE \"" + ds.chave_data + "\" BETWEEN ? AND ?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                sqlite3_finalize(st);
                continue;
            }
            sqlite3_bind_text(st, 1, datas.front().c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, datas.back().c_str(), -1, SQLITE_TRANSIENT);
            ConjuntoDatas periodo(datas.begin(), datas.end());
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string k_repr;
                for (size_t ci = 0; ci < cols_dim.size(); ++ci) {
                    const unsigned char* val = sqlite3_column_text(st, static_cast<int>(ci));
                    if (!k_repr.empty()) k_repr += "|";
                    k_repr += cols_dim[ci] + "=" + (val ? reinterpret_cast<const char*>(val) : "");
                }
                const unsigned char* tx = sqlite3_column_text(st, static_cast<int>(cols_dim.size()));
                if (!tx) continue;
                std::string dt(reinterpret_cast<const char*>(tx));
                if (!periodo.count(dt)) continue;
                auto it = mapa.find(k_repr);
                if (it != mapa.end()) {
                    it->second.insert(dt);
                }
            }
            sqlite3_finalize(st);
        }
    }
    return out;
}
