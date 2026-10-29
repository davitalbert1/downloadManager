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
#include <mutex>
#include <random>
#include <thread>

static std::mutex g_mtx_existentes;
static std::mutex g_write_mtx;
static std::mutex g_stats_mtx;

static std::string url_do_dataset(const DatasetCfg& ds, const std::string& data) {
    if (!ds.url_passado.empty() || !ds.url_futuro.empty()) {
        bool futuro = !(data < hoje());
        if (!futuro && !ds.url_passado.empty()) return ds.url_passado;
        if (futuro && !ds.url_futuro.empty()) return ds.url_futuro;
    }
    return ds.url;
}

static std::vector<std::string> urls_candidatas(const std::string& lista, const Contexto& ctx) {
    std::vector<std::string> out;
    for (auto& parte : dividir(lista, '|')) {
        std::string u = cortar(aplicar_ctx(parte, ctx));
        if (u.empty()) continue;
        if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
    }
    if (out.empty()) {
        std::string u = cortar(aplicar_ctx(lista, ctx));
        if (!u.empty()) out.push_back(u);
    }
    return out;
}

static std::vector<std::string> chaves_candidatas(const std::string& lista) {
    std::vector<std::string> out;
    for (auto& parte : dividir(lista, '|')) {
        std::string k = cortar(parte);
        if (k.empty()) continue;
        if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    }
    return out;
}

static const DatasetCfg* dataset_por_id(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& d : CFG.datasets)
        if (d.id == id) return &d;
    return nullptr;
}

std::vector<Registro> baixar_dataset_bloco(const DatasetCfg& ds,
                                          const std::vector<std::string>& datas,
                                          const Contexto& ctx_base,
                                          int profundidade) {
    std::vector<std::string> alvo;
    for (const auto& d : datas) {
        if (data_no_intervalo(d, ds)) alvo.push_back(d);
    }
    if (ds.iterar_datas && alvo.empty()) return {};
    std::sort(alvo.begin(), alvo.end());
    std::vector<Registro> todos;
    const AuthApi auth = auth_do_dataset(ds);
    const size_t chunk = (size_t)std::max(1, CFG.chunk_dias);

    size_t i = 0;
    const size_t n_passos = ds.iterar_datas ? alvo.size() : 1;
    while (i < n_passos) {
        std::string lista = url_do_dataset(ds, alvo.empty() ? hoje() : alvo[i]);
        size_t j = i;
        if (ds.iterar_datas) {
            while (j < alvo.size() && url_do_dataset(ds, alvo[j]) == lista &&
                   (j - i) < chunk)
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

        std::vector<std::string> chaves = chaves_candidatas(ds.api_keys);

        Contexto ctx = ctx_base;
        ctx["start"] = grupo.empty() ? hoje() : grupo.front();
        ctx["end"] = grupo.empty() ? hoje() : grupo.back();
        ctx["start_date"] = ctx["start"];
        ctx["end_date"] = ctx["end"];
        ctx["hoje"] = hoje();
        ctx["timezone"] = CFG.fuso_horario;
        ctx["vars"] = juntar(vars, ",");
        ctx["api_key"] = chaves.empty() ? std::string() : chaves.front();
        ctx["id"] = ds.id;
        ctx["tabela"] = ds.tabela;

        // Parâmetros automáticos entram sempre; param.* do INI sobrescreve
        // quando usar o mesmo nome.
        std::map<std::string, std::string> params;
        if (ds.usar_coordenadas) {
            auto ilat = ctx.find("latitude");
            auto ilon = ctx.find("longitude");
            if (ilat != ctx.end()) params["latitude"] = ilat->second;
            if (ilon != ctx.end()) params["longitude"] = ilon->second;
            if (!CFG.fuso_horario.empty()) params["timezone"] = CFG.fuso_horario;
        }
        if (ds.iterar_datas && !grupo.empty()) {
            params["start_date"] = grupo.front();
            params["end_date"] = grupo.back();
        }
        for (const auto& kv : ds.params)
            params[kv.first] = aplicar_ctx(kv.second, ctx);
        if (!vars.empty() && !chave_req.empty() && !params.count(chave_req))
            params[chave_req] = juntar(vars, ",");

        std::map<std::string, std::string> hdrs;
        for (const auto& kv : ds.headers)
            hdrs[kv.first] = aplicar_ctx(kv.second, ctx);

        OpcoesRetry req;
        req.urls = urls_candidatas(lista, ctx);
        req.chaves = chaves;
        req.params = params;
        req.max_tentativas = CFG.max_retries;
        req.custo = custo;
        req.metodo = ds.metodo;
        req.headers = hdrs;
        req.corpo = aplicar_ctx(ds.corpo, ctx);
        req.auth = auth;
        // Com fallback configurado, nao vale a pena esperar horas pelo reset
        // da cota: falha cedo e deixa o provedor alternativo assumir. O
        // proprio fallback tambem nao espera horas (cota pequena): se ele
        // limitar, o bloco e apenas pulado.
        req.permitir_espera_longa = ds.fallback.empty() && !ds.so_fallback;
        bool esgotou_429 = false;
        req.esgotou_429 = &esgotou_429;
        auto dados = baixar_com_retry(req);

        if (!dados && esgotou_429 && !ds.fallback.empty() && profundidade < 2 &&
            ds.iterar_datas && !grupo.empty()) {
            const DatasetCfg* fb = dataset_por_id(ds.fallback);
            if (fb && fb != &ds) {
                log("[FALLBACK] " + ds.id + " -> " + fb->id + " para " +
                    grupo.front() + ".." + grupo.back() + " (429/cota)");
                Contexto ctx_fb = ctx_base;
                ctx_fb["fonte"] = fb->id;
                auto regs_fb = baixar_dataset_bloco(*fb, grupo, ctx_fb, profundidade + 1);
                todos.insert(todos.end(), std::make_move_iterator(regs_fb.begin()),
                             std::make_move_iterator(regs_fb.end()));
            } else {
                log("[FALLBACK] Dataset nao encontrado: " + ds.fallback);
            }
        }
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
                executar_paralelo(CFG.download_workers, regs.size(), [&](size_t k) {
                    auto it_url = regs[k].find(col_url);
                    if (it_url == regs[k].end()) return;
                    if (!std::holds_alternative<std::string>(it_url->second)) return;
                    std::string sub_url = std::get<std::string>(it_url->second);
                    if (sub_url.rfind("http", 0) != 0) return;
                    OpcoesRetry sub_req;
                    sub_req.urls = {sub_url};
                    sub_req.chaves = chaves;
                    sub_req.max_tentativas = CFG.max_retries;
                    sub_req.custo = 0.5;
                    sub_req.metodo = "GET";
                    sub_req.headers = hdrs;
                    sub_req.auth = auth;
                    auto sub_dados = baixar_com_retry(sub_req);
                    if (!sub_dados || !sub_dados->is_object()) return;
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
                        log("[DETALHE " + std::to_string(n) + "/" + std::to_string(regs.size()) + "] " + item_nome);
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
                });
            }

            todos.insert(todos.end(), std::make_move_iterator(regs.begin()),
                         std::make_move_iterator(regs.end()));
        }
        i = j;
    }
    return todos;
}

