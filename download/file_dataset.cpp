#include "file_dataset.h"
#include "era5_importer.h"
#include "rate_limit.h"
#include "storage_sqlite.h"
#include "utils.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using json = nlohmann::json;

struct RespostaHttpAvancada {
    long status = 0;
    bool erro_rede = false;
    std::string erro_mensagem;
    std::string corpo;
    std::string header_location;
};

static size_t escrever_string_cb(char* ptr, size_t tam, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, tam * nmemb);
    return tam * nmemb;
}

static size_t ler_headers_cb(char* buffer, size_t tam, size_t n, void* ud) {
    auto* resp = static_cast<RespostaHttpAvancada*>(ud);
    std::string linha(buffer, tam * n);
    std::string baixo = linha;
    std::transform(baixo.begin(), baixo.end(), baixo.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (baixo.rfind("location:", 0) == 0) resp->header_location = cortar(linha.substr(9));
    return tam * n;
}

static size_t escrever_arquivo_cb(char* ptr, size_t tam, size_t nmemb, void* ud) {
    auto* out = static_cast<std::ofstream*>(ud);
    out->write(ptr, tam * nmemb);
    return tam * nmemb;
}

static RespostaHttpAvancada http_post_json(const std::string& url,
                                           const std::string& json_body,
                                           const std::string& api_key,
                                           double timeout_sec = 60.0) {
    RespostaHttpAvancada r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.erro_rede = true;
        r.erro_mensagem = "Falha ao inicializar CURL";
        return r;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!api_key.empty()) {
        if (api_key.find(':') != std::string::npos) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, api_key.c_str());
        } else {
            std::string auth_header = "Authorization: Bearer " + api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        std::string token_header = "PRIVATE-TOKEN: " + api_key;
        headers = curl_slist_append(headers, token_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, escrever_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.corpo);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ler_headers_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_sec * 1000.0));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.erro_rede = true;
        r.erro_mensagem = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

static RespostaHttpAvancada http_get_headers(const std::string& url,
                                             const std::string& api_key,
                                             double timeout_sec = 60.0) {
    RespostaHttpAvancada r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.erro_rede = true;
        r.erro_mensagem = "Falha ao inicializar CURL";
        return r;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!api_key.empty()) {
        if (api_key.find(':') != std::string::npos) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, api_key.c_str());
        } else {
            std::string auth_header = "Authorization: Bearer " + api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        std::string token_header = "PRIVATE-TOKEN: " + api_key;
        headers = curl_slist_append(headers, token_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, escrever_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.corpo);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ler_headers_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_sec * 1000.0));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.erro_rede = true;
        r.erro_mensagem = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

static std::string sanitizar_url_download(std::string url) {
    const char* brancos = " \t\r\n";
    size_t inicio = url.find_first_not_of(brancos);
    if (inicio == std::string::npos) return "";
    url = url.substr(inicio, url.find_last_not_of(brancos) - inicio + 1);

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

struct DownloadProgressContext {
    std::string tag;
    std::chrono::steady_clock::time_point ult_log;
    double ultimo_pct = -1.0;
};

static int curl_xfer_info_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    if (dltotal <= 0) return 0;
    auto* ctx = static_cast<DownloadProgressContext*>(clientp);
    auto agora = std::chrono::steady_clock::now();
    double dec = std::chrono::duration<double>(agora - ctx->ult_log).count();
    bool concluido = (dlnow == dltotal);

    double baixado_mb = static_cast<double>(dlnow) / (1024.0 * 1024.0);
    double total_mb = static_cast<double>(dltotal) / (1024.0 * 1024.0);
    double pct = (total_mb > 0.0) ? (baixado_mb / total_mb * 100.0) : 0.0;

    // Vários workers baixam ao mesmo tempo. Em vez de sobrescrever a mesma linha
    // com '\r' (o que empilhava os itens uns sobre os outros), cada atualização é
    // impressa em sua própria linha via log(), que serializa a saída com mutex.
    // Assim os workers aparecem empilhados e em ordem, sem se sobreporem.
    // Reduz a frequência: imprime no início, a cada ~1s quando o % avança >= 2,
    // e sempre na conclusão (100%).
    bool inicio = (ctx->ultimo_pct < 0.0);
    if (concluido || (dec >= 1.0 && (inicio || pct - ctx->ultimo_pct >= 2.0))) {
        ctx->ult_log = agora;
        ctx->ultimo_pct = pct;

        char buf[200];
        std::snprintf(buf, sizeof(buf), "  [BAIXANDO] %s: %.2f MB / %.2f MB (%.1f%%)",
                      ctx->tag.c_str(), baixado_mb, total_mb, pct);
        log(buf);
    }
    return 0;
}

