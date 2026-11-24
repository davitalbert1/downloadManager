#include "grib_importer.h"
#include "i18n.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    GribImportConfig cfg;

    std::string arquivo_especifico = "";
    std::string idioma_cli;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-l" || arg == "--lang" || arg == "--idioma") && i + 1 < argc) {
            idioma_cli = argv[i + 1];
        }
    }
    i18n::iniciar(idioma_cli);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            i18n::cout << "Uso: grib_import.exe [opcoes]\n"
                       << "Opcoes:\n"
                       << "  --pasta <caminho>     Pasta contendo arquivos .grib (padrao: dados_grib)\n"
                       << "  --arquivo <caminho>   Importar apenas um arquivo .grib especifico\n"
                       << "  --db <caminho>        Banco SQLite destino (padrao: clima.db)\n"
                       << "  --tabela <nome>       Tabela destino (padrao: clima_horario)\n"
                       << "  --ini <caminho>       Arquivo de configuracao para coordenadas (padrao: download.ini)\n"
                       << "  --lote <tamanho>      Tamanho do lote de commit SQL (padrao: 1000)\n"
                       << "  --workers <n>         Threads de importacao GRIB->SQLite (padrao: nucleos)\n"
                       << "  --no-kelvin-to-celsius Nao converter temperatura de Kelvin para Celsius\n"
                       << "  -l, --lang <pt|en|es> Idioma da interface (padrao: auto)\n";
            return 0;
        } else if (arg == "-l" || arg == "--lang" || arg == "--idioma") {
            if (i + 1 < argc) ++i;
        } else if (arg == "--pasta" && i + 1 < argc) {
            cfg.pasta_grib = argv[++i];
        } else if (arg == "--arquivo" && i + 1 < argc) {
            arquivo_especifico = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            cfg.db_path = argv[++i];
        } else if (arg == "--tabela" && i + 1 < argc) {
            cfg.tabela = argv[++i];
        } else if (arg == "--ini" && i + 1 < argc) {
            cfg.ini_path = argv[++i];
        } else if (arg == "--lote" && i + 1 < argc) {
            cfg.tamanho_lote = std::stoul(argv[++i]);
        } else if (arg == "--workers" && i + 1 < argc) {
            cfg.process_workers = std::stoi(argv[++i]);
        } else if (arg == "--no-kelvin-to-celsius") {
            cfg.converter_kelvin_para_celsius = false;
        } else if (arg.size() > 5 && arg.substr(arg.size() - 5) == ".grib") {
            arquivo_especifico = arg;
        }
    }

    if (!arquivo_especifico.empty()) {
        bool ok = importar_arquivo_grib(arquivo_especifico, cfg);
        return ok ? 0 : 1;
    } else {
        size_t qtd = importar_pasta_grib(cfg);
        return qtd > 0 ? 0 : 1;
    }
}
