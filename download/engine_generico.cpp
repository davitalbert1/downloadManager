#include "engine_generico.h"
#include "file_dataset.h"
#include "http_client.h"
#include "json_parser.h"
#include "rate_limit.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <shared_mutex>
#include <thread>

static std::shared_mutex g_mtx_existentes;
static std::mutex g_write_mtx;
static std::mutex g_stats_mtx;
static std::mutex g_print_mtx;

static std::mt19937& rng() {
    static thread_local std::mt19937 gen{
        static_cast<std::mt19937::result_type>(std::random_device{}() ^
                                               std::hash<std::thread::id>{}(
                                                   std::this_thread::get_id()))};
    return gen;
}

static std::string url_do_dataset(const DatasetCfg& ds, const std::string& data) {
    if (!ds.url_passado.empty() || !ds.url_futuro.empty()) {
        bool futuro = !(data < hoje());
        if (!futuro && !ds.url_passado.empty()) return ds.url_passado;
        if (futuro && !ds.url_futuro.empty()) return ds.url_futuro;
    }
    return ds.url;
}

static std::string escolher_url(const std::string& lista) {
    auto cands = dividir(lista, '|');
    cands.erase(std::remove_if(cands.begin(), cands.end(), [](const std::string& s) {
        return s.empty();
    }), cands.end());
    if (cands.empty()) return lista;
    if (cands.size() == 1) return cands.front();
    static std::atomic<unsigned> g_rr_url{0};
    unsigned base = g_rr_url.fetch_add(1);
    return cands[base % cands.size()];
}