static bool http_download_stream(const std::string& url,
                                 const std::string& caminho_destino,
                                 const std::string& api_key,
                                 const std::string& tag,
                                 double timeout_sec = 1800.0) {
    std::ofstream arq(caminho_destino, std::ios::binary);
    if (!arq) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    bool eh_storage_presigned = (url.find("object-store") != std::string::npos ||
                                 url.find("s3.") != std::string::npos ||
                                 url.find("amazonaws.com") != std::string::npos ||
                                 url.find("Signature=") != std::string::npos ||
                                 url.find("X-Amz-") != std::string::npos ||
                                 url.find("cci2-prod-cache") != std::string::npos);

    struct curl_slist* headers = nullptr;
    if (!api_key.empty() && !eh_storage_presigned) {
        if (api_key.find(':') != std::string::npos) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, api_key.c_str());
        } else {
            std::string auth_header = "Authorization: Bearer " + api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        std::string token_header = "PRIVATE-TOKEN: " + api_key;
        headers = curl_slist_append(headers, token_header.c_str());
    }

    DownloadProgressContext prog_ctx{tag, std::chrono::steady_clock::now()};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, escrever_arquivo_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &arq);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_xfer_info_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog_ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_sec * 1000.0));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    arq.close();

    if (rc != CURLE_OK || status < 200 || status >= 300) {
        log("\n[ERRO] Download HTTP " + std::to_string(status) + " para " + tag);
        return false;
    }

    std::ifstream checagem(caminho_destino, std::ios::binary);
    if (!checagem) return false;

    char cabecalho[512] = {0};
    checagem.read(cabecalho, sizeof(cabecalho) - 1);
    std::streamsize lidos = checagem.gcount();
    checagem.close();

    std::string inicio_str(cabecalho, lidos > 0 ? static_cast<size_t>(lidos) : 0);
    if (inicio_str.rfind("<?xml", 0) == 0 || inicio_str.rfind("<html", 0) == 0 ||
        inicio_str.rfind("<!DOCTYPE", 0) == 0 || inicio_str.rfind("{\"error\"", 0) == 0 ||
        inicio_str.rfind("{\"code\"", 0) == 0) {
        log("\n[ERRO] Servidor retornou mensagem de erro para " + tag + ": " + inicio_str.substr(0, 150));
        return false;
    }

    return true;
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

