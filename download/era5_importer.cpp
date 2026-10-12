#include "era5_importer.h"
#include <gdal.h>
#include <gdal_priv.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

// Utilitários internos
static std::string trim_str(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::string normalizar_nome_coluna(std::string s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == '_' || c == '-' || c == ' ') {
            if (!out.empty() && out.back() != '_') out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "col" : out;
}

// Conversao civil pura de segundos Unix (UTC) que suporta timestamps anteriores a 1970
static void unix_time_para_data_hora(int64_t unix_time, int& ano, int& mes, int& dia, int& hora, int& min, int& seg) {
    int64_t dias = unix_time / 86400;
    int64_t seg_no_dia = unix_time % 86400;
    if (seg_no_dia < 0) {
        seg_no_dia += 86400;
        dias -= 1;
    }

    hora = static_cast<int>(seg_no_dia / 3600);
    min  = static_cast<int>((seg_no_dia % 3600) / 60);
    seg  = static_cast<int>(seg_no_dia % 60);

    int64_t z = dias + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    dia = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    mes = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    ano = static_cast<int>(y + (mes <= 2 ? 1 : 0));
}

static std::string formatar_timestamp_data(int64_t unix_time) {
    int ano = 0, mes = 0, dia = 0, hora = 0, min = 0, seg = 0;
    unix_time_para_data_hora(unix_time, ano, mes, dia, hora, min, seg);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ano, mes, dia);
    return std::string(buf);
}

static std::string formatar_timestamp_hora(int64_t unix_time) {
    int ano = 0, mes = 0, dia = 0, hora = 0, min = 0, seg = 0;
    unix_time_para_data_hora(unix_time, ano, mes, dia, hora, min, seg);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hora, min);
    return std::string(buf);
}

static std::string obter_iso_agora() {
    auto now = std::chrono::system_clock::now();
    int64_t s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    int ano = 0, mes = 0, dia = 0, hora = 0, min = 0, seg = 0;
    unix_time_para_data_hora(s, ano, mes, dia, hora, min, seg);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", ano, mes, dia, hora, min, seg);
    return std::string(buf);
}