std::vector<Registro> baixar_dataset_bloco(const DatasetCfg& ds,
                                          const std::vector<std::string>& datas,
                                          const Contexto& ctx_base) {
    std::vector<std::string> alvo;
    for (const auto& d : datas) {
        if (data_no_intervalo(d, ds)) alvo.push_back(d);
    }
    if (ds.iterar_datas && alvo.empty()) return {};
    std::sort(alvo.begin(), alvo.end());
    std::vector<Registro> todos;

    size_t i = 0;
    const size_t n_passos = ds.iterar_datas ? alvo.size() : 1;
    while (i < n_passos) {
        std::string lista = url_do_dataset(ds, alvo.empty() ? hoje() : alvo[i]);
        size_t j = i;
        if (ds.iterar_datas) {
            while (j < alvo.size() && url_do_dataset(ds, alvo[j]) == lista &&
                   (j - i) < static_cast<size_t>(CFG.chunk_dias))
                ++j;
        } else {
            j = 1;
        }
        std::vector<std::string> grupo;
        if (ds.iterar_datas) grupo.assign(alvo.begin() + i, alvo.begin() + j);
        std::string bloco_req = ds.bloco;
        std::string chave_req = ds.params_chave;
        std::vector<std::string> vars = ds.vars;

        // Casos opcionais de exclusão de variáveis em URLs históricas
        if (lista == ds.url_passado && !ds.url_passado.empty() && !ds.vars_excluir.empty()) {
            vars.erase(std::remove_if(vars.begin(), vars.end(),
                                      [&](const std::string& v) {
                                          return ds.vars_excluir.count(v);
                                      }),
                       vars.end());
        }

        double custo = 1.0;
        if (!vars.empty())
            custo = std::max(custo, static_cast<double>(vars.size()) / CFG.custo_vars_ref);
        try {
            if (!grupo.empty()) {
                Data da = para_data(grupo.front());
                Data db = para_data(grupo.back());
                long d1 = dias_de_civil(da.ano, da.mes, da.dia);
                long d2 = dias_de_civil(db.ano, db.mes, db.dia);
                custo = std::max(custo, (d2 - d1 + 1) * CFG.custo_dias_fator / CFG.custo_dias_ref);
            }
        } catch (...) {
            custo = std::max(custo, static_cast<double>(grupo.size()) * CFG.custo_dias_fator / CFG.custo_dias_ref);
        }

        std::string chave_api;
        if (!ds.api_keys.empty()) {
            auto chaves = dividir(ds.api_keys, '|');
            chaves.erase(std::remove_if(chaves.begin(), chaves.end(), [](const std::string& s) {
                return s.empty();
            }), chaves.end());
            if (!chaves.empty()) {
                static std::atomic<unsigned> g_rr_chave{0};
                chave_api = chaves[g_rr_chave.fetch_add(1) % chaves.size()];
            }
        }

        Contexto ctx = ctx_base;
        ctx["start"] = grupo.empty() ? hoje() : grupo.front();
        ctx["end"] = grupo.empty() ? hoje() : grupo.back();
        ctx["start_date"] = ctx["start"];
        ctx["end_date"] = ctx["end"];
        ctx["hoje"] = hoje();
        ctx["timezone"] = CFG.fuso_horario;
        ctx["vars"] = juntar(vars, ",");
        ctx["api_key"] = chave_api;
        ctx["id"] = ds.id;
        ctx["tabela"] = ds.tabela;

        std::map<std::string, std::string> params;
        if (ds.params.empty()) {
            if (ds.usar_coordenadas) {
                auto ilat = ctx.find("latitude");
                auto ilon = ctx.find("longitude");
                if (ilat != ctx.end()) params["latitude"] = ilat->second;
                if (ilon != ctx.end()) params["longitude"] = ilon->second;
            }
            if (ds.iterar_datas && !grupo.empty()) {
                params["start_date"] = grupo.front();
                params["end_date"] = grupo.back();
            }
            if (ds.usar_coordenadas && !CFG.fuso_horario.empty())
                params["timezone"] = CFG.fuso_horario;
        }
        for (const auto& kv : ds.params)
            params[kv.first] = aplicar_ctx(kv.second, ctx);
        if (!vars.empty() && !chave_req.empty() && !params.count(chave_req))
            params[chave_req] = juntar(vars, ",");
        if (!chave_api.empty() && ds.api_key_em == "query")
            params[CFG.api_key_param] = chave_api;

        std::map<std::string, std::string> hdrs;
        for (const auto& kv : ds.headers)
            hdrs[kv.first] = aplicar_ctx(kv.second, ctx);
        if (!chave_api.empty() && ds.api_key_em == "header") {
            std::string pref = ds.api_key_prefix;
            hdrs[ds.api_key_header] = pref.empty() ? chave_api : (pref + " " + chave_api);
        }

        std::string url_req = aplicar_ctx(escolher_url(lista), ctx);
        std::string corpo = aplicar_ctx(ds.corpo, ctx);
        auto dados = baixar_com_retry(url_req, params, CFG.max_retries, custo, chave_api,
                                      ds.metodo, hdrs, corpo, ds.api_key_em);
        if (dados) {
            std::set<std::string> filtro(grupo.begin(), grupo.end());
            const std::set<std::string>* pf = ds.iterar_datas ? &filtro : nullptr;
            auto regs = parse_json_generico(*dados, ds, ctx, pf, false, bloco_req);

            // Se ds.seguir_url_detalhe estiver ativado, cada registro que contenha uma URL de detalhe
            // (ex: pokeapi.co/api/v2/pokemon/1/) eh baixado para enriquecer o registro com o objeto completo
            if (ds.seguir_url_detalhe && !regs.empty()) {
                std::string col_url = ds.campo_url_detalhe.empty() ? "url" : ds.campo_url_detalhe;
                log("[DETALHES] Enriquecendo " + std::to_string(regs.size()) + " registros via " + col_url + "...");
                std::atomic<size_t> num_det{0};
                std::atomic<size_t> idx_det{0};
                int n_det = CFG.download_workers < 1 ? 1 : CFG.download_workers;
                if ((size_t)n_det > regs.size()) n_det = (int)regs.size();
                std::vector<std::thread> th_det;
                th_det.reserve((size_t)n_det);
                for (int w = 0; w < n_det; ++w) {
                    th_det.emplace_back([&, w] {
                        while (true) {
                            size_t k = idx_det.fetch_add(1);
                            if (k >= regs.size()) break;
                            auto it_url = regs[k].find(col_url);
                            if (it_url == regs[k].end()) continue;
                            if (!std::holds_alternative<std::string>(it_url->second)) continue;
                            std::string sub_url = std::get<std::string>(it_url->second);
                            if (sub_url.rfind("http", 0) != 0) continue;
                            auto sub_dados = baixar_com_retry(sub_url, {}, CFG.max_retries, 0.5, chave_api,
                                                              "GET", hdrs, "", ds.api_key_em);
                            if (!sub_dados || !sub_dados->is_object()) continue;
                            auto sub_regs = parse_objeto(*sub_dados, *sub_dados, ds, ctx);
                            if (!sub_regs.empty()) {
                                for (const auto& kv : sub_regs.front()) regs[k][kv.first] = kv.second;
                            }
                            std::string item_nome;
                            auto it_nom = regs[k].find("name");
                            if (it_nom == regs[k].end()) it_nom = regs[k].find("nome");
                            if (it_nom != regs[k].end() && std::holds_alternative<std::string>(it_nom->second))
                                item_nome = std::get<std::string>(it_nom->second);
                            size_t n = num_det.fetch_add(1) + 1;
                            if (n % 10 == 0 || n == regs.size())
                                log("  [DETALHE " + std::to_string(n) + "/" + std::to_string(regs.size()) + "] " + item_nome);
                            if (ds.baixar_todas_imagens) {
                                std::vector<std::string> imgs;
                                extrair_todas_urls_imagens(*sub_dados, imgs);
                                if (!imgs.empty()) {
                                    std::string pasta = resolver_pasta_saida(ds.pasta_imagens, "imagens", "PASTA_IMAGENS");
                                    std::filesystem::path sub_pasta(pasta);
                                    if (!item_nome.empty()) sub_pasta /= sanitizar_nome_arquivo(item_nome);
                                    for (const auto& img_url : imgs) {
                                        std::filesystem::path dest = sub_pasta / obter_nome_arquivo_da_url(img_url);
                                        baixar_arquivo_seguro(img_url, dest.string(), "", ds.tamanho_min_bytes, ds.headers, CFG.http_timeout,
                                                              ds.tamanho_max_bytes, nullptr);
                                    }
                                }
                            }
                        }
                    });
                }
                for (auto& t : th_det) t.join();
            }

            todos.insert(todos.end(), std::make_move_iterator(regs.begin()),
                         std::make_move_iterator(regs.end()));
        }
        i = j;
    }
    return todos;
}