static bool processar_dataset_era5_mes(const DatasetCfg& ds, int ano, int mes, const std::string& base_url, const std::string& api_key) {
    std::string pasta_saida_str = obter_env_ou_config("PASTA_SAIDA", ds.pasta_saida, "dados_arquivos");
    std::filesystem::path pasta_saida(pasta_saida_str);
    std::filesystem::create_directories(pasta_saida);

    std::string extensao = obter_env_ou_config("FORMATO", ds.extensao, "bin");
    if (extensao == "netcdf") extensao = "nc";

    char nome_final[128];
    if (!ds.padrao_arquivo.empty()) {
        Contexto fctx;
        fctx["id"] = ds.id;
        fctx["ano"] = std::to_string(ano);
        char mes_buf[8];
        std::snprintf(mes_buf, sizeof(mes_buf), "%02d", mes);
        fctx["mes"] = mes_buf;
        fctx["extensao"] = extensao;
        std::string fn = aplicar_ctx(ds.padrao_arquivo, fctx);
        std::snprintf(nome_final, sizeof(nome_final), "%s", fn.c_str());
    } else {
        std::snprintf(nome_final, sizeof(nome_final), "%s_%04d_%02d.%s", ds.id.c_str(), ano, mes, extensao.c_str());
    }
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
            if (era5_mes_ja_no_banco(conn, tab_alvo, ano, mes, CFG.coordenadas, hrs)) {
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
                Era5ImportConfig imp_cfg;
                imp_cfg.db_path = db_alvo;
                imp_cfg.tabela = tab_alvo;
                imp_cfg.coordenadas = CFG.coordenadas;
                importar_arquivo_era5(destino.string(), imp_cfg);
            }
            return true;
        } else {
            log("[REMOVENDO ARQUIVO INVÁLIDO/INCOMPLETO] " + std::string(nome_final));
            std::filesystem::remove(destino);
        }
    }

    int dias_no_mes = 31;
    if (mes == 4 || mes == 6 || mes == 9 || mes == 11) dias_no_mes = 30;
    else if (mes == 2) {
        bool bissexto = (ano % 4 == 0 && (ano % 100 != 0 || ano % 400 == 0));
        dias_no_mes = bissexto ? 29 : 28;
    }

    std::vector<std::string> lista_dias;
    for (int d = 1; d <= dias_no_mes; ++d) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d", d);
        lista_dias.push_back(buf);
    }

    std::vector<std::string> horarios = ds.horarios;
    if (horarios.empty()) {
        std::string h_str = obter_env_ou_config("HORARIOS", "", "00:00,12:00");
        horarios = dividir(h_str, ',');
    }

    std::vector<std::string> vars = ds.vars;
    if (vars.empty()) {
        vars = {
            "10m_u_component_of_wind", "10m_v_component_of_wind",
            "2m_temperature", "2m_dewpoint_temperature",
            "surface_pressure", "mean_sea_level_pressure",
            "total_precipitation", "surface_solar_radiation_downwards"
        };
    }

    std::string dataset_name = ds.dataset_api.empty() ? "reanalysis-era5-single-levels" : ds.dataset_api;

    char tag[64];
    std::snprintf(tag, sizeof(tag), "%04d-%02d", ano, mes);
    log("[SOLICITANDO] " + std::string(tag));

    json solicitacao_direct;
    solicitacao_direct["product_type"] = json::array({ds.product_type.empty() ? "reanalysis" : ds.product_type});
    solicitacao_direct["variable"] = vars;
    char ano_str[16], mes_str[16];
    std::snprintf(ano_str, sizeof(ano_str), "%04d", ano);
    std::snprintf(mes_str, sizeof(mes_str), "%02d", mes);
    solicitacao_direct["year"] = json::array({ano_str});
    solicitacao_direct["month"] = json::array({mes_str});
    solicitacao_direct["day"] = lista_dias;
    solicitacao_direct["time"] = horarios;
    solicitacao_direct["format"] = (extensao == "nc" ? "netcdf" : "grib");
    solicitacao_direct["data_format"] = (extensao == "nc" ? "netcdf" : "grib");
    solicitacao_direct["download_format"] = "unarchived";

    json solicitacao_cads;
    solicitacao_cads["inputs"] = solicitacao_direct;

    std::string endpoint_post = base_url;
    if (endpoint_post.rfind("http", 0) != 0) endpoint_post = "https://cds.climate.copernicus.eu/api";
    if (endpoint_post.back() == '/') endpoint_post.pop_back();

    std::string url_execute = endpoint_post + "/resources/" + dataset_name;
    RespostaHttpAvancada resp;
    for (int tentativa = 0; tentativa < 3; ++tentativa) {
        if (tentativa > 0) {
            long espera = (resp.status == 429) ? 30L : 10L * tentativa;
            log("[REPETINDO] " + std::string(tag) + ": nova tentativa de envio em " +
                std::to_string(espera) + "s (falha anterior: HTTP " + std::to_string(resp.status) +
                (resp.erro_rede ? " / " + resp.erro_mensagem : "") + ")");
            dormir(static_cast<double>(espera));
        }

        url_execute = endpoint_post + "/resources/" + dataset_name;
        resp = http_post_json(url_execute, solicitacao_direct.dump(), api_key);

        if (resp.status == 404) {
            url_execute = endpoint_post + "/retrieve/v1/processes/" + dataset_name + "/execute";
            resp = http_post_json(url_execute, solicitacao_cads.dump(), api_key);
        }

        if (!resp.erro_rede && resp.status >= 200 && resp.status < 300) break;
    }

    if (resp.erro_rede || resp.status < 200 || resp.status >= 300) {
        std::string err_info = resp.erro_rede ? resp.erro_mensagem : ("HTTP " + std::to_string(resp.status));
        std::string corpo_reduzido = resp.corpo.substr(0, 200);
        log("[ERRO] Falha ao enviar requisição para " + std::string(tag) + " (" + err_info + "): " + corpo_reduzido);
        return false;
    }

    std::string job_id;
    std::string download_url;

    try {
        json jresp = json::parse(resp.corpo);
        download_url = extrair_url_download(jresp);
        if (download_url.empty()) {
            if (jresp.contains("job_id") && jresp["job_id"].is_string()) {
                job_id = jresp["job_id"].get<std::string>();
            } else if (jresp.contains("request_id") && jresp["request_id"].is_string()) {
                job_id = jresp["request_id"].get<std::string>();
            } else if (jresp.contains("jobID") && jresp["jobID"].is_string()) {
                job_id = jresp["jobID"].get<std::string>();
            } else if (jresp.contains("status") && jresp.contains("result")) {
                download_url = extrair_url_download(jresp["result"]);
            }
        }
    } catch (...) {}

    if (job_id.empty() && download_url.empty() && !resp.header_location.empty()) {
        download_url = sanitizar_url_download(resp.header_location);
    }

    if (!job_id.empty() && download_url.empty()) {
        std::string status_url = endpoint_post + "/retrieve/v1/jobs/" + job_id;
        for (int tentativa = 0; tentativa < 360; ++tentativa) {
            dormir(5.0);
            RespostaHttpAvancada sresp = http_get_headers(status_url, api_key);
            if (sresp.status >= 200 && sresp.status < 300) {
                try {
                    json sj = json::parse(sresp.corpo);
                    std::string st = sj.value("status", "");
                    if (st == "successful" || st == "completed") {
                        download_url = extrair_url_download(sj);
                        if (download_url.empty()) download_url = status_url + "/results";
                        break;
                    } else if (st == "failed") {
                        log("[ERRO] CDS Job falhou: " + sj.value("message", "erro"));
                        return false;
                    }
                } catch (...) {}
            }
        }
    }

    download_url = sanitizar_url_download(download_url);

    if (download_url.empty()) {
        log("[ERRO] Não foi possível obter URL de download para " + std::string(tag));
        return false;
    }

    if (download_url.rfind("http", 0) != 0) {
        if (download_url.front() != '/') download_url = "/" + download_url;
        download_url = endpoint_post + download_url;
    }

    char nome_temp[128];
    std::snprintf(nome_temp, sizeof(nome_temp), "%s_%04d_%02d.temp", ds.id.c_str(), ano, mes);
    std::filesystem::path temporario = pasta_saida / nome_temp;

    std::string url_atual = download_url;
    bool ok = false;
    for (int pulo = 0; pulo < 4; ++pulo) {
        log("[URL] " + std::string(tag) + ": " + url_atual);
        ok = http_download_stream(url_atual, temporario.string(), api_key, tag);
        if (!ok) break;

        std::string asset_url = extrair_asset_de_arquivo(temporario);
        if (asset_url.empty()) break;

        std::filesystem::remove(temporario);
        if (asset_url.rfind("http", 0) != 0) {
            if (asset_url.front() != '/') asset_url = "/" + asset_url;
            asset_url = endpoint_post + asset_url;
        }
        log("[SEGUINDO ASSET] " + std::string(tag) + " -> " + asset_url);
        url_atual = asset_url;
    }

    if (ok && std::filesystem::exists(temporario)) {
        std::error_code ec;
        std::filesystem::rename(temporario, destino, ec);
        if (!ec) {
            log("[CONCLUÍDO] " + std::string(nome_final));

            // Importação automática imediata para o SQLite se configurado
            if (ds.auto_importar_banco && (extensao == "grib" || extensao == "grb")) {
                Era5ImportConfig imp_cfg;
                imp_cfg.db_path = db_alvo;
                imp_cfg.tabela = tab_alvo;
                imp_cfg.coordenadas = CFG.coordenadas;
                importar_arquivo_era5(destino.string(), imp_cfg);
            }

            return true;
        } else {
            log("[ERRO] Falha ao renomear arquivo final: " + ec.message());
        }
    }

    return false;
}

