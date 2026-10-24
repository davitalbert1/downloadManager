#include "file_dataset.h"
#include "grib_importer.h"
#include "http_client.h"
#include "json_parser.h"
#include "rate_limit.h"
#include "storage_sqlite.h"
#include "utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using json = nlohmann::json;

static RespostaHttp requisicao_api(const std::string& url, const AuthApi& auth,
                                   const std::string& api_key,
                                   const std::string& metodo = "GET",
                                   const std::string& corpo = "",
                                   double timeout_sec = 60.0) {
    OpcoesHttp op;
    op.metodo = metodo;
    op.auth = &auth;
    op.chave_api = api_key;
    op.timeout_sec = timeout_sec;
    op.headers["Accept"] = "application/json";
    op.corpo = corpo;
    return http_requisicao(url, op);
}

static std::string sanitizar_url_download(std::string url) {
    url = cortar(url);
    if (url.empty()) return "";

    size_t corte = url.find("%22");
    if (corte == std::string::npos) corte = url.find('"');
    if (corte != std::string::npos) {
        if (corte == 0) return "";
        url.resize(corte);
    }
    return url;
}

static std::string extrair_url_download_bruto(const json& j) {
    if (j.contains("asset") && j["asset"].is_object()) {
        const auto& a = j["asset"];
        if (a.contains("value") && a["value"].is_object()) {
            const auto& v = a["value"];
            if (v.contains("href") && v["href"].is_string()) return v["href"].get<std::string>();
        }
        if (a.contains("href") && a["href"].is_string()) return a["href"].get<std::string>();
    }
    if (j.contains("location") && j["location"].is_string()) {
        return j["location"].get<std::string>();
    }
    if (j.contains("href") && j["href"].is_string()) return j["href"].get<std::string>();
    if (j.contains("url") && j["url"].is_string()) return j["url"].get<std::string>();
    if (j.contains("download_url") && j["download_url"].is_string()) {
        return j["download_url"].get<std::string>();
    }
    if (j.contains("result") && j["result"].is_object()) return extrair_url_download_bruto(j["result"]);
    return "";
}

static std::string extrair_url_download(const json& j) {
    return sanitizar_url_download(extrair_url_download_bruto(j));
}

static bool http_download_stream(const std::string& url_entrada,
                                 const std::string& caminho_destino,
                                 const AuthApi& auth,
                                 const std::string& api_key,
                                 const std::string& tag,
                                 double timeout_sec = 1800.0,
                                 long long tamanho_max = 0,
                                 bool* pulado_por_tamanho = nullptr) {
    OpcoesDownload op;
    op.url = url_entrada;
    op.caminho = caminho_destino;
    op.tag = tag;
    op.auth = &auth;
    op.chave_api = api_key;
    op.timeout_sec = timeout_sec;
    op.tamanho_max = tamanho_max;
    op.detectar_pagina_erro = true;
    op.pulado_por_tamanho = pulado_por_tamanho;

    ResultadoDownload r = http_baixar_arquivo(op);
    if (r.ok) return true;
    if (r.pulado_tamanho) return false;
    if (r.pagina_erro) {
        log("[ERRO] Servidor retornou mensagem de erro para " + tag + ": " +
            r.corpo_erro.substr(0, 150));
    } else {
        log("[ERRO] Download HTTP " + std::to_string(r.status) + " para " + tag +
            (r.erro.empty() ? "" : " (" + r.erro + ")"));
    }
    return false;
}

static std::string extrair_asset_de_arquivo(const std::filesystem::path& arq) {
    std::error_code ec;
    unsigned long long tam = std::filesystem::file_size(arq, ec);
    if (ec || tam == 0 || tam > 1024ULL * 1024ULL) return "";

    std::ifstream in(arq, std::ios::binary);
    if (!in) return "";
    std::string conteudo;
    conteudo.resize(static_cast<size_t>(tam));
    in.read(conteudo.data(), static_cast<std::streamsize>(tam));
    conteudo.resize(static_cast<size_t>(in.gcount()));

    size_t i = conteudo.find_first_not_of(" \t\r\n");
    if (i == std::string::npos || conteudo[i] != '{') return "";

    try {
        json j = json::parse(conteudo);
        return extrair_url_download(j);
    } catch (...) {
        return "";
    }
}

static bool texto_contem_algum(const std::string& texto,
                               const std::vector<std::string>& trechos) {
    for (const auto& t : trechos)
        if (!t.empty() && texto.find(t) != std::string::npos) return true;
    return false;
}

static bool status_na_lista(const std::string& status,
                            const std::vector<std::string>& lista) {
    return std::find(lista.begin(), lista.end(), status) != lista.end();
}

