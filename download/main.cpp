#include "config.h"
#include "engine_generico.h"
#include "file_dataset.h"
#include "google_drive.h"
#include "rate_limit.h"
#include "storage_sqlite.h"
#include "utils.h"
#include "wikipedia_importer.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    std::string caminho_ini = "download.ini";
    std::vector<std::string> filtros_secao;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string arg_lower = arg;
        std::transform(arg_lower.begin(), arg_lower.end(), arg_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (arg == "-h" || arg == "--help") {
            std::cout << "Uso: download.exe [caminho_config.ini] [-d dataset_id] [dataset1,dataset2]\n";
            return 0;
        } else if (arg.size() >= 4 && arg_lower.rfind(".ini") == arg.size() - 4) {
            caminho_ini = arg;
        } else if (arg == "-d" || arg == "--dataset" || arg == "-s" || arg == "--secao") {
            if (i + 1 < argc) {
                filtros_secao.push_back(argv[++i]);
            }
        } else {
            if (arg.front() == '-') continue;
            for (const auto& parte : dividir(arg, ',')) {
                if (!parte.empty()) filtros_secao.push_back(parte);
            }
        }
    }

    carregar_env(".env");
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

    curl_global_init(CURL_GLOBAL_DEFAULT);
    iniciar_rate_limit();

    try {
        // Executa rigorosamente na ordem declarada em download.ini (de cima para baixo)
        bool tem_db = false;
        for (const auto& ds : CFG.datasets) {
            if (ds.tipo_saida != "arquivo" && ds.tipo_saida != "drive" && ds.tipo_saida != "google_drive" &&
                ds.tipo_saida != "wikipedia" && ds.tipo_saida != "wiki" && ds.drive_folder_id.empty()) {
                tem_db = true;
                break;
            }
        }

        // Se não houver dataset com data (apenas records/APIs sem datas), usa lista com 1 dummy
        bool tem_datas = false;
        for (const auto& ds : CFG.datasets) {
            if (ds.iterar_datas && ds.tipo_saida != "arquivo" && ds.tipo_saida != "drive" && ds.tipo_saida != "google_drive" &&
                ds.tipo_saida != "wikipedia" && ds.tipo_saida != "wiki" && ds.drive_folder_id.empty()) {
                tem_datas = true;
                break;
            }
        }

        std::vector<std::string> datas;
        ExistentesGen existentes;
        double tload = 0.0;

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
                    auto t0 = std::chrono::steady_clock::now();
                    existentes = carregar_existentes_generico(datas);
                    tload = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
                        if (CFG.datasets[di].tipo_saida == "arquivo" ||
                            CFG.datasets[di].tipo_saida == "drive" ||
                            CFG.datasets[di].tipo_saida == "google_drive" ||
                            !CFG.datasets[di].drive_folder_id.empty()) continue;
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

        for (size_t di = 0; di < CFG.datasets.size(); ++di) {
            const auto& ds = CFG.datasets[di];
            if (ds.tipo_saida == "drive" || ds.tipo_saida == "google_drive" || !ds.drive_folder_id.empty()) {
                executar_dataset_google_drive(ds);
            } else if (ds.tipo_saida == "wikipedia" || ds.tipo_saida == "wiki") {
                executar_dataset_wikipedia(ds);
            } else if (ds.tipo_saida == "arquivo") {
                executar_dataset_arquivo(ds);
            } else {
                executar_generico(datas, tload, existentes, di);
            }
        }

        curl_global_cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[ERRO] " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}