bool baixar_arquivo_seguro(const std::string& url,
                           const std::string& caminho_destino,
                           const std::string& api_key,
                           int tamanho_min,
                           const std::map<std::string, std::string>& headers,
                           double timeout_sec) {
    std::filesystem::path dest(caminho_destino);
    std::filesystem::create_directories(dest.parent_path());

    // Se já existe e é válido
    std::error_code ec;
    if (std::filesystem::exists(dest, ec)) {
        auto sz = std::filesystem::file_size(dest, ec);
        if (!ec && sz > 0 && (tamanho_min <= 0 || sz >= static_cast<unsigned long long>(tamanho_min))) {
            return true;
        }
        std::filesystem::remove(dest, ec);
    }

    std::filesystem::path temp_dest = dest;
    temp_dest += ".temp";
    std::filesystem::remove(temp_dest, ec);

    std::ofstream arq(temp_dest, std::ios::binary);
    if (!arq) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    for (const auto& kv : headers) {
        std::string l = kv.first + ": " + kv.second;
        hdr = curl_slist_append(hdr, l.c_str());
    }

    if (!api_key.empty()) {
        if (api_key.find(':') != std::string::npos) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, api_key.c_str());
        } else {
            std::string auth = "Authorization: Bearer " + api_key;
            hdr = curl_slist_append(hdr, auth.c_str());
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (hdr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, escrever_arquivo_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &arq);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_sec * 1000.0));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (hdr) curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    arq.close();

    if (rc != CURLE_OK || status < 200 || status >= 300) {
        std::filesystem::remove(temp_dest, ec);
        return false;
    }

    auto sz = std::filesystem::file_size(temp_dest, ec);
    if (ec || sz == 0 || (tamanho_min > 0 && sz < static_cast<unsigned long long>(tamanho_min))) {
        std::filesystem::remove(temp_dest, ec);
        return false;
    }

    std::filesystem::rename(temp_dest, dest, ec);
    return !ec;
}