static std::string valor_do_campo(const json& j, const std::string& campo) {
    if (campo.empty() || !j.is_object()) return "";
    auto it = j.find(campo);
    if (it == j.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}

static const json* lista_de_jobs(const json& j, const std::string& campo) {
    if (j.is_array()) return &j;
    if (!j.is_object()) return nullptr;
    for (const char* chave : {campo.c_str(), "jobs", "data", "results"}) {
        if (!chave || !*chave) continue;
        auto it = j.find(chave);
        if (it != j.end() && it->is_array()) return &(*it);
    }
    return nullptr;
}

static std::string extrair_id_job(const json& j, const std::string& campo) {
    std::string v = valor_do_campo(j, campo);
    if (!v.empty()) return v;
    for (const char* k : {"id", "job_id", "jobId", "request_id"}) {
        v = valor_do_campo(j, k);
        if (!v.empty()) return v;
    }
    return "";
}

static std::string criado_em(const json& job) {
    for (const char* k : {"created", "created_at", "creation_time", "creationTime",
                          "started", "start_time", "submitted"}) {
        auto it = job.find(k);
        if (it != job.end() && it->is_string()) return it->get<std::string>();
    }
    return "";
}

static std::string url_do_job(const std::string& modelo, const DatasetCfg& ds,
                              const std::string& base, const std::string& job_id) {
    if (modelo.empty()) return "";
    Contexto ctx;
    ctx["base"] = base;
    ctx["dataset_api"] = ds.dataset_api;
    ctx["dataset"] = ds.dataset_api;
    ctx["job_id"] = job_id;
    std::string url = cortar(aplicar_ctx(modelo, ctx));
    if (url.empty()) return "";
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        if (url.front() != '/') url = "/" + url;
        url = base + url;
    }
    return url;
}

static std::string motivo_rejeicao_job(const DatasetCfg& ds, const AuthApi& auth,
                                       const std::string& base, const std::string& api_key,
                                       const std::string& job_id) {
    std::string url = url_do_job(ds.job_resultado, ds, base, job_id);
    if (url.empty()) return "";
    RespostaHttp r = requisicao_api(url, auth, api_key);
    if (r.erro_rede || r.corpo.empty()) return "";
    std::string msg;
    try {
        json j = json::parse(r.corpo);
        for (const char* k : {"traceback", "detail", "title", "message", "reason", "error"}) {
            auto it = j.find(k);
            if (it != j.end() && it->is_string()) {
                msg = cortar(it->get<std::string>());
                if (!msg.empty()) break;
            }
        }
        if (msg.empty()) msg = cortar(r.corpo);
    } catch (...) {
        msg = cortar(r.corpo);
    }
    if (msg.size() > 800) msg.resize(800);
    return msg;
}

// GRIB1, secao 1: ano do seculo, mes e seculo ficam em offsets fixos.
static bool decodificar_data_grib(const unsigned char* b, size_t n, int& ano, int& mes) {
    if (n < 64 || b[0] != 'G' || b[1] != 'R' || b[2] != 'I' || b[3] != 'B' || b[7] != 1)
        return false;
    int seculo = b[32];
    if (seculo < 1 || seculo > 30) return false;
    int a = (seculo - 1) * 100 + b[20];
    int m = b[21];
    if (a < 1900 || a > 2100 || m < 1 || m > 12) return false;
    ano = a;
    mes = m;
    return true;
}

static bool ler_data_grib_local(const std::filesystem::path& arquivo, int& ano, int& mes) {
    std::ifstream in(arquivo, std::ios::binary);
    if (!in) return false;
    unsigned char b[64] = {0};
    in.read(reinterpret_cast<char*>(b), sizeof(b));
    return in.gcount() == static_cast<std::streamsize>(sizeof(b)) &&
           decodificar_data_grib(b, sizeof(b), ano, mes);
}

static bool ler_data_grib_remota(const std::string& url_entrada, const AuthApi& auth,
                                 const std::string& api_key, int& ano, int& mes) {
    OpcoesHttp op;
    op.auth = &auth;
    op.chave_api = api_key;
    op.range = "0-63";
    op.timeout_sec = 30.0;
    RespostaHttp r = http_requisicao(url_entrada, op);
    if (r.erro_rede || r.status < 200 || r.status >= 300 || r.corpo.size() < 64) return false;
    return decodificar_data_grib(
        reinterpret_cast<const unsigned char*>(r.corpo.data()), r.corpo.size(), ano, mes);
}

