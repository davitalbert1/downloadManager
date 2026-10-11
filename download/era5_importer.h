#pragma once
#include <sqlite3.h>
#include <string>
#include <vector>
#include <map>
#include <variant>

// Tipo genérico de valor de registro (compatível com o restante do projeto)
using Era5Valor = std::variant<std::monostate, double, std::string>;
using Era5Registro = std::map<std::string, Era5Valor>;

struct Era5ImportConfig {
    std::string pasta_grib = "dados_era5"; // Pasta contendo os arquivos .grib
    std::string db_path = "clima.db"; // Banco SQLite destino
    std::string tabela = "clima_horario"; // Tabela SQLite destino (unificada)
    std::string ini_path = "download.ini"; // Arquivo .ini para ler [coordenadas] se vazias
    std::vector<std::pair<double, double>> coordenadas; // Lat, Lon dos pontos a extrair
    bool converter_kelvin_para_celsius = true; // Se 2T / 2D estiver em Kelvin (~ > 150), subtrai 273.15
    size_t tamanho_lote = 1000; // Tamanho do lote de inserção (transações SQLite)
};

// Funções do importador
std::vector<std::pair<double, double>> carregar_coordenadas_ini(const std::string& ini_path);
bool era5_mes_ja_no_banco(sqlite3* db, const std::string& tabela, int ano, int mes, const std::vector<std::pair<double, double>>& coords, const std::vector<std::string>& horarios);
bool importar_arquivo_era5(const std::string& grib_path, const Era5ImportConfig& cfg);
size_t importar_pasta_era5(const Era5ImportConfig& cfg);