static std::optional<ResultadoGenerico> baixar_bloco_generico(
    size_t dataset_idx, const std::vector<std::string>& datas, const std::string& chave_existentes,
    const Contexto& ctx_tarefa, const ExistentesGen& existentes) {
    std::uniform_real_distribution<double> jitter(0.0, CFG.jitter_inicial_max);
    dormir(jitter(rng()));
    const DatasetCfg& ds = CFG.datasets[dataset_idx];
    std::vector<std::string> faltantes;
    // Datasets sem data: sempre 1 download (idempotencia ja checada na
    // montagem das tarefas via contar_linhas_tabela).
    if (!ds.iterar_datas) {
        ResultadoGenerico r;
        r.dataset_idx = dataset_idx;
        r.chave_existentes = chave_existentes;
        r.ctx = ctx_tarefa;
        r.registros = baixar_dataset_bloco(ds, datas, ctx_tarefa);
        return r;
    }
    {
        std::shared_lock lk(g_mtx_existentes);
        auto it = existentes.find(dataset_idx);
        const ConjuntoDatas* tem = nullptr;
        if (it != existentes.end()) {
            auto jt = it->second.find(chave_existentes);
            if (jt != it->second.end()) tem = &jt->second;
        }
        for (const auto& d : datas) {
            if (!data_no_intervalo(d, ds)) continue;
            if (!tem || !tem->count(d)) faltantes.push_back(d);
        }
    }
    ResultadoGenerico r;
    r.dataset_idx = dataset_idx;
    r.chave_existentes = chave_existentes;
    r.ctx = ctx_tarefa;
    r.datas_pedidas = faltantes;
    if (!faltantes.empty()) {
        r.registros = baixar_dataset_bloco(ds, faltantes, ctx_tarefa);
    }
    return r;
}