// Reaproveita o job ativo mais recente (mantém a posição já conquistada na
// fila) e cancela apenas duplicados antigos, conforme a configuração.
static std::string obter_job_ativo(const DatasetCfg& ds, const AuthApi& auth,
                                   const std::string& base, const std::string& api_key,
                                   const std::string& tag) {
    std::string url = url_do_job(ds.job_listar_ativos, ds, base, "");
    if (url.empty()) return "";
    RespostaHttp r = requisicao_api(url, auth, api_key);
    if (r.erro_rede || r.status < 200 || r.status >= 300) return "";

    json j;
    try {
        j = json::parse(r.corpo);
    } catch (...) {
        return "";
    }
    const json* lista = lista_de_jobs(j, ds.job_lista_campo);
    if (!lista) return "";

    struct JobAtivo {
        std::string id;
        std::string created;
    };
    std::vector<JobAtivo> ativos;
    for (const auto& job : *lista) {
        if (!job.is_object()) continue;
        if (!ds.job_campo_dataset.empty() && !ds.dataset_api.empty() &&
            valor_do_campo(job, ds.job_campo_dataset) != ds.dataset_api)
            continue;
        std::string id = extrair_id_job(job, ds.job_campo_id);
        if (id.empty()) continue;
        ativos.push_back({id, criado_em(job)});
    }
    if (ativos.empty()) return "";

    size_t manter = 0;
    for (size_t i = 1; i < ativos.size(); ++i)
        if (ativos[i].created > ativos[manter].created) manter = i;

    if (ds.job_cancelar_duplicados) {
        for (size_t i = 0; i < ativos.size(); ++i) {
            if (i == manter) continue;
            std::string url_job = url_do_job(ds.job_status, ds, base, ativos[i].id);
            if (url_job.empty()) continue;
            RespostaHttp d = requisicao_api(url_job, auth, api_key, "DELETE");
            if (!d.erro_rede && d.status >= 200 && d.status < 300)
                log("[CANCELADO] " + tag + ": job duplicado " + ativos[i].id.substr(0, 8));
            else
                log("[AVISO] " + tag + ": nao conseguiu cancelar o job duplicado " +
                    ativos[i].id.substr(0, 8));
        }
    }
    return ativos[manter].id;
}

static std::string nome_arquivo_mensal(const DatasetCfg& ds, int ano, int mes,
                                       const std::string& extensao) {
    if (!ds.padrao_arquivo.empty()) {
        Contexto fctx;
        fctx["id"] = ds.id;
        fctx["ano"] = std::to_string(ano);
        char mes_buf[8];
        std::snprintf(mes_buf, sizeof(mes_buf), "%02d", mes);
        fctx["mes"] = mes_buf;
        fctx["extensao"] = extensao;
        return aplicar_ctx(ds.padrao_arquivo, fctx);
    }
    char fn[256];
    std::snprintf(fn, sizeof(fn), "%s_%04d_%02d.%s", ds.id.c_str(), ano, mes, extensao.c_str());
    return fn;
}