static ResultadoGenerico baixar_bloco_generico(
    size_t dataset_idx, const std::vector<std::string>& datas, const std::string& chave_existentes,
    const Contexto& ctx_tarefa) {
    std::uniform_real_distribution<double> jitter(0.0, CFG.jitter_inicial_max);
    dormir(jitter(rng_thread_local()));
    const DatasetCfg& ds = CFG.datasets[dataset_idx];
    ResultadoGenerico r;
    r.dataset_idx = dataset_idx;
    r.chave_existentes = chave_existentes;
    r.ctx = ctx_tarefa;
    r.datas_pedidas = datas;
    r.registros = baixar_dataset_bloco(ds, datas, ctx_tarefa);
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
        pulados[r.dataset_idx] += std::max(0LL, static_cast<long long>(r.datas_pedidas.size()) -
                                                    dias_retornados);
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
        struct AlvoVinc {
            std::string url;
            std::string nome;
        };
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
        executar_paralelo(CFG.download_workers, alvos.size(), [&](size_t k) {
            std::filesystem::path dest = std::filesystem::path(pasta) / alvos[k].nome;
            baixar_arquivo_seguro(alvos[k].url, dest.string(), "", ds.tamanho_min_bytes, ds.headers, CFG.http_timeout,
                                  ds.tamanho_max_bytes, nullptr);
        });
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx_existentes);
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