static void processar_generico(const ResultadoGenerico& r, ExistentesGen& existentes,
                               std::map<size_t, long long>& baixados,
                               std::map<size_t, long long>& pulados) {
    const DatasetCfg& ds = CFG.datasets[r.dataset_idx];
    // Datasets sem data: "baixados" = nº de registros; sem conta de dias.
    if (!ds.iterar_datas) {
        if (r.registros.empty()) return;
        {
            std::lock_guard<std::mutex> lk(g_write_mtx);
            salvar_dataset(ds, r.registros);
        }
        {
            std::lock_guard<std::mutex> lk(g_stats_mtx);
            baixados[r.dataset_idx] += static_cast<long long>(r.registros.size());
        }
        return;
    }
    long long dias_retornados = 0;
    {
        std::set<std::string> dias;
        std::string col = ds.chave_data.empty() ? "data" : ds.chave_data;
        for (const auto& reg : r.registros) {
            auto it = reg.find(col);
            if (it != reg.end() && std::holds_alternative<std::string>(it->second))
                dias.insert(std::get<std::string>(it->second));
        }
        dias_retornados = static_cast<long long>(dias.size());
    }
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        pulados[r.dataset_idx] += static_cast<long long>(r.datas_pedidas.size()) - dias_retornados;
    }
    if (r.registros.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_write_mtx);
        salvar_dataset(ds, r.registros);
    }

    // Se o dataset tiver baixar_arquivos_vinculados ativado (como imagens de pokemons, assets, etc.)
    // Downloads em paralelo com download_workers (rede).
    if (ds.baixar_arquivos_vinculados && !ds.campo_url_arquivo.empty()) {
        std::string pasta = resolver_pasta_saida(ds.pasta_saida, "arquivos_" + ds.id, "PASTA_SAIDA");
        struct AlvoVinc { std::string url; std::string nome; };
        std::vector<AlvoVinc> alvos;
        alvos.reserve(r.registros.size());
        for (const auto& reg : r.registros) {
            auto it_u = reg.find(ds.campo_url_arquivo);
            if (it_u != reg.end() && std::holds_alternative<std::string>(it_u->second)) {
                std::string file_url = std::get<std::string>(it_u->second);
                if (file_url.rfind("http", 0) == 0) {
                    std::string nome_local;
                    if (!ds.campo_nome_arquivo.empty()) {
                        auto it_n = reg.find(ds.campo_nome_arquivo);
                        if (it_n != reg.end() && std::holds_alternative<std::string>(it_n->second)) {
                            nome_local = sanitizar_nome_arquivo(std::get<std::string>(it_n->second));
                        }
                    }
                    if (nome_local.empty()) {
                        nome_local = obter_nome_arquivo_da_url(file_url);
                    }
                    alvos.push_back({file_url, nome_local});
                }
            }
        }
        int n_v = CFG.download_workers < 1 ? 1 : CFG.download_workers;
        if ((size_t)n_v > alvos.size() && !alvos.empty()) n_v = (int)alvos.size();
        std::atomic<size_t> idx_v{0};
        std::vector<std::thread> th_v;
        th_v.reserve((size_t)std::max(1, n_v));
        for (int w = 0; w < n_v; ++w) {
            th_v.emplace_back([&] {
                while (true) {
                    size_t k = idx_v.fetch_add(1);
                    if (k >= alvos.size()) break;
                    std::filesystem::path dest = std::filesystem::path(pasta) / alvos[k].nome;
                    baixar_arquivo_seguro(alvos[k].url, dest.string(), "", ds.tamanho_min_bytes, ds.headers, CFG.http_timeout,
                                          ds.tamanho_max_bytes, nullptr);
                }
            });
        }
        for (auto& t : th_v) t.join();
    }
    {
        std::unique_lock<std::shared_mutex> lk(g_mtx_existentes);
        auto& conj = existentes[r.dataset_idx][r.chave_existentes];
        std::string col = ds.chave_data.empty() ? "data" : ds.chave_data;
        for (const auto& reg : r.registros) {
            auto it = reg.find(col);
            if (it != reg.end() && std::holds_alternative<std::string>(it->second))
                conj.insert(std::get<std::string>(it->second));
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        baixados[r.dataset_idx] += dias_retornados;
    }
}