// Processa um mês de um dataset cujo resultado é um arquivo. Pode criar um job
// assíncrono (endpoints em job_*) ou baixar direto de url_pronta.
static bool processar_mes_arquivo(const DatasetCfg& ds, int ano, int mes,
                                  const std::string& base_url, const std::string& api_key,
                                  const std::string& url_pronta = "") {
    AuthApi auth = auth_do_dataset(ds);
    // pasta_saida aceita path absoluto (C:\..., /..., \\server\...) ou relativo.
    std::string pasta_saida_str = resolver_pasta_saida(ds.pasta_saida, "dados_arquivos", "PASTA_SAIDA");
    std::filesystem::path pasta_saida(pasta_saida_str);
    std::error_code ec_dir;
    std::filesystem::create_directories(pasta_saida, ec_dir);

    std::string extensao = obter_env_ou_config("FORMATO", ds.extensao, "bin");
    if (extensao == "netcdf") extensao = "nc";

    std::string nome_final = nome_arquivo_mensal(ds, ano, mes, extensao);
    std::filesystem::path destino = pasta_saida / nome_final;

    std::string db_alvo = ds.db.empty() ? CFG.db_path : ds.db;
    std::string tab_alvo = ds.tabela.empty() ? "clima_horario" : ds.tabela;

    // 0) Se configurado para evitar download quando já estiver no banco, checa diretamente no SQLite
    if (ds.evitar_download_se_no_banco && !db_alvo.empty()) {
        sqlite3* conn = nullptr;
        try {
            conn = get_connection_para(db_alvo);
        } catch (...) {
            conn = nullptr;
        }

        if (conn) {
            std::vector<std::string> hrs = ds.horarios;
            if (hrs.empty()) {
                std::string h_str = obter_env_ou_config("HORARIOS", "", "00:00,12:00");
                hrs = dividir(h_str, ',');
            }
            if (grib_mes_ja_no_banco(conn, tab_alvo, ano, mes, CFG.coordenadas, hrs)) {
                log("[NO BANCO] " + std::string(nome_final) + " ja importado em " + db_alvo + "." + tab_alvo + " - pulando download.");
                return true;
            }
        }
    }

    if (std::filesystem::exists(destino)) {
        if (std::filesystem::file_size(destino) > 100000) {
            log("[JÁ EXISTE] " + std::string(nome_final));
            // Se o arquivo existe em disco mas os dados ainda não foram pro banco e auto_importar_banco está ligado:
            if (ds.auto_importar_banco && (extensao == "grib" || extensao == "grb")) {
                GribImportConfig imp_cfg;
                imp_cfg.db_path = db_alvo;
                imp_cfg.tabela = tab_alvo;
                imp_cfg.coordenadas = CFG.coordenadas;
                imp_cfg.process_workers = 1; // import único inline; paralelismo é no nível de meses
                importar_arquivo_grib(destino.string(), imp_cfg);
            }
            return true;
        } else {
            log("[REMOVENDO ARQUIVO INVÁLIDO/INCOMPLETO] " + std::string(nome_final));
            std::filesystem::remove(destino);
        }
    }

    int qtd_dias = dias_no_mes(ano, mes);

    std::vector<std::string> lista_dias;
    for (int d = 1; d <= qtd_dias; ++d) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d", d);
        lista_dias.push_back(buf);
    }

    std::vector<std::string> horarios = ds.horarios;
    if (horarios.empty()) {
        std::string h_str = obter_env_ou_config("HORARIOS", "", "00:00,12:00");
        horarios = dividir(h_str, ',');
    }

    char tag[64];
    std::snprintf(tag, sizeof(tag), "%04d-%02d", ano, mes);
    if (url_pronta.empty()) log("[SOLICITANDO] " + std::string(tag));

    std::string endpoint = cortar(base_url);
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    if (endpoint.empty()) {
        log("[ERRO] " + std::string(tag) + ": url base nao configurada");
        return false;
    }

    std::string download_url = url_pronta;
    if (download_url.empty()) {
        std::string corpo = ds.corpo;
        if (corpo.empty()) {
            corpo = "{}";
        } else {
            char ano_str[16], mes_str[16];
            std::snprintf(ano_str, sizeof(ano_str), "%04d", ano);
            std::snprintf(mes_str, sizeof(mes_str), "%02d", mes);
            Contexto ctx;
            ctx["ano"] = ano_str;
            ctx["mes"] = mes_str;
            ctx["dataset_api"] = ds.dataset_api;
            ctx["extensao"] = extensao;
            ctx["product_type"] = ds.product_type;
            ctx["data_format"] = ds.data_format;
            ctx["download_format"] = ds.download_format;
            ctx["vars_json"] = json(ds.vars).dump();
            ctx["dias_json"] = json(lista_dias).dump();
            ctx["horarios_json"] = json(horarios).dump();
            corpo = aplicar_ctx(corpo, ctx);
        }

        std::string url_executar = ds.job_executar.empty()
                                       ? endpoint
                                       : url_do_job(ds.job_executar, ds, endpoint, "");
        if (url_executar.empty()) {
            log("[ERRO] " + std::string(tag) + ": job_executar invalido");
            return false;
        }

        // Reaproveita o job que ja esta na fila em vez de cancelar e
        // reiniciar a espera a cada execucao.
        std::string job_id = obter_job_ativo(ds, auth, endpoint, api_key, tag);
        if (!job_id.empty())
            log("[FILA] " + std::string(tag) + ": reaproveitando job " + job_id.substr(0, 8) +
                " ja aceito (sem reiniciar a espera)");

        bool reenviar = false;
        do {
            reenviar = false;
            if (job_id.empty()) {
                while (true) {
                    RespostaHttp resp = requisicao_api(url_executar, auth, api_key, "POST", corpo);

                    if (resp.erro_rede || resp.status == 429 || resp.status >= 500 ||
                        texto_contem_algum(minusculas(resp.corpo), ds.job_reenviar_se)) {
                        std::string motivo = resp.erro_rede ? resp.erro_mensagem
                                                            : ("HTTP " + std::to_string(resp.status));
                        log("[JOB] " + std::string(tag) + ": instabilidade (" + motivo +
                            "); nova tentativa em 60s");
                        dormir(60.0);
                        continue;
                    }
                    if (resp.status < 200 || resp.status >= 300) {
                        std::string erro = cortar(resp.corpo);
                        if (erro.size() > 300) erro.resize(300);
                        log("[ERRO] " + std::string(tag) + ": HTTP " + std::to_string(resp.status) +
                            " - " + erro);
                        return false;
                    }

                    try {
                        json jresp = json::parse(resp.corpo);
                        download_url = extrair_url_download(jresp);
                        if (download_url.empty()) job_id = extrair_id_job(jresp, ds.job_campo_id);
                    } catch (...) {
                        log("[ERRO] " + std::string(tag) + ": resposta invalida da API");
                        return false;
                    }
                    if (job_id.empty() && download_url.empty()) {
                        log("[ERRO] " + std::string(tag) + ": API nao devolveu id do job");
                        return false;
                    }
                    break;
                }
                if (!download_url.empty()) break;
                if (!job_id.empty())
                    log("[JOB] " + std::string(tag) + ": job " + job_id.substr(0, 8) +
                        " enviado; aguardando processamento (pode levar horas)");
            }

            std::string url_status = url_do_job(ds.job_status, ds, endpoint, job_id);
            if (url_status.empty()) {
                log("[ERRO] " + std::string(tag) + ": job_status nao configurado");
                return false;
            }
            std::vector<std::string> status_ok = ds.job_status_ok;
            if (status_ok.empty())
                status_ok = {"successful", "completed", "succeeded", "finished"};
            std::vector<std::string> status_erro = ds.job_status_erro;
            if (status_erro.empty())
                status_erro = {"rejected", "failed", "dismissed", "cancelled", "canceled", "error"};

            auto inicio = std::chrono::steady_clock::now();
            auto ultimo_log = inicio;
            std::string status_anterior;
            json ultimo_json;
            int falhas_seguidas = 0;

            while (true) {
                dormir(std::max(1.0, ds.job_poll_intervalo));
                RespostaHttp sresp = requisicao_api(url_status, auth, api_key);
                if (sresp.erro_rede || sresp.status < 200 || sresp.status >= 300) {
                    if (++falhas_seguidas == 1 || falhas_seguidas % 10 == 0) {
                        std::string motivo = sresp.erro_rede
                                                 ? sresp.erro_mensagem
                                                 : ("HTTP " + std::to_string(sresp.status));
                        log("[JOB] " + std::string(tag) + ": falha ao consultar status (" +
                            motivo + "); tentando de novo");
                    }
                    continue;
                }
                falhas_seguidas = 0;

                std::string status;
                try {
                    ultimo_json = json::parse(sresp.corpo);
                    status = minusculas(valor_do_campo(ultimo_json, ds.job_campo_status));
                    if (status.empty()) status = minusculas(valor_do_campo(ultimo_json, "state"));
                } catch (...) {
                    continue;
                }
                if (status.empty()) continue;

                auto agora = std::chrono::steady_clock::now();
                double decorrido = std::chrono::duration<double>(agora - inicio).count();
                // Log so na mudanca de status e em um heartbeat por minuto para
                // nao parecer um loop enquanto o servidor mantem o job na fila.
                if (status != status_anterior) {
                    std::string espera;
                    if (!status_na_lista(status, status_ok) &&
                        !status_na_lista(status, status_erro))
                        espera = " - aguardando o servidor processar (pode levar horas; "
                                 "nao cancele, senao perde a posicao na fila)";
                    log("[JOB] " + std::string(tag) + ": " + status + espera + " | " +
                        formatar_duracao(decorrido));
                    status_anterior = status;
                    ultimo_log = agora;
                } else if (std::chrono::duration<double>(agora - ultimo_log).count() >= 60.0) {
                    log("[JOB] " + std::string(tag) + ": " + status + " | " +
                        formatar_duracao(decorrido));
                    ultimo_log = agora;
                }

                if (status_na_lista(status, status_ok)) {
                    std::string url_resultado = url_do_job(ds.job_resultado, ds, endpoint, job_id);
                    if (url_resultado.empty()) {
                        download_url = extrair_url_download(ultimo_json);
                    } else {
                        RespostaHttp rresp =
                            requisicao_api(url_resultado, auth, api_key);
                        if (!rresp.erro_rede && rresp.status >= 200 && rresp.status < 300) {
                            try {
                                download_url = extrair_url_download(json::parse(rresp.corpo));
                            } catch (...) {}
                        }
                        if (download_url.empty()) download_url = url_resultado;
                    }
                    if (download_url.empty()) {
                        log("[ERRO] " + std::string(tag) + ": resultado indisponivel");
                        return false;
                    }
                    break;
                }

                if (status_na_lista(status, status_erro)) {
                    std::string motivo = motivo_rejeicao_job(ds, auth, endpoint, api_key, job_id);
                    if (motivo.empty()) motivo = status;
                    if (texto_contem_algum(minusculas(motivo), ds.job_reenviar_se)) {
                        log("[JOB] " + std::string(tag) + ": nova tentativa em 60s (" + motivo + ")");
                        dormir(60.0);
                        reenviar = true;
                        job_id.clear();
                    } else {
                        log("[ERRO] " + std::string(tag) + ": " + motivo);
                        return false;
                    }
                    break;
                }
            }
        } while (reenviar);
    } else {
        log("[JOB PRONTO] " + std::string(tag) + ": resultado ja processado; baixando agora");
    }

    download_url = sanitizar_url_download(download_url);
    if (download_url.empty()) {
        log("[ERRO] " + std::string(tag) + ": sem URL de download");
        return false;
    }
    if (download_url.rfind("http", 0) != 0) {
        if (download_url.front() != '/') download_url = "/" + download_url;
        download_url = endpoint + download_url;
    }

    char nome_temp[128];
    std::snprintf(nome_temp, sizeof(nome_temp), "%s_%04d_%02d.temp", ds.id.c_str(), ano, mes);
    std::filesystem::path temporario = pasta_saida / nome_temp;

    bool ok = false;
    bool pulado_max = false;
    double timeout_download = std::max(CFG.http_timeout, 1800.0);

    auto baixar_ate_asset = [&](std::string url) {
        for (int salto = 0; salto < 4; ++salto) {
            std::error_code ec;
            std::filesystem::remove(temporario, ec);
            pulado_max = false;
            if (!http_download_stream(url, temporario.string(), auth, api_key, tag,
                                      timeout_download, ds.tamanho_max_bytes, &pulado_max))
                return false;
            std::string asset_url = extrair_asset_de_arquivo(temporario);
            if (asset_url.empty()) return true;
            std::filesystem::remove(temporario, ec);
            if (asset_url.rfind("http", 0) != 0) {
                if (asset_url.front() != '/') asset_url = "/" + asset_url;
                asset_url = endpoint + asset_url;
            }
            url = asset_url;
        }
        return false;
    };

    for (int tentativa_dl = 0; tentativa_dl < 3 && !ok && !pulado_max; ++tentativa_dl) {
        if (tentativa_dl > 0) {
            log("[REPETINDO] Download " + std::string(tag) + " em 15s (tentativa " +
                std::to_string(tentativa_dl + 1) + "/3)...");
            dormir(15.0);
        }
        ok = baixar_ate_asset(download_url);
    }
    if (!ok && !pulado_max) {
        std::error_code ec;
        std::filesystem::remove(temporario, ec);
    }

    if (ok && std::filesystem::exists(temporario)) {
        // Checagem final de tamanho (servidor pode nao informar tamanho antes)
        if (ds.tamanho_max_bytes > 0) {
            std::error_code ec_sz;
            auto sz = std::filesystem::file_size(temporario, ec_sz);
            if (!ec_sz && sz > static_cast<unsigned long long>(ds.tamanho_max_bytes)) {
                std::filesystem::remove(temporario, ec_sz);
                log("[SKIP] " + std::string(tag) + " (" + formatar_bytes((long long)sz) +
                    ") excede tamanho_max (" + formatar_bytes(ds.tamanho_max_bytes) + ")");
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(temporario, destino, ec);
        if (!ec) {
            // Confere o mes real no GRIB (protege contra job reaproveitado de outro mes).
            if (extensao == "grib" || extensao == "grb") {
                int ano_real = 0, mes_real = 0;
                if (ler_data_grib_local(destino, ano_real, mes_real) &&
                    (ano_real != ano || mes_real != mes)) {
                    char tag_real[64];
                    std::snprintf(tag_real, sizeof(tag_real), "%04d-%02d", ano_real, mes_real);
                    std::filesystem::path destino_real =
                        pasta_saida / nome_arquivo_mensal(ds, ano_real, mes_real, extensao);
                    std::error_code ec_mv;
                    std::filesystem::rename(destino, destino_real, ec_mv);
                    if (!ec_mv) {
                        log("[AVISO] Servidor devolveu " + std::string(tag_real) + " para o pedido " +
                            std::string(tag) + "; arquivo salvo no nome correto.");
                        destino = destino_real;
                    } else if (std::filesystem::exists(destino_real)) {
                        // Mes ja baixado antes: descarta a copia com nome errado.
                        std::error_code ec_rm;
                        std::filesystem::remove(destino, ec_rm);
                        log("[AVISO] Servidor devolveu " + std::string(tag_real) + " (ja existente); " +
                            "duplicado do pedido " + std::string(tag) + " descartado.");
                        return true;
                    }
                }
            }
            log("[CONCLUÍDO] " + destino.filename().string());

            // Importação automática imediata para o SQLite se configurado
            if (ds.auto_importar_banco && (extensao == "grib" || extensao == "grb")) {
                GribImportConfig imp_cfg;
                imp_cfg.db_path = db_alvo;
                imp_cfg.tabela = tab_alvo;
                imp_cfg.coordenadas = CFG.coordenadas;
                imp_cfg.process_workers = 1; // import único inline; paralelismo é no nível de meses
                importar_arquivo_grib(destino.string(), imp_cfg);
            }

            return true;
        } else {
            log("[ERRO] Falha ao renomear arquivo final: " + ec.message());
        }
    }

    return false;
}

// Baixa resultados de jobs já concluídos, evitando reenviar o pedido e esperar
// tudo de novo na fila. Para arquivos GRIB o período é lido do cabeçalho,
// pois muitas APIs de jobs não devolvem os parâmetros do pedido.
static void recuperar_resultados_job(const DatasetCfg& ds, const std::string& base_url,
                                     const std::string& api_key) {
    std::string url = url_do_job(ds.job_listar_ok, ds, cortar(base_url), "");
    if (url.empty()) return;
    AuthApi auth = auth_do_dataset(ds);
    RespostaHttp r = requisicao_api(url, auth, api_key);
    if (r.erro_rede || r.status < 200 || r.status >= 300) return;

    json j;
    try {
        j = json::parse(r.corpo);
    } catch (...) {
        return;
    }
    const json* lista = lista_de_jobs(j, ds.job_lista_campo);
    if (!lista) return;

    std::set<std::string> vistos;
    int disponiveis = 0;
    for (const auto& job : *lista) {
        if (!job.is_object()) continue;
        if (!ds.job_campo_dataset.empty() && !ds.dataset_api.empty() &&
            valor_do_campo(job, ds.job_campo_dataset) != ds.dataset_api)
            continue;

        const json* alvo = &job;
        if (!ds.job_campo_asset.empty()) {
            alvo = navegar_json(job, ds.job_campo_asset);
            if (!alvo) continue;
        }
        std::string href = extrair_url_download(*alvo);
        if (href.empty() || !vistos.insert(href).second) continue;

        int ano = 0, mes = 0;
        if (!ler_data_grib_remota(href, auth, api_key, ano, mes)) continue;
        ++disponiveis;
        char tag[64];
        std::snprintf(tag, sizeof(tag), "%04d-%02d", ano, mes);
        log("[JOB] resultado pronto: " + std::string(tag));
        processar_mes_arquivo(ds, ano, mes, base_url, api_key, href);
    }
    if (disponiveis > 0)
        log("[RECUPERADOS] " + std::to_string(disponiveis) + " resultado(s) prontos.");
}

bool baixar_arquivo_seguro(const std::string& url,
                           const std::string& caminho_destino,
                           const std::string& api_key,
                           long long tamanho_min,
                           const std::map<std::string, std::string>& headers,
                           double timeout_sec,
                           long long tamanho_max,
                           bool* pulado_por_tamanho) {
    OpcoesDownload op;
    op.url = url;
    op.caminho = caminho_destino;
    op.headers = headers;
    op.chave_api = api_key;
    op.timeout_sec = timeout_sec;
    op.tamanho_min = tamanho_min;
    op.tamanho_max = tamanho_max;
    op.verificar_ssl = false; // downloads diretos costumam usar certificados proprios
    op.user_agent_navegador = true;
    op.permitir_reuso = true;
    op.progresso = false;
    op.pulado_por_tamanho = pulado_por_tamanho;

    return http_baixar_arquivo(op).ok;
}

void executar_dataset_arquivo(const DatasetCfg& ds) {
    std::string pasta_saida_str = resolver_pasta_saida(ds.pasta_saida, "dados_arquivos", "PASTA_SAIDA");
    std::filesystem::path pasta_saida(pasta_saida_str);
    std::error_code ec_mk;
    std::filesystem::create_directories(pasta_saida, ec_mk);
    if (ec_mk) {
        log("[ERRO] Nao foi possivel criar pasta_saida '" + pasta_saida.string() + "': " + ec_mk.message());
        return;
    }

    if (std::filesystem::exists(pasta_saida)) {
        for (const auto& entry : std::filesystem::directory_iterator(pasta_saida)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".temp" || ext == ".tmp") {
                    try {
                        std::filesystem::remove(entry.path());
                        log("[LIMPEZA] Removido arquivo temporário: " + entry.path().filename().string());
                    } catch (...) {}
                }
            }
        }
    }

    std::string api_url = ds.url.empty() ? obter_env_ou_config("API_URL", "", "") : ds.url;
    std::string api_key = ds.api_keys;
    if (api_key.empty() && !ds.api_key_env.empty())
        api_key = obter_env_ou_config(ds.api_key_env.c_str(), "", "");
    if (api_key.empty()) api_key = obter_env_ou_config("API_KEY", "", "");
    AuthApi auth = auth_do_dataset(ds);

    // Caso 1: Se o dataset contém uma lista direta de URLs (modo lista direta ou extração web)
    if (!ds.lista_urls.empty() || (!ds.padrao_extracao.empty() && !api_url.empty())) {
        std::vector<std::string> urls_para_baixar = ds.lista_urls;

        if (!ds.padrao_extracao.empty() && !api_url.empty()) {
            log("[EXTRAÇÃO] Acessando " + api_url + " para extrair links (" + ds.padrao_extracao + ")...");
            RespostaHttp r = requisicao_api(api_url, auth, api_key, "GET", "", CFG.http_timeout);
            if (r.status >= 200 && r.status < 300 && !r.corpo.empty()) {
                auto extraidas = extrair_urls_de_conteudo(r.corpo, api_url, ds.padrao_extracao);
                log("[EXTRAÇÃO] " + std::to_string(extraidas.size()) + " links/arquivos encontrados.");
                urls_para_baixar.insert(urls_para_baixar.end(), extraidas.begin(), extraidas.end());
            } else {
                log("[ERRO] Falha ao ler página para extração: HTTP " + std::to_string(r.status));
            }
        }

        if (urls_para_baixar.empty()) {
            log("[INFO] Nenhuma URL encontrada para download em " + ds.id);
            return;
        }

        int n_workers = CFG.download_workers;
        if (n_workers < 1) n_workers = 1;
        std::map<std::string, std::string> headers_dl = ds.headers;
        std::string chave_dl = api_key;
        if (!api_key.empty()) {
            std::string modo = auth.modo;
            if (modo == "auto") modo = api_key.find(':') != std::string::npos ? "basic" : "header";
            if (modo == "header") {
                headers_dl[auth.header] =
                    auth.prefixo.empty() ? api_key : auth.prefixo + " " + api_key;
                chave_dl.clear();
            } else if (modo == "query") {
                chave_dl.clear();
            }
        }
        std::atomic<size_t> idx_url{0};
        std::atomic<size_t> baixados{0};
        std::atomic<size_t> falhas{0};
        std::atomic<size_t> pulados{0};
        std::vector<std::thread> workers;

        log("============================================================");
        log("BAIXANDO ARQUIVOS DIRETOS: " + ds.id + " (" + std::to_string(urls_para_baixar.size()) + " itens)");
        log("Diretório de saída: " + pasta_saida.string());
        if (ds.tamanho_max_bytes > 0)
            log("Tamanho maximo: " + formatar_bytes(ds.tamanho_max_bytes) + " (maiores sao pulados)");
        log("============================================================");

        for (int i = 0; i < n_workers; ++i) {
            workers.emplace_back([&] {
                while (true) {
                    size_t k = idx_url.fetch_add(1);
                    if (k >= urls_para_baixar.size()) break;
                    const std::string& u = urls_para_baixar[k];
                    std::string nome_arq = obter_nome_arquivo_da_url(u);
                    std::filesystem::path dest = pasta_saida / nome_arq;

                    std::string url_final = url_com_chave(u, auth, api_key);
                    bool pulado_max = false;
                    bool ok = baixar_arquivo_seguro(url_final, dest.string(), chave_dl,
                                                    ds.tamanho_min_bytes, headers_dl,
                                                    CFG.http_timeout, ds.tamanho_max_bytes,
                                                    &pulado_max);
                    if (ok) {
                        ++baixados;
                        log("[ARQUIVO " + std::to_string(baixados.load()) + "/" + std::to_string(urls_para_baixar.size()) + "] " + nome_arq);
                    } else if (pulado_max) {
                        ++pulados;
                    } else {
                        ++falhas;
                        log("[FALHA] " + u);
                    }
                }
            });
        }
        for (auto& t : workers) t.join();
        log("[CONCLUÍDO] " + std::to_string(baixados.load()) + " baixados, " +
            std::to_string(pulados.load()) + " pulados (tamanho), " +
            std::to_string(falhas.load()) + " falhas.");
        return;
    }

    // Caso 2: Dataset temporal (jobs/arquivos por período ou mês)
    int ano_inicial = para_int(obter_env_ou_config("ANO_INICIAL", "", std::to_string(ds.ano_inicial)), 1940);
    int dias_atraso = para_int(obter_env_ou_config("DIAS_ATRASO", "", std::to_string(ds.dias_atraso)), 0);

    Data data_limite = civil_de_dias(dias_de_civil(agora_local().tm_year + 1900, agora_local().tm_mon + 1, agora_local().tm_mday) - dias_atraso);

    std::vector<std::pair<int, int>> meses;
    for (int ano = ano_inicial; ano <= data_limite.ano; ++ano) {
        for (int mes = 1; mes <= 12; ++mes) {
            if (ano == data_limite.ano && mes > data_limite.mes) break;
            meses.emplace_back(ano, mes);
        }
    }

    int n_workers = CFG.download_workers;
    if (n_workers < 1) n_workers = 1;
    if (ds.job_sequencial) {
        if (n_workers != 1)
            log("[CFG] " + ds.id + " exige jobs sequenciais; usando 1 download_worker.");
        n_workers = 1;
    }
    if ((size_t)n_workers > meses.size() && !meses.empty())
        n_workers = (int)meses.size();

    log("============================================================");
    log("PROCESSANDO DATASET EM ARQUIVOS: " + ds.id);
    log("Diretório de saída: " + pasta_saida.string());
    log("Anos: " + std::to_string(ano_inicial) + " até " + std::to_string(data_limite.ano));
    log("Meses: " + std::to_string(meses.size()) + " | download_workers=" +
        std::to_string(n_workers));
    log("============================================================");

    std::string formato_saida = minusculas(obter_env_ou_config("FORMATO", ds.extensao, "bin"));
    if (!ds.job_listar_ok.empty() &&
        (formato_saida == "grib" || formato_saida == "grb")) {
        recuperar_resultados_job(ds, api_url, api_key);
    }

    std::atomic<size_t> idx_mes{0};
    std::vector<std::thread> workers;
    workers.reserve((size_t)n_workers);
    for (int i = 0; i < n_workers; ++i) {
        workers.emplace_back([&] {
            while (true) {
                size_t k = idx_mes.fetch_add(1);
                if (k >= meses.size()) break;
                try {
                    processar_mes_arquivo(ds, meses[k].first, meses[k].second, api_url, api_key);
                } catch (const std::exception& e) {
                    log(std::string("[ERROR] MES ") + std::to_string(meses[k].first) + "-" +
                        std::to_string(meses[k].second) + ": " + e.what());
                }
            }
        });
    }
    for (auto& t : workers) t.join();
}