int executar_generico(const std::vector<std::string>& datas,
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

    // Chave = coordenada "lat,lon" ou combinação de eixos "k=v|k=v" (mesma
    // usada por storage_sqlite ao carregar o que já existe).
    auto adicionar_tarefas = [&](size_t di, const Contexto& ctx,
                                 const std::vector<std::string>& pendentes) {
        std::string chave = chave_contexto(ctx);
        const ConjuntoDatas* tem = nullptr;
        auto it = existentes.find(di);
        if (it != existentes.end()) {
            auto jt = it->second.find(chave);
            if (jt != it->second.end()) tem = &jt->second;
        }
        std::vector<std::string> f;
        f.reserve(pendentes.size());
        for (const auto& d : pendentes)
            if (!tem || !tem->count(d)) f.push_back(d);
        if (f.empty()) return;
        falt += (long long)f.size();
        const size_t chunk = (size_t)std::max(1, CFG.chunk_dias);
        for (size_t i = 0; i < f.size(); i += chunk) {
            size_t e = std::min(i + chunk, f.size());
            tarefas.push_back({di, std::vector<std::string>(f.begin() + i, f.begin() + e), chave, ctx});
        }
    };

    for (size_t di = di_inicio; di < di_fim; ++di) {
        const DatasetCfg& ds = CFG.datasets[di];
        if (!dataset_generico(ds) || ds.so_fallback) continue;

        // Datasets sem data (ex: PokeAPI records): 1 tarefa por contexto.
        // Sem isso, 1940-2025 vira 31412 dias x 1 coord = 1048 blocos
        // identicos baixando a mesma URL repetidamente.
        if (!ds.iterar_datas) {
            if (contar_linhas_tabela(ds) > 0) continue; // idempotente
            for (const auto& ctx : contextos_de_download(ds)) {
                tarefas.push_back({di, {}, chave_contexto(ctx), ctx});
                falt += 1;
            }
            continue;
        }

        std::vector<std::string> pendentes;
        pendentes.reserve(datas.size());
        for (const auto& d : datas)
            if (data_no_intervalo(d, ds)) pendentes.push_back(d);
        if (pendentes.empty()) continue;

        for (const auto& ctx : contextos_de_download(ds))
            adicionar_tarefas(di, ctx, pendentes);
    }

    log("[DB] Tarefas: " + std::to_string(tarefas.size()) + " blocos, " +
        std::to_string(falt) + " dia x chave pendentes.");
    if (tarefas.empty()) {
        log("[DB] Nada a fazer.");
        return 0;
    }

    std::map<size_t, long long> baixados, pulados;
    std::atomic<long long> concluidos{0};
    std::atomic<long long> falhas{0};
    long long total = (long long)tarefas.size();
    auto inicio = std::chrono::steady_clock::now();
    const int n_download = std::max(1, CFG.download_workers);
    const int n_process = std::max(1, CFG.process_workers);
    Fila<ResultadoGenerico> fila((size_t)n_download * 2);
    std::atomic<size_t> idx_t{0};
    std::atomic<long long> feitas{0};

    log("\nIniciando " + std::to_string(total) + " tarefas (" +
        std::to_string(n_download) + " download_workers, " +
        std::to_string(n_process) + " process_workers)...\n");

    std::vector<std::thread> consumidores;
    for (int i = 0; i < n_process; ++i) {
        consumidores.emplace_back([&] {
            try {
                ResultadoGenerico r;
                while (fila.get(r)) {
                    try {
                        processar_generico(r, existentes, baixados, pulados);
                    } catch (const std::exception& e) {
                        ++falhas;
                        log(std::string("[ERROR] Falha ao processar bloco: ") + e.what());
                    }
                    long long c = ++concluidos;
                    if (c % 5 == 0 || c == total) {
                        double pct = 100.0 * (double)c / (double)total;
                        double dec = std::chrono::duration<double>(
                                         std::chrono::steady_clock::now() - inicio)
                                         .count();
                        double eta = c > 0 ? dec * (double)(total - c) / (double)c : 0.0;
                        std::string dec_txt = formatar_duracao(dec);
                        std::string eta_txt = formatar_duracao(eta);
                        char buf[220];
                        std::snprintf(buf, sizeof(buf),
                                      "[PROGRESSO] %lld/%lld blocos (%.1f%%) - %s decorridos - ETA %s",
                                      c, total, pct, dec_txt.c_str(), eta_txt.c_str());
                        log(buf);
                    }
                }
            } catch (const std::exception& e) {
                ++falhas;
                log(std::string("[ERROR] Consumidor: ") + e.what());
            }
        });
    }

    std::vector<std::thread> produtores;
    for (int i = 0; i < n_download; ++i) {
        produtores.emplace_back([&] {
            try {
                while (true) {
                    size_t k = idx_t.fetch_add(1);
                    if (k >= tarefas.size()) break;
                    const TarefaGen& t = tarefas[k];
                    fila.put(baixar_bloco_generico(t.ds, t.datas, t.chave_existentes, t.ctx));
                    if (feitas.fetch_add(1) + 1 >= total) fila.close();
                }
            } catch (const std::exception& e) {
                ++falhas;
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
        if (!dataset_generico(CFG.datasets[di]) || CFG.datasets[di].so_fallback) continue;
        log(CFG.datasets[di].id + " (" + CFG.datasets[di].db + "." +
            CFG.datasets[di].tabela + "): " + std::to_string(baixados[di]) +
            " registros novos | " + std::to_string(pulados[di]) + " ja existentes");
    }
    log("============================================================");
    if (falhas.load() > 0) log("Falhas: " + std::to_string(falhas.load()));
    return falhas.load() > 0 ? 1 : 0;
}
