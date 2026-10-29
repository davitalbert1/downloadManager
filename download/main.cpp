#include "config.h"
#include "engine_generico.h"
#include "file_dataset.h"
#include "google_drive.h"
#include "rate_limit.h"
#include "storage_sqlite.h"
#include "utils.h"
#include "video_downloader.h"
#include "wikipedia_importer.h"
#include <curl/curl.h>
#include <algorithm>
#include <iostream>
#include <vector>

namespace {
    struct CurlGlobal {
        CurlGlobal() {
            curl_global_init(CURL_GLOBAL_DEFAULT);
        }
        ~CurlGlobal() {
            curl_global_cleanup();
        }
    };
}

int main(int argc, char** argv) {
    std::string caminho_ini = "download.ini";
    std::vector<std::string> filtros_secao;
    std::vector<std::string> urls_diretas;
    std::string pasta_saida_cli;
    std::string nome_arquivo_cli;
    std::string qualidade_cli;
    int max_workers_cli = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string arg_lower = arg;
        std::transform(arg_lower.begin(), arg_lower.end(), arg_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (arg == "-h" || arg == "--help") {
            std::cout << "Uso:\n"
                      << "  download.exe <url_ou_link_video> [-o pasta_saida] [-n nome_arquivo] [-q qualidade]\n"
                      << "  download.exe [caminho_config.ini] [-d dataset_id] [dataset1,dataset2]\n"
                      << "\nExemplos de Download de Video:\n"
                      << "  download.exe https://www.enma.lol/watch/k-on-5680?ep=1\n"
                      << "  download.exe https://www.enma.lol/watch/k-on-5680?ep=1 -o videos/k_on\n"
                      << "  download.exe https://example.com/stream.m3u8 -o videos -n meu_video\n"
                      << "\nExemplos de Datasets e APIs:\n"
                      << "  download.exe download.ini -d clima_diario\n";
            return 0;
        } else if (arg == "-u" || arg == "--url") {
            if (i + 1 < argc) urls_diretas.push_back(argv[++i]);
        } else if (arg == "-o" || arg == "--pasta" || arg == "--output" || arg == "--out") {
            if (i + 1 < argc) pasta_saida_cli = argv[++i];
        } else if (arg == "-n" || arg == "--nome" || arg == "--name") {
            if (i + 1 < argc) nome_arquivo_cli = argv[++i];
        } else if (arg == "-q" || arg == "--qualidade" || arg == "--quality") {
            if (i + 1 < argc) qualidade_cli = argv[++i];
        } else if (arg == "-w" || arg == "--workers") {
            if (i + 1 < argc) max_workers_cli = std::atoi(argv[++i]);
        } else if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
            urls_diretas.push_back(arg);
        } else if (arg.size() >= 4 && arg_lower.rfind(".ini") == arg.size() - 4) {
            caminho_ini = arg;
        } else if (arg == "-d" || arg == "--dataset" || arg == "-s" || arg == "--secao") {
            if (i + 1 < argc) {
                filtros_secao.push_back(argv[++i]);
            }
        } else {
            if (arg.empty() || arg.front() == '-') continue;
            for (const auto& parte : dividir(arg, ',')) {
                if (!parte.empty()) filtros_secao.push_back(parte);
            }
        }
    }

    carregar_env(".env");

    // Se foram informadas URLs diretas via linha de comando (ex: links do enma.lol, streams ou vídeos diretos)
    if (!urls_diretas.empty()) {
        CurlGlobal curl_guard;
        iniciar_rate_limit();
        int sucesso_count = 0;
        for (const auto& u : urls_diretas) {
            OpcoesVideo op;
            op.url = u;
            if (!pasta_saida_cli.empty()) op.pasta_saida = pasta_saida_cli;
            if (!nome_arquivo_cli.empty()) op.nome_arquivo = nome_arquivo_cli;
            if (!qualidade_cli.empty()) op.qualidade = qualidade_cli;
            if (max_workers_cli > 0) op.max_workers = max_workers_cli;
            if (baixar_video(op)) {
                ++sucesso_count;
            }
        }
        return (sucesso_count > 0) ? 0 : 1;
    }
    carregar_config(caminho_ini);

    if (!filtros_secao.empty()) {
        std::vector<DatasetCfg> filtrados;
        for (const auto& ds : CFG.datasets) {
            for (const auto& f : filtros_secao) {
                std::string fn = f;
                if (fn.rfind("dataset:", 0) == 0) fn = fn.substr(8);
                std::string id_low = minusculas(ds.id);
                std::string fn_low = minusculas(fn);
                if (ds.id == fn || id_low == fn_low) {
                    filtrados.push_back(ds);
                    break;
                }
            }
        }
        CFG.datasets = std::move(filtrados);
    }

    if (CFG.datasets.empty()) {
        std::cerr << "[ERROR] Nenhum dataset correspondente encontrado em " << caminho_ini << "\n";
        return 1;
    }

    CurlGlobal curl_guard;
    iniciar_rate_limit();

    try {
        // Executa rigorosamente na ordem declarada em download.ini (de cima para baixo)
        bool tem_db = false;
        bool tem_datas = false;
        for (const auto& ds : CFG.datasets) {
            if (!dataset_generico(ds) || ds.so_fallback) continue;
            tem_db = true;
            if (ds.iterar_datas) tem_datas = true;
        }

        std::vector<std::string> datas;
        ExistentesGen existentes;

        if (tem_db) {
            criar_tabelas_datasets();
            if (tem_datas) {
                datas = expandir_periodo(CFG.periodo_inicio, CFG.periodo_fim);
                if (!datas.empty()) {
                    std::cout << "============================================================\n"
                              << "DOWNLOAD GENERICO DE DADOS E APIS\n"
                              << "============================================================\n"
                              << "Configuracao: " << caminho_ini << "\n"
                              << "Fuso horario: " << CFG.fuso_horario << "\n"
                              << "Banco de dados: " << CFG.db_path << "\n"
                              << "download_workers: " << CFG.download_workers
                              << " | process_workers: " << CFG.process_workers << "\n";

                    std::cout << "\n[DB] Carregando registros existentes...\n";
                    existentes = carregar_existentes_generico(datas);
                    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
                        if (!dataset_generico(CFG.datasets[di])) continue;
                        long long tot = 0;
                        auto it = existentes.find(di);
                        if (it != existentes.end())
                            for (const auto& kv : it->second) tot += (long long)kv.second.size();
                        std::cout << "[DB] " << CFG.datasets[di].id << ": " << tot << " dias existentes\n";
                    }
                }
            } else {
                datas = { hoje() };
            }
        }

        int codigo_saida = 0;
        for (size_t di = 0; di < CFG.datasets.size(); ++di) {
            const auto& ds = CFG.datasets[di];
            if (ds.so_fallback) continue;
            if (ds.tipo_saida == "drive" || ds.tipo_saida == "google_drive" || !ds.drive_folder_id.empty()) {
                executar_dataset_google_drive(ds);
            } else if (ds.tipo_saida == "wikipedia" || ds.tipo_saida == "wiki") {
                executar_dataset_wikipedia(ds);
            } else if (ds.tipo_saida == "video" || ds.tipo_saida == "stream" ||
                       eh_midia_direta(ds.url) || ds.url.find("enma.lol/watch") != std::string::npos) {
                executar_dataset_video(ds);
            } else if (ds.tipo_saida == "arquivo") {
                executar_dataset_arquivo(ds);
            } else {
                if (executar_generico(datas, existentes, di) != 0) codigo_saida = 1;
            }
        }
        return codigo_saida;
    } catch (const std::exception& e) {
        std::cerr << "\n[ERRO] " << e.what() << "\n";
        return 1;
    }
}