int executar_generico(const std::vector<std::string>& datas, double tempo_load,
                      const ExistentesGen& existentes_inicial,
                      size_t dataset_idx) {
    ExistentesGen existentes = existentes_inicial;
    struct TarefaGen {
        size_t ds;
        std::vector<std::string> datas;
        std::string chave_existentes;
        Contexto ctx;
    };
    std::vector<TarefaGen> tarefas;
    long long falt = 0;

    size_t di_inicio = (dataset_idx < CFG.datasets.size()) ? dataset_idx : 0;
    size_t di_fim = (dataset_idx < CFG.datasets.size()) ? (dataset_idx + 1) : CFG.datasets.size();

    for (size_t di = di_inicio; di < di_fim; ++di) {
        const DatasetCfg& ds = CFG.datasets[di];
        if (ds.tipo_saida == "arquivo") continue;

        // Datasets sem data (ex: PokeAPI records): 1 tarefa por contexto.
        // Sem isso, 1940-2025 vira 31412 dias x 1 coord = 1048 blocos
        // identicos baixando a mesma URL repetidamente.
        if (!ds.iterar_datas) {
            auto contextos = expandir_contextos(ds);
            for (const auto& ctx : contextos) {
                if (contar_linhas_tabela(ds) > 0) continue; // idempotente
                tarefas.push_back({di, {}, "", ctx});
                falt += 1;
            }
            continue;
        }

        // Se o dataset usar coordenadas (ou tiver coordenadas como eixo)
        if (ds.usar_coordenadas || !CFG.coordenadas.empty()) {
            std::vector<std::pair<double, double>> lista_coords = CFG.coordenadas;
            if (lista_coords.empty()) {
                lista_coords.push_back({0.0, 0.0});
            }
            for (const auto& coord : lista_coords) {
                std::string ch = num_str(coord.first) + "," + num_str(coord.second);
                std::vector<std::string> f;
                {
                    std::shared_lock lk(g_mtx_existentes);
                    auto it = existentes.find(di);
                    const ConjuntoDatas* tem = nullptr;
                    if (it != existentes.end()) {
                        auto jt = it->second.find(ch);
                        if (jt != it->second.end()) tem = &jt->second;
                    }
                    for (const auto& d : datas) {
                        if (!data_no_intervalo(d, ds)) continue;
                        if (!tem || !tem->count(d)) f.push_back(d);
                    }
                }
                if (f.empty()) continue;
                falt += (long long)f.size();

                Contexto ctx;
                if (ds.usar_coordenadas) {
                    ctx["latitude"] = num_str(coord.first);
                    ctx["longitude"] = num_str(coord.second);
                    ctx["lat"] = ctx["latitude"];
                    ctx["lon"] = ctx["longitude"];
                }

                for (size_t i = 0; i < f.size(); i += (size_t)CFG.chunk_dias) {
                    size_t e = std::min(i + (size_t)CFG.chunk_dias, f.size());
                    tarefas.push_back({di, std::vector<std::string>(f.begin() + i, f.begin() + e),
                                       ch, ctx});
                }
            }
        } else {
            // Expansão por eixos de dimensão arbitrários
            auto contextos = expandir_contextos(ds);
            for (const auto& ctx : contextos) {
                std::string ch;
                for (const auto& kv : ctx) {
                    if (chave_ctx_interna(kv.first)) continue;
                    if (!ch.empty()) ch += "|";
                    ch += kv.first + "=" + kv.second;
                }
                std::vector<std::string> f;
                {
                    std::shared_lock lk(g_mtx_existentes);
                    auto it = existentes.find(di);
                    const ConjuntoDatas* tem = nullptr;
                    if (it != existentes.end()) {
                        auto jt = it->second.find(ch);
                        if (jt != it->second.end()) tem = &jt->second;
                    }
                    for (const auto& d : datas) {
                        if (!data_no_intervalo(d, ds)) continue;
                        if (!tem || !tem->count(d)) f.push_back(d);
                    }
                }
                if (f.empty()) continue;
                falt += (long long)f.size();
                for (size_t i = 0; i < f.size(); i += (size_t)CFG.chunk_dias) {
                    size_t e = std::min(i + (size_t)CFG.chunk_dias, f.size());
                    tarefas.push_back({di, std::vector<std::string>(f.begin() + i, f.begin() + e),
                                       ch, ctx});
                }
            }
        }
    }

    log("[DB] Tarefas: " + std::to_string(tarefas.size()) + " blocos, " +
        std::to_string(falt) + " dia x chave pendentes.");
    if (tarefas.empty()) {
        log("[DB] Nada a fazer.");
        return 0;
    }

    std::map<size_t, long long> baixados, pulados;
    std::atomic<long long> concluidos{0};
    long long total = (long long)tarefas.size();
    auto inicio = std::chrono::steady_clock::now();
    Fila<ResultadoGenerico> fila((size_t)CFG.download_workers * 2);
    std::atomic<size_t> idx_t{0};
    std::atomic<long long> feitas{0};

    log("\nIniciando " + std::to_string(total) + " tarefas (" +
        std::to_string(CFG.download_workers) + " download_workers, " +
        std::to_string(CFG.process_workers) + " process_workers)...\n");

    std::vector<std::thread> consumidores;
    for (int i = 0; i < CFG.process_workers; ++i) {
        consumidores.emplace_back([&] {
            try {
                ResultadoGenerico r;
                while (fila.get(r)) {
                    processar_generico(r, existentes, baixados, pulados);
                    long long c = ++concluidos;
                    if (c % 5 == 0 || c == total) {
                        double pct = 100.0 * (double)c / (double)total;
                        double dec = std::chrono::duration<double>(
                                         std::chrono::steady_clock::now() - inicio)
                                         .count();
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "[PROGRESSO] %lld/%lld blocos (%.1f%%) - %.0fs", c,
                                      total, pct, dec);
                        log(buf);
                    }
                }
            } catch (const std::exception& e) {
                log(std::string("[ERROR] Consumidor: ") + e.what());
            }
        });
    }

    std::vector<std::thread> produtores;
    for (int i = 0; i < CFG.download_workers; ++i) {
        produtores.emplace_back([&] {
            try {
                while (true) {
                    size_t k = idx_t.fetch_add(1);
                    if (k >= tarefas.size()) break;
                    const TarefaGen& t = tarefas[k];
                    auto res = baixar_bloco_generico(t.ds, t.datas, t.chave_existentes, t.ctx, existentes);
                    if (res) fila.put(std::move(*res));
                    if (feitas.fetch_add(1) + 1 >= total) fila.close();
                }
            } catch (const std::exception& e) {
                log(std::string("[ERROR] Produtor: ") + e.what());
                if (feitas.fetch_add(1) + 1 >= total) fila.close();
            }
        });
    }

    for (auto& t : produtores) t.join();
    for (auto& t : consumidores) t.join();

    double tt = std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();
    char tt_buf[32];
    std::snprintf(tt_buf, sizeof(tt_buf), "%.1f", tt);
    log("\n\n============================================================");
    log("DOWNLOAD CONCLUIDO: " + CFG.datasets[di_inicio].id);
    log("============================================================");
    log("Tempo: " + std::string(tt_buf) + "s");
    log("Blocos processados: " + std::to_string(concluidos.load()) + "/" +
        std::to_string(total) + "\n--- RESUMO ---");
    for (size_t di = di_inicio; di < di_fim; ++di) {
        if (CFG.datasets[di].tipo_saida == "arquivo") continue;
        log(CFG.datasets[di].id + " (" + CFG.datasets[di].db + "." +
            CFG.datasets[di].tabela + "): " + std::to_string(baixados[di]) +
            " registros novos | " + std::to_string(pulados[di]) + " ja existentes");
    }
    log("============================================================");
    return 0;
}