void executar_dataset_arquivo(const DatasetCfg& ds) {
    std::string pasta_saida_str = obter_env_ou_config("PASTA_SAIDA", ds.pasta_saida, "dados_arquivos");
    std::filesystem::path pasta_saida(pasta_saida_str);
    std::filesystem::create_directories(pasta_saida);

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
    std::string api_key = ds.api_keys.empty() ? obter_env_ou_config("API_KEY", obter_env_ou_config("CDSAPI_KEY", "", ""), "") : ds.api_keys;

    // Caso 1: Se o dataset contém uma lista direta de URLs (modo lista direta ou extração web)
    if (!ds.lista_urls.empty() || (!ds.padrao_extracao.empty() && !api_url.empty())) {
        std::vector<std::string> urls_para_baixar = ds.lista_urls;

        if (!ds.padrao_extracao.empty() && !api_url.empty()) {
            log("[EXTRAÇÃO] Acessando " + api_url + " para extrair links (" + ds.padrao_extracao + ")...");
            RespostaHttpAvancada r = http_get_headers(api_url, api_key, CFG.http_timeout);
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
        std::atomic<size_t> idx_url{0};
        std::atomic<size_t> baixados{0};
        std::atomic<size_t> falhas{0};
        std::vector<std::thread> workers;

        log("============================================================");
        log("BAIXANDO ARQUIVOS DIRETOS: " + ds.id + " (" + std::to_string(urls_para_baixar.size()) + " itens)");
        log("Diretório de saída: " + pasta_saida.string());
        log("============================================================");

        for (int i = 0; i < n_workers; ++i) {
            workers.emplace_back([&] {
                while (true) {
                    size_t k = idx_url.fetch_add(1);
                    if (k >= urls_para_baixar.size()) break;
                    const std::string& u = urls_para_baixar[k];
                    std::string nome_arq = obter_nome_arquivo_da_url(u);
                    std::filesystem::path dest = pasta_saida / nome_arq;

                    bool ok = baixar_arquivo_seguro(u, dest.string(), api_key, ds.tamanho_min_bytes, ds.headers, CFG.http_timeout);
                    if (ok) {
                        ++baixados;
                        log("  [ARQUIVO " + std::to_string(baixados.load()) + "/" + std::to_string(urls_para_baixar.size()) + "] " + nome_arq);
                    } else {
                        ++falhas;
                        log("  [FALHA] " + u);
                    }
                }
            });
        }
        for (auto& t : workers) t.join();
        log("[CONCLUÍDO] " + std::to_string(baixados.load()) + " baixados, " + std::to_string(falhas.load()) + " falhas.");
        return;
    }

    // Caso 2: Dataset temporal (ERA5 ou APIs de reanálise por períodos/meses)
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
    if ((size_t)n_workers > meses.size() && !meses.empty())
        n_workers = (int)meses.size();

    log("============================================================");
    log("PROCESSANDO DATASET EM ARQUIVOS: " + ds.id);
    log("Diretório de saída: " + pasta_saida.string());
    log("Anos: " + std::to_string(ano_inicial) + " até " + std::to_string(data_limite.ano));
    log("Meses: " + std::to_string(meses.size()) + " | download_workers=" +
        std::to_string(n_workers));
    log("============================================================");

    std::atomic<size_t> idx_mes{0};
    std::vector<std::thread> workers;
    workers.reserve((size_t)n_workers);
    for (int i = 0; i < n_workers; ++i) {
        workers.emplace_back([&] {
            while (true) {
                size_t k = idx_mes.fetch_add(1);
                if (k >= meses.size()) break;
                try {
                    processar_dataset_era5_mes(ds, meses[k].first, meses[k].second, api_url, api_key);
                } catch (const std::exception& e) {
                    log(std::string("[ERROR] MES ") + std::to_string(meses[k].first) + "-" +
                        std::to_string(meses[k].second) + ": " + e.what());
                }
            }
        });
    }
    for (auto& t : workers) t.join();
}