// Mapeia códigos GRIB_ELEMENT conhecidos para os nomes das colunas existentes no banco (clima_horario)
static std::string mapear_elemento_para_coluna(const std::string& elem, const std::string& comment) {
    std::string el = elem;
    std::transform(el.begin(), el.end(), el.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    if (el == "2T") return "temperature_2m";
    if (el == "2D") return "dewpoint_2m";
    if (el == "10U") return "wind_u_10m";
    if (el == "10V") return "wind_v_10m";
    if (el == "SP") return "surface_pressure";
    if (el == "MSL") return "pressure_msl";
    if (el == "TP") return "precipitation"; // Unificado com clima_horario
    if (el == "SSRD") return "solar_radiation_downwards";

    if (!comment.empty()) {
        std::string c_norm = normalizar_nome_coluna(comment);
        if (c_norm == "total_precipitation") return "precipitation";
        if (!c_norm.empty()) return c_norm;
    }
    return normalizar_nome_coluna(elem);
}

std::vector<std::pair<double, double>> carregar_coordenadas_ini(const std::string& ini_path) {
    std::vector<std::pair<double, double>> coords;
    std::ifstream file(ini_path);
    if (!file.is_open()) return coords;

    std::string line;
    bool in_coord_sec = false;
    while (std::getline(file, line)) {
        std::string tr = trim_str(line);
        if (tr.empty()) continue;
        if (tr[0] == '[' && tr.back() == ']') {
            std::string sec = trim_str(tr.substr(1, tr.size() - 2));
            in_coord_sec = (sec == "coordenadas");
            continue;
        }
        if (!in_coord_sec) continue;

        // Limpa comentários iniciados com ';' ou '#'
        auto pos_comment = tr.find_first_of(";#");
        if (pos_comment != std::string::npos) tr = trim_str(tr.substr(0, pos_comment));
        if (tr.empty()) continue;

        auto pos_comma = tr.find(',');
        if (pos_comma != std::string::npos) {
            try {
                double lat = std::stod(trim_str(tr.substr(0, pos_comma)));
                double lon = std::stod(trim_str(tr.substr(pos_comma + 1)));
                coords.emplace_back(lat, lon);
            } catch (...) {}
        }
    }
    return coords;
}

// Verifica se um mês (ano, mes) já está totalmente povoado no banco para as coordenadas fornecidas
bool era5_mes_ja_no_banco(sqlite3* db, const std::string& tabela, int ano, int mes, const std::vector<std::pair<double, double>>& coords, const std::vector<std::string>& horarios) {
    if (!db || coords.empty()) return false;

    // Checa se a tabela existe
    std::string sql_check = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=\"" + tabela + "\"";
    sqlite3_stmt* stmt_chk = nullptr;
    if (sqlite3_prepare_v2(db, sql_check.c_str(), -1, &stmt_chk, nullptr) != SQLITE_OK) {
        if (stmt_chk) sqlite3_finalize(stmt_chk);
        return false;
    }
    bool existe = (sqlite3_step(stmt_chk) == SQLITE_ROW);
    sqlite3_finalize(stmt_chk);
    if (!existe) return false;

    int dias_no_mes = 31;
    if (mes == 4 || mes == 6 || mes == 9 || mes == 11) dias_no_mes = 30;
    else if (mes == 2) {
        bool bissexto = (ano % 4 == 0 && (ano % 100 != 0 || ano % 400 == 0));
        dias_no_mes = bissexto ? 29 : 28;
    }

    size_t qtd_horarios = horarios.empty() ? 2 : horarios.size();
    size_t esperado_por_coord = static_cast<size_t>(dias_no_mes) * qtd_horarios;

    char data_ini[16], data_fim[16];
    std::snprintf(data_ini, sizeof(data_ini), "%04d-%02d-01", ano, mes);
    std::snprintf(data_fim, sizeof(data_fim), "%04d-%02d-%02d", ano, mes, dias_no_mes);

    std::string sql_cnt = "SELECT count(*) FROM \"" + tabela + "\" WHERE latitude = ? AND longitude = ? AND data BETWEEN ? AND ?";
    sqlite3_stmt* stmt_cnt = nullptr;
    if (sqlite3_prepare_v2(db, sql_cnt.c_str(), -1, &stmt_cnt, nullptr) != SQLITE_OK) {
        if (stmt_cnt) sqlite3_finalize(stmt_cnt);
        return false;
    }

    bool todos_completos = true;
    for (const auto& c : coords) {
        sqlite3_bind_double(stmt_cnt, 1, c.first);
        sqlite3_bind_double(stmt_cnt, 2, c.second);
        sqlite3_bind_text(stmt_cnt, 3, data_ini, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_cnt, 4, data_fim, -1, SQLITE_TRANSIENT);

        size_t count = 0;
        if (sqlite3_step(stmt_cnt) == SQLITE_ROW) {
            count = static_cast<size_t>(sqlite3_column_int64(stmt_cnt, 0));
        }
        sqlite3_reset(stmt_cnt);
        sqlite3_clear_bindings(stmt_cnt);

        // Se tiver menos registros que o esperado para este mês, consideramos incompleto
        if (count < esperado_por_coord) {
            todos_completos = false;
            break;
        }
    }
    sqlite3_finalize(stmt_cnt);
    return todos_completos;
}

// Garante que a tabela exista com as colunas necessárias e índice UNIQUE
static bool preparar_tabela_sqlite(sqlite3* db, const std::string& tabela, const std::vector<std::string>& colunas_variaveis) {
    // 1) Descobre colunas existentes na tabela
    std::set<std::string> cols_existentes;
    std::string sql_info = "PRAGMA table_info(\"" + tabela + "\")";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql_info.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* cname = sqlite3_column_text(stmt, 1);
            if (cname) cols_existentes.insert(reinterpret_cast<const char*>(cname));
        }
        sqlite3_finalize(stmt);
    }

    if (cols_existentes.empty()) {
        // Criar tabela do zero
        std::string sql_create = "CREATE TABLE IF NOT EXISTS \"" + tabela + "\" (\n"
                                 "  data TEXT NOT NULL,\n"
                                 "  hora TEXT NOT NULL,\n"
                                 "  latitude REAL NOT NULL,\n"
                                 "  longitude REAL NOT NULL";
        for (const auto& col : colunas_variaveis) {
            sql_create += ",\n  \"" + col + "\" REAL";
        }
        sql_create += ",\n  created_at TEXT,\n"
                      "  UNIQUE (data, hora, latitude, longitude)\n"
                      ");";

        char* errmsg = nullptr;
        if (sqlite3_exec(db, sql_create.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
            std::cerr << "[ERRO SQL] Criar tabela " << tabela << ": " << (errmsg ? errmsg : "") << "\n";
            sqlite3_free(errmsg);
            return false;
        }

        std::string sql_idx = "CREATE INDEX IF NOT EXISTS \"idx_" + tabela + "_tempo_loc\" ON \"" +
                              tabela + "\" (data, hora, latitude, longitude);";
        sqlite3_exec(db, sql_idx.c_str(), nullptr, nullptr, nullptr);
    } else {
        // Tabela já existe, adicionar colunas que faltam se necessário
        for (const auto& col : colunas_variaveis) {
            if (cols_existentes.find(col) == cols_existentes.end()) {
                std::string sql_alter = "ALTER TABLE \"" + tabela + "\" ADD COLUMN \"" + col + "\" REAL";
                char* errmsg = nullptr;
                if (sqlite3_exec(db, sql_alter.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
                    std::cerr << "[AVISO SQL] Adicionar coluna " << col << ": " << (errmsg ? errmsg : "") << "\n";
                    sqlite3_free(errmsg);
                }
            }
        }
        if (cols_existentes.find("created_at") == cols_existentes.end()) {
            sqlite3_exec(db, ("ALTER TABLE \"" + tabela + "\" ADD COLUMN created_at TEXT").c_str(), nullptr, nullptr, nullptr);
        }
    }
    return true;
}

// Estrutura de metadados de uma banda GRIB
struct BandaInfo {
    int band_index = 0;
    std::string element;
    std::string comment;
    std::string col_name;
    int64_t valid_time = 0;
    std::string data;
    std::string hora;
    double nodata = -999999.0;
    bool has_nodata = false;
};

// Ponto na grade raster (pixel e linha)
struct PixelCoord {
    double lat = 0.0;
    double lon = 0.0;
    int pixel_x = 0;
    int line_y = 0;
    bool valido = false;
};

bool importar_arquivo_era5(const std::string& grib_path, const Era5ImportConfig& cfg) {
    std::cout << "\n[ERA5 Import] Abrindo: " << grib_path << " ...\n";

    GDALAllRegister();
    GDALDataset* poDataset = static_cast<GDALDataset*>(GDALOpen(grib_path.c_str(), GA_ReadOnly));
    if (!poDataset) {
        std::cerr << "[ERRO GDAL] Nao foi possivel abrir: " << grib_path << "\n";
        return false;
    }

    int raster_x_size = poDataset->GetRasterXSize();
    int raster_y_size = poDataset->GetRasterYSize();
    int band_count = poDataset->GetRasterCount();

    double geo_transform[6];
    if (poDataset->GetGeoTransform(geo_transform) != CE_None) {
        std::cerr << "[ERRO GDAL] GeoTransform nao disponivel no arquivo GRIB.\n";
        GDALClose(poDataset);
        return false;
    }

    // geo_transform:
    // Xgeo = GT[0] + Xpixel*GT[1] + Yline*GT[2]
    // Ygeo = GT[3] + Xpixel*GT[4] + Yline*GT[5]
    // Para grade regular norte-sul (GT[2] == 0, GT[4] == 0):
    // Xpixel = (Xgeo - GT[0]) / GT[1]
    // Yline  = (Ygeo - GT[3]) / GT[5]
    double origin_x = geo_transform[0];
    double pixel_w  = geo_transform[1];
    double origin_y = geo_transform[3];
    double pixel_h  = geo_transform[5];

    // Coordenadas a extrair
    std::vector<std::pair<double, double>> coords = cfg.coordenadas;
    if (coords.empty() && !cfg.ini_path.empty()) {
        coords = carregar_coordenadas_ini(cfg.ini_path);
    }
    if (coords.empty()) {
        std::cerr << "[AVISO] Nenhuma coordenada definida para extracao.\n";
        GDALClose(poDataset);
        return false;
    }

    std::vector<PixelCoord> pixels;
    for (const auto& c : coords) {
        double lat = c.first;
        double lon = c.second;

        // Longitude ERA5 costuma ir de -180 a 180 ou de 0 a 360 dependendo do arquivo.
        // Se origin_x >= 0 e lon < 0, converte lon para [0, 360)
        double lon_calc = lon;
        if (origin_x >= 0.0 && lon_calc < 0.0) {
            lon_calc += 360.0;
        }

        int px = static_cast<int>(std::floor((lon_calc - origin_x) / pixel_w));
        int py = static_cast<int>(std::floor((lat - origin_y) / pixel_h));

        PixelCoord pc;
        pc.lat = lat;
        pc.lon = lon;
        pc.pixel_x = px;
        pc.line_y = py;
        if (px >= 0 && px < raster_x_size && py >= 0 && py < raster_y_size) {
            pc.valido = true;
            pixels.push_back(pc);
        } else {
            std::cerr << "[AVISO] Ponto lat=" << lat << " lon=" << lon
                      << " fora da extensao da grade (" << px << ", " << py << ")\n";
        }
    }

    if (pixels.empty()) {
        std::cerr << "[ERRO] Todos os pontos solicitados estao fora da grade.\n";
        GDALClose(poDataset);
        return false;
    }

    // 1) Escanear todas as bandas e metadados GRIB
    std::vector<BandaInfo> bandas;
    bandas.reserve(band_count);
    std::set<std::string> todas_colunas_set;

    for (int b = 1; b <= band_count; ++b) {
        GDALRasterBand* poBand = poDataset->GetRasterBand(b);
        if (!poBand) continue;

        BandaInfo info;
        info.band_index = b;
        int has_no_data = 0;
        double nodata = poBand->GetNoDataValue(&has_no_data);
        info.has_nodata = (has_no_data != 0);
        info.nodata = nodata;

        CSLConstList papszMeta = poBand->GetMetadata(nullptr);
        if (!papszMeta) {
            papszMeta = poBand->GetMetadata("GRIB");
        }
        if (papszMeta) {
            const char* pszElem = CSLFetchNameValue(papszMeta, "GRIB_ELEMENT");
            const char* pszComm = CSLFetchNameValue(papszMeta, "GRIB_COMMENT");
            const char* pszTime = CSLFetchNameValue(papszMeta, "GRIB_VALID_TIME");

            if (pszElem) info.element = pszElem;
            if (pszComm) info.comment = pszComm;
            if (pszTime) {
                try {
                    info.valid_time = std::stoll(pszTime);
                } catch (...) {}
            }
        }

        if (info.valid_time == 0) {
            // Tenta pegar do domínio padrão se não houver no GRIB
            const char* pszTime = poBand->GetMetadataItem("GRIB_VALID_TIME");
            if (pszTime) {
                try {
                    info.valid_time = std::stoll(pszTime);
                } catch (...) {}
            }
        }

        info.col_name = mapear_elemento_para_coluna(info.element, info.comment);
        todas_colunas_set.insert(info.col_name);
        info.data = formatar_timestamp_data(info.valid_time);
        info.hora = formatar_timestamp_hora(info.valid_time);

        bandas.push_back(info);
    }

    std::vector<std::string> lista_colunas(todas_colunas_set.begin(), todas_colunas_set.end());

    // 2) Conectar ao banco SQLite e garantir tabela
    try {
        fs::path pai = fs::path(cfg.db_path).parent_path();
        if (!pai.empty()) {
            std::error_code ec_dir;
            fs::create_directories(pai, ec_dir);
        }
    } catch (...) {}
    sqlite3* db = nullptr;
    if (sqlite3_open(cfg.db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[ERRO SQLite] Falha ao abrir banco: " << cfg.db_path << "\n";
        GDALClose(poDataset);
        return false;
    }

    sqlite3_exec(db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA busy_timeout = 60000;", nullptr, nullptr, nullptr);

    if (!preparar_tabela_sqlite(db, cfg.tabela, lista_colunas)) {
        sqlite3_close(db);
        GDALClose(poDataset);
        return false;
    }

    // 3) Agrupar bandas por (data, hora) para ler valores das colunas correspondentes
    // Chave de agrupamento: tempo_key = (data, hora)
    struct RegistroPonto {
        std::string data;
        std::string hora;
        double lat = 0.0;
        double lon = 0.0;
        std::map<std::string, double> valores;
    };

    // Mapeamento: (data + "_" + hora) -> vetor de bandas com seus elementos
    std::map<std::pair<std::string, std::string>, std::vector<BandaInfo>> grupos_tempo;
    for (const auto& b : bandas) grupos_tempo[{b.data, b.hora}].push_back(b);

    std::cout << "[ERA5 Import] Total de bandas: " << band_count
              << " | Instantes temporais: " << grupos_tempo.size()
              << " | Pontos: " << pixels.size() << "\n";

    // 4) Leitura otimizada por banda: para cada banda, lemos os pixels das coordenadas
    // Mapa: (data, hora, lat, lon) -> RegistroPonto
    using ChaveRegistro = std::tuple<std::string, std::string, double, double>;
    std::map<ChaveRegistro, RegistroPonto> registros_map;

    // Inicializa todos os registros esperados
    for (const auto& gt : grupos_tempo) {
        const std::string& d = gt.first.first;
        const std::string& h = gt.first.second;
        for (const auto& pc : pixels) {
            ChaveRegistro k{d, h, pc.lat, pc.lon};
            RegistroPonto rp;
            rp.data = d;
            rp.hora = h;
            rp.lat = pc.lat;
            rp.lon = pc.lon;
            registros_map[k] = rp;
        }
    }

    // Para cada banda, lê apenas os pontos necessários via RasterIO
    for (const auto& binfo : bandas) {
        GDALRasterBand* poBand = poDataset->GetRasterBand(binfo.band_index);
        if (!poBand) continue;

        for (const auto& pc : pixels) {
            double valor = 0.0;
            CPLErr err = poBand->RasterIO(GF_Read, pc.pixel_x, pc.line_y, 1, 1,
                                          &valor, 1, 1, GDT_Float64, 0, 0);
            if (err == CE_None) {
                if (binfo.has_nodata && std::fabs(valor - binfo.nodata) < 1e-4) continue; // NoData

                // Ajuste de Kelvin para Celsius caso necessário para 2T e 2D
                if (cfg.converter_kelvin_para_celsius &&
                    (binfo.element == "2T" || binfo.element == "2D") &&
                    valor > 150.0) {
                    valor -= 273.15;
                }

                ChaveRegistro k{binfo.data, binfo.hora, pc.lat, pc.lon};
                registros_map[k].valores[binfo.col_name] = valor;
            }
        }
    }

    GDALClose(poDataset);

    // 5) Salvar no banco SQLite em lote com UPSERT (ON CONFLICT DO UPDATE)
    // Monta o comando SQL preparado:
    // INSERT INTO tabela (data, hora, latitude, longitude, col1, col2..., created_at)
    // VALUES (?, ?, ?, ?, ?, ?, ..., ?)
    // ON CONFLICT(data, hora, latitude, longitude) DO UPDATE SET col1=excluded.col1, ...
    std::string sql_insert = "INSERT INTO \"" + cfg.tabela + "\" (data, hora, latitude, longitude";
    for (const auto& c : lista_colunas) {
        sql_insert += ", \"" + c + "\"";
    }
    sql_insert += ", created_at) VALUES (?, ?, ?, ?";
    for (size_t i = 0; i < lista_colunas.size(); ++i) sql_insert += ", ?";
    sql_insert += ", ?) ON CONFLICT(data, hora, latitude, longitude) DO UPDATE SET ";

    bool prim_upd = true;
    for (const auto& c : lista_colunas) {
        if (!prim_upd) sql_insert += ", ";
        sql_insert += "\"" + c + "\"=excluded.\"" + c + "\"";
        prim_upd = false;
    }
    sql_insert += ", created_at=excluded.created_at;";

    sqlite3_stmt* insert_stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql_insert.c_str(), -1, &insert_stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERRO SQLite] Preparar insert: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return false;
    }

    std::string iso_agora = obter_iso_agora();
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    size_t cont = 0;
    for (const auto& kv : registros_map) {
        const auto& rp = kv.second;

        sqlite3_bind_text(insert_stmt, 1, rp.data.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 2, rp.hora.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(insert_stmt, 3, rp.lat);
        sqlite3_bind_double(insert_stmt, 4, rp.lon);

        int param_idx = 5;
        for (const auto& c : lista_colunas) {
            auto itv = rp.valores.find(c);
            if (itv != rp.valores.end()) {
                sqlite3_bind_double(insert_stmt, param_idx, itv->second);
            } else {
                sqlite3_bind_null(insert_stmt, param_idx);
            }
            param_idx++;
        }
        sqlite3_bind_text(insert_stmt, param_idx, iso_agora.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            std::cerr << "[ERRO SQLite] Inserir registro: " << sqlite3_errmsg(db) << "\n";
        }
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);

        cont++;
        if (cont % cfg.tamanho_lote == 0) {
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        }
    }

    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_finalize(insert_stmt);
    sqlite3_close(db);

    std::cout << "[ERA5 Import] Concluido com sucesso: " << cont << " registros inseridos/atualizados na tabela '"
              << cfg.tabela << "'!\n";
    return true;
}

size_t importar_pasta_era5(const Era5ImportConfig& cfg) {
    if (!fs::exists(cfg.pasta_grib)) {
        std::cerr << "[ERRO] Pasta nao encontrada: " << cfg.pasta_grib << "\n";
        return 0;
    }

    std::vector<fs::path> arquivos_grib;
    for (const auto& entry : fs::directory_iterator(cfg.pasta_grib)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".grib" || ext == ".grb" || ext == ".grib2" || ext == ".grb2") {
            arquivos_grib.push_back(entry.path());
        }
    }

    std::sort(arquivos_grib.begin(), arquivos_grib.end());

    std::cout << "============================================================\n"
              << "IMPORTACAO DE DADOS ERA5 (GRIB -> SQLITE)\n"
              << "============================================================\n"
              << "Pasta de entrada: " << cfg.pasta_grib << "\n"
              << "Banco SQLite: " << cfg.db_path << "\n"
              << "Tabela destino: " << cfg.tabela << "\n"
              << "Arquivos GRIB: " << arquivos_grib.size() << " encontrados\n"
              << "============================================================\n";

    size_t processados = 0;
    for (const auto& p : arquivos_grib) {
        if (importar_arquivo_era5(p.string(), cfg)) processados++;
    }

    std::cout << "\n[FINALIZADO] " << processados << " de " << arquivos_grib.size()
              << " arquivos importados para o banco " << cfg.db_path << ".\n";
    return processados;
}
