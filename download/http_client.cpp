#include "http_client.h"
#include "config.h"
#include "rate_limit.h"
#include "utils.h"
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static CURL* curl_da_thread() {
    static thread_local CURL* handle = curl_easy_init();
    return handle;
}

// Autenticação
AuthApi auth_do_dataset(const DatasetCfg& ds) {
    AuthApi a;
    a.modo = minusculas(ds.api_key_em);
    if (a.modo.empty()) a.modo = "auto";
    if (!ds.api_key_header.empty()) a.header = ds.api_key_header;
    a.prefixo = ds.api_key_prefix;
    a.param = ds.api_key_param.empty() ? CFG.api_key_param : ds.api_key_param;
    return a;
}

bool url_presignada(const std::string& url) {
    return url.find("X-Amz-") != std::string::npos ||
           url.find("X-Goog-") != std::string::npos ||
           url.find("Signature=") != std::string::npos ||
           url.find("sig=") != std::string::npos ||
           url.find("object-store") != std::string::npos ||
           url.find("amazonaws.com") != std::string::npos;
}

std::string escapar_url(const std::string& s) {
    CURL* c = curl_da_thread();
    if (!c) return s;
    char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string r = e ? e : s;
    if (e) curl_free(e);
    return r;
}

std::string montar_url(const std::string& base,
                       const std::map<std::string, std::string>& params) {
    if (params.empty()) return base;
    std::string url = base;
    char sep = url.find('?') == std::string::npos ? '?' : '&';
    if (!url.empty() && (url.back() == '?' || url.back() == '&')) sep = '\0';
    for (const auto& [k, v] : params) {
        if (sep) url += sep;
        sep = '&';
        url += escapar_url(k) + "=" + escapar_url(v);
    }
    return url;
}

std::string url_com_chave(const std::string& url_entrada, const AuthApi& auth,
                          const std::string& chave) {
    if (chave.empty() || auth.modo != "query" || url_presignada(url_entrada))
        return url_entrada;
    std::string url = url_entrada;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += auth.param + "=" + escapar_url(chave);
    return url;
}

namespace {

struct ListaHeaders {
    curl_slist* p = nullptr;
    ~ListaHeaders() {
        if (p) curl_slist_free_all(p);
    }
    void adicionar(const std::string& linha) {
        p = curl_slist_append(p, linha.c_str());
    }
};

void aplicar_auth(CURL* curl, ListaHeaders& headers, const AuthApi& auth,
                  const std::string& chave, const std::string& url) {
    if (chave.empty() || auth.modo == "none" || url_presignada(url)) return;
    std::string modo = auth.modo;
    if (modo == "auto")
        modo = (chave.find(':') != std::string::npos) ? "basic" : "header";
    if (modo == "basic") {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, chave.c_str());
    } else if (modo == "header") {
        std::string valor = auth.prefixo.empty() ? chave : auth.prefixo + " " + chave;
        headers.adicionar(auth.header + ": " + valor);
    }
}

std::string ca_bundle_padrao() {
    const char* env = std::getenv("CURL_CA_BUNDLE");
    if (env && *env && fs::exists(fs::path(env))) return env;
    static const std::vector<std::string> candidatos = {
        R"(D:\msys64\mingw64\etc\ssl\certs\ca-bundle.crt)",
        R"(C:\msys64\mingw64\etc\ssl\certs\ca-bundle.crt)",
        R"(D:\msys64\mingw64\etc\ssl\cert.pem)",
        R"(C:\msys64\mingw64\etc\ssl\cert.pem)",
        R"(D:\msys64\usr\etc\ssl\certs\ca-bundle.crt)",
        R"(C:\msys64\usr\etc\ssl\certs\ca-bundle.crt)",
        R"(D:\msys64\usr\ssl\certs\ca-bundle.crt)",
        R"(C:\msys64\usr\ssl\certs\ca-bundle.crt)",
    };
    std::error_code ec;
    for (const auto& c : candidatos)
        if (fs::exists(fs::path(c), ec)) return c;
    return "";
}

void aplicar_ca_bundle(CURL* curl) {
    static const std::string bundle = ca_bundle_padrao();
    if (!bundle.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, bundle.c_str());
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    }
}

size_t ler_header(char* buffer, size_t tam, size_t n, void* ud) {
    auto* r = static_cast<RespostaHttp*>(ud);
    size_t total = tam * n;
    std::string linha(buffer, total);

    // Cada bloco de resposta (redirect, 100-continue) reinicia os valores.
    if (linha.rfind("HTTP/", 0) == 0) {
        r->location.clear();
        r->content_length = -1;
        r->content_type.clear();
        r->retry_after = -1.0;
        return total;
    }

    std::string baixo = minusculas(linha);
    if (baixo.rfind("retry-after:", 0) == 0) {
        try {
            r->retry_after = std::stod(cortar(linha.substr(12)));
        } catch (...) {}
    } else if (baixo.rfind("location:", 0) == 0) {
        r->location = cortar(linha.substr(9));
    } else if (baixo.rfind("content-length:", 0) == 0) {
        try {
            r->content_length = std::stoll(cortar(linha.substr(15)));
        } catch (...) {}
    } else if (baixo.rfind("content-type:", 0) == 0) {
        r->content_type = cortar(linha.substr(13));
    }
    return total;
}

struct CtxEscrita {
    const std::function<size_t(char*, size_t)>* fn = nullptr;
    std::string* corpo = nullptr;
};

size_t cb_escrita(char* ptr, size_t tam, size_t nmemb, void* ud) {
    auto* ctx = static_cast<CtxEscrita*>(ud);
    size_t total = tam * nmemb;
    if (ctx->fn && *ctx->fn) return (*ctx->fn)(ptr, total);
    ctx->corpo->append(ptr, total);
    return total;
}

struct CtxProgresso {
    const std::function<int(long long, long long)>* fn = nullptr;
};

int cb_progresso(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<CtxProgresso*>(ud);
    return (*ctx->fn)(static_cast<long long>(dltotal), static_cast<long long>(dlnow));
}

// Escritor assíncrono de disco: o curl empurra blocos para uma fila limitada
// enquanto uma thread separada grava, desacoplando rede e I/O.
class AsyncFileWriter {
public:
    explicit AsyncFileWriter(const std::string& caminho, size_t max_blocos = 64)
        : arq_(caminho, std::ios::binary), max_(max_blocos), ok_(arq_.good()) {
        if (ok_) writer_ = std::thread([this] {
            loop_escrita();
        });
    }

    ~AsyncFileWriter() {
        close();
    }

    AsyncFileWriter(const AsyncFileWriter&) = delete;
    AsyncFileWriter& operator=(const AsyncFileWriter&) = delete;

    bool good() const {
        return ok_.load();
    }

    bool push(const char* dados, size_t n) {
        if (!ok_.load()) return false;
        std::vector<char> bloco(dados, dados + n);
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_cheia_.wait(lk, [&] {
                return fila_.size() < max_ || fechada_;
            });
            if (fechada_) return false;
            fila_.push(std::move(bloco));
        }
        cv_vazia_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fechada_ = true;
        }
        cv_vazia_.notify_one();
        cv_cheia_.notify_all();
        if (writer_.joinable()) writer_.join();
        if (arq_.is_open()) arq_.close();
    }

private:
    void loop_escrita() {
        while (true) {
            std::vector<char> bloco;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_vazia_.wait(lk, [&] {
                    return !fila_.empty() || fechada_;
                });
                if (fila_.empty()) break;
                bloco = std::move(fila_.front());
                fila_.pop();
            }
            cv_cheia_.notify_one();
            arq_.write(bloco.data(), static_cast<std::streamsize>(bloco.size()));
            if (!arq_.good()) {
                ok_.store(false);
                break;
            }
        }
    }

    std::ofstream arq_;
    size_t max_;
    std::atomic<bool> ok_;
    bool fechada_ = false;
    std::queue<std::vector<char>> fila_;
    std::mutex mtx_;
    std::condition_variable cv_cheia_;
    std::condition_variable cv_vazia_;
    std::thread writer_;
};

struct ProgressoDownload {
    std::string tag;
    bool logar = true;
    std::chrono::steady_clock::time_point inicio = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ult_log = std::chrono::steady_clock::now();
    double ultimo_pct = -1.0;
    int ultimo_marco = -1;
};

void log_progresso(ProgressoDownload& p, long long dltotal, long long dlnow) {
    auto agora = std::chrono::steady_clock::now();
    double dec = std::chrono::duration<double>(agora - p.ult_log).count();
    double baixado_mb = static_cast<double>(dlnow) / (1024.0 * 1024.0);

    if (dltotal <= 0) {
        if (dec >= 3.0 && dlnow > 0) {
            double total_decorrido =
                std::chrono::duration<double>(agora - p.inicio).count();
            double mbps = total_decorrido > 0.0 ? baixado_mb / total_decorrido : 0.0;
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "[BAIXANDO] %s: %.2f MB baixados - %.2f MB/s (tamanho total desconhecido)",
                          p.tag.c_str(), baixado_mb, mbps);
            log(buf);
            p.ult_log = agora;
            p.ultimo_pct = 0.0;
        }
        return;
    }

    bool concluido = (dlnow == dltotal);
    double total_mb = static_cast<double>(dltotal) / (1024.0 * 1024.0);
    double pct = total_mb > 0.0 ? (baixado_mb / total_mb * 100.0) : 0.0;
    int marco_atual = static_cast<int>(pct / 10.0);
    bool inicio = (p.ultimo_pct < 0.0);
    bool novo_marco = (marco_atual > p.ultimo_marco);
    bool tempo_suficiente = (dec >= 3.0);

    if (concluido || inicio || novo_marco || tempo_suficiente) {
        p.ult_log = agora;
        p.ultimo_pct = pct;
        p.ultimo_marco = marco_atual;
        double decorrido = std::chrono::duration<double>(agora - p.inicio).count();
        double mbps = decorrido > 0.0 ? baixado_mb / decorrido : 0.0;
        double eta = mbps > 0.0 ? (total_mb - baixado_mb) / mbps : 0.0;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "[BAIXANDO] %s: %.2f MB / %.2f MB (%.1f%%) - %.2f MB/s - ETA %s",
                      p.tag.c_str(), baixado_mb, total_mb, pct, mbps,
                      formatar_duracao(eta).c_str());
        log(buf);
    }
}

} // namespace

RespostaHttp http_executar(const std::string& url, const OpcoesHttp& op,
                           const CallbacksHttp& cb) {
    RespostaHttp r;
    CURL* curl = curl_da_thread();
    if (!curl) {
        r.erro_rede = true;
        r.erro_mensagem = "falha ao iniciar curl";
        return r;
    }
    curl_easy_reset(curl);

    std::string url_final = url;
    if (op.auth && !op.chave_api.empty())
        url_final = url_com_chave(url, *op.auth, op.chave_api);

    ListaHeaders headers;
    for (const auto& kv : op.headers)
        headers.adicionar(kv.first + ": " + kv.second);
    if (op.auth) aplicar_auth(curl, headers, *op.auth, op.chave_api, url_final);

    CtxEscrita escrita;
    escrita.fn = &cb.escrita;
    escrita.corpo = &r.corpo;

    CtxProgresso progresso;
    progresso.fn = &cb.progresso;

    std::string metodo_u = minusculas(op.metodo);
    bool tem_corpo = !op.corpo.empty() && metodo_u != "get" && metodo_u != "head";
    bool tem_json = tem_corpo && (op.corpo.front() == '{' || op.corpo.front() == '[');
    bool tem_ct = false;
    for (const auto& kv : op.headers)
        if (minusculas(kv.first) == "content-type") tem_ct = true;
    if (tem_json && !tem_ct) headers.adicionar("Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url_final.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cb_escrita);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &escrita);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ler_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);

    double timeout = op.timeout_sec > 0.0 ? op.timeout_sec : CFG.http_timeout;
    if (timeout > 0.0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout * 1000.0));
    if (op.connect_timeout_sec > 0.0)
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(op.connect_timeout_sec * 1000.0));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     op.user_agent_navegador
                         ? "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                           "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
                         : "download-generico/2.0");
    if (op.aceitar_gzip) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (op.verificar_ssl) {
        aplicar_ca_bundle(curl);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (!op.range.empty()) curl_easy_setopt(curl, CURLOPT_RANGE, op.range.c_str());
    if (headers.p) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.p);
    if (cb.progresso) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cb_progresso);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progresso);
    }

    if (metodo_u == "post") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, op.corpo.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(op.corpo.size()));
    } else if (metodo_u != "get" && metodo_u != "head") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, op.metodo.c_str());
        if (tem_corpo) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, op.corpo.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(op.corpo.size()));
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.erro_rede = true;
        r.erro_mensagem = curl_easy_strerror(rc);
        return r;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
}

RespostaHttp http_requisicao(const std::string& url, const OpcoesHttp& op) {
    return http_executar(url, op);
}

ResultadoDownload http_baixar_arquivo(const OpcoesDownload& op) {
    ResultadoDownload res;
    auto inicio = std::chrono::steady_clock::now();
    auto concluir = [&]() {
        res.segundos = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - inicio).count();
        if (op.pulado_por_tamanho) *op.pulado_por_tamanho = res.pulado_tamanho;
        return res;
    };

    if (op.pulado_por_tamanho) *op.pulado_por_tamanho = false;
    long long tamanho_min = std::max(0LL, op.tamanho_min);
    long long tamanho_max = std::max(0LL, op.tamanho_max);

    fs::path destino(expandir_caminho(op.caminho));
    std::error_code ec;
    if (!destino.parent_path().empty()) fs::create_directories(destino.parent_path(), ec);

    if (fs::exists(destino, ec)) {
        auto sz = fs::file_size(destino, ec);
        if (!ec && sz > 0 &&
            (tamanho_min <= 0 || sz >= static_cast<unsigned long long>(tamanho_min))) {
            res.bytes = static_cast<long long>(sz);
            if (tamanho_max > 0 && sz > static_cast<unsigned long long>(tamanho_max)) {
                res.pulado_tamanho = true;
                if (op.logar_skip)
                    log("[SKIP] " + op.tag + " excede tamanho_max (" +
                        formatar_bytes(tamanho_max) + "): " + op.url);
                return concluir();
            }
            if (op.permitir_reuso) {
                res.ok = true;
                res.ja_existia = true;
                return concluir();
            }
            fs::remove(destino, ec);
        } else if (!op.permitir_reuso) {
            fs::remove(destino, ec);
        }
    }

    fs::path temporario = destino;
    temporario += ".temp";
    fs::remove(temporario, ec);

    AsyncFileWriter writer(temporario.string());
    if (!writer.good()) {
        res.erro = "nao foi possivel criar arquivo temporario";
        return concluir();
    }

    struct EstadoEscrita {
        AsyncFileWriter* writer = nullptr;
        long long max = 0;
        long long escrito = 0;
        bool estourou = false;
    } estado{&writer, tamanho_max, 0, false};

    CallbacksHttp cb;
    ProgressoDownload prog;
    prog.tag = op.tag.empty() ? obter_nome_arquivo_da_url(op.url) : op.tag;
    prog.logar = op.progresso;

    cb.escrita = [&estado](char* ptr, size_t n) -> size_t {
        if (estado.max > 0 &&
            estado.escrito + static_cast<long long>(n) > estado.max) {
            estado.estourou = true;
            return 0;
        }
        if (!estado.writer->push(ptr, n)) return 0;
        estado.escrito += static_cast<long long>(n);
        return n;
    };
    cb.progresso = [&](long long total, long long atual) -> int {
        if (tamanho_max > 0 &&
            ((total > 0 && total > tamanho_max) || atual > tamanho_max)) {
            estado.estourou = true;
            return 1;
        }
        if (prog.logar) log_progresso(prog, total, atual);
        if (op.ao_progresso) op.ao_progresso(atual, total);
        return 0;
    };

    OpcoesHttp req;
    req.headers = op.headers;
    req.auth = op.auth;
    req.chave_api = op.chave_api;
    req.timeout_sec = op.timeout_sec;
    req.verificar_ssl = op.verificar_ssl;
    req.user_agent_navegador = op.user_agent_navegador;
    RespostaHttp resp = http_executar(op.url, req, cb);
    writer.close();

    auto remover_temp = [&]() {
        std::error_code e;
        fs::remove(temporario, e);
    };

    if (estado.estourou || (tamanho_max > 0 && resp.content_length > tamanho_max)) {
        remover_temp();
        res.pulado_tamanho = true;
        if (op.logar_skip)
            log("[SKIP] " + prog.tag + " excede tamanho_max (" +
                formatar_bytes(tamanho_max) + "): " + op.url);
        return concluir();
    }

    res.status = resp.status;
    if (resp.erro_rede) {
        remover_temp();
        res.erro = resp.erro_mensagem;
        return concluir();
    }
    if (!writer.good()) {
        remover_temp();
        res.erro = "falha ao gravar arquivo";
        return concluir();
    }
    if (resp.status < 200 || resp.status >= 300) {
        std::ifstream erro_in(temporario, std::ios::binary);
        if (erro_in) {
            char buffer[512];
            erro_in.read(buffer, sizeof(buffer));
            res.corpo_erro.assign(buffer, static_cast<size_t>(erro_in.gcount()));
        }
        remover_temp();
        res.erro = "HTTP " + std::to_string(resp.status);
        return concluir();
    }

    unsigned long long sz = fs::file_size(temporario, ec);
    if (ec || sz == 0 ||
        (tamanho_min > 0 && sz < static_cast<unsigned long long>(tamanho_min))) {
        remover_temp();
        res.erro = "tamanho invalido (" + std::to_string(ec ? 0 : sz) + " bytes)";
        return concluir();
    }
    if (tamanho_max > 0 && sz > static_cast<unsigned long long>(tamanho_max)) {
        remover_temp();
        res.pulado_tamanho = true;
        if (op.logar_skip)
            log("[SKIP] " + prog.tag + " excede tamanho_max (" +
                formatar_bytes(tamanho_max) + "): " + op.url);
        return concluir();
    }

    if (op.detectar_pagina_erro && sz <= 1024ULL * 1024ULL) {
        std::ifstream in(temporario, std::ios::binary);
        std::string inicio_str;
        inicio_str.resize(static_cast<size_t>(std::min<unsigned long long>(sz, 512)));
        if (in) {
            in.read(inicio_str.data(), static_cast<std::streamsize>(inicio_str.size()));
            inicio_str.resize(static_cast<size_t>(in.gcount()));
        }
        if (inicio_str.rfind("<?xml", 0) == 0 || inicio_str.rfind("<html", 0) == 0 ||
            inicio_str.rfind("<!DOCTYPE", 0) == 0 || inicio_str.rfind("{\"error\"", 0) == 0 ||
            inicio_str.rfind("{\"code\"", 0) == 0) {
            res.pagina_erro = true;
            res.corpo_erro = inicio_str.substr(0, 512);
            remover_temp();
            return concluir();
        }
    }

    fs::remove(destino, ec);
    fs::rename(temporario, destino, ec);
    if (ec) {
        remover_temp();
        res.erro = "falha ao renomear: " + ec.message();
        return concluir();
    }
    res.ok = true;
    res.bytes = static_cast<long long>(sz);
    return concluir();
}

std::optional<json> baixar_com_retry(const std::string& url,
                                     const std::map<std::string, std::string>& params,
                                     int max_tentativas, double custo,
                                     const std::string& chave_api,
                                     const std::string& metodo,
                                     const std::map<std::string, std::string>& headers,
                                     const std::string& corpo,
                                     const AuthApi& auth) {
    OpcoesHttp op;
    op.metodo = metodo;
    op.headers = headers;
    op.corpo = corpo;

    AuthApi auth_efetiva = auth;
    if (auth_efetiva.modo.empty()) auth_efetiva.modo = "auto";
    std::map<std::string, std::string> params_req = params;
    std::string modo = minusculas(auth_efetiva.modo);
    if (!chave_api.empty() && modo != "none") {
        if (modo == "query") params_req.erase(auth_efetiva.param);
        op.auth = &auth_efetiva;
        op.chave_api = chave_api;
    }
    std::string url_completa = montar_url(url, params_req);
    std::string grupo = chave_api.empty() ? std::string("anonymous") : chave_api;

    for (int tentativa = 1; tentativa <= max_tentativas; ++tentativa) {
        esperar_envio(grupo, custo);

        RespostaHttp r = http_requisicao(url_completa, op);

        if (!r.erro_rede && r.status < 400) {
            try {
                restaurar_taxa();
                return json::parse(r.corpo);
            } catch (const std::exception& e) {
                if (tentativa == max_tentativas) {
                    log("[ERROR] Resposta sem JSON valido apos " +
                        std::to_string(max_tentativas) + " tentativas: " + e.what());
                    return std::nullopt;
                }
                double espera = backoff_exponencial(tentativa);
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "[RETRY %d/%d] Resposta sem JSON valido. Aguardando %.1fs...",
                              tentativa, max_tentativas, espera);
                log(buf);
                dormir(espera);
                continue;
            }
        }

        if (!r.erro_rede && r.status == 429) {
            std::string corpo_resp = minusculas(r.corpo.substr(0, 1000));
            bool quota = corpo_resp.find("daily") != std::string::npos ||
                         corpo_resp.find("per day") != std::string::npos ||
                         corpo_resp.find("hour") != std::string::npos ||
                         corpo_resp.find("minute") != std::string::npos ||
                         corpo_resp.find("month") != std::string::npos;
            double espera;
            if (r.retry_after > 0) espera = r.retry_after;
            else if (quota) espera = CFG.cooldown_quota;
            else espera = std::max(CFG.espera_min_429, backoff_exponencial(tentativa));

            tratar_429(espera, quota ? "cota da API excedida" : "HTTP 429", url);
            if (tentativa == max_tentativas) {
                log("[ERROR] 429 persistente apos " + std::to_string(max_tentativas) +
                    " tentativas");
                return std::nullopt;
            }
            char buf[220];
            std::snprintf(buf, sizeof(buf),
                          "[RETRY %d/%d] HTTP 429%s. Pausa global de %.0fs...",
                          tentativa, max_tentativas, quota ? " (cota)" : "", espera);
            log(buf);
        } else if (!r.erro_rede) {
            if (r.status == 400 || r.status == 404) {
                std::string motivo = r.corpo.substr(0, 300);
                log("[ERROR] HTTP " + std::to_string(r.status) +
                    " (sem retry): " + motivo + " :: " + url_completa);
                return std::nullopt;
            }
            if (tentativa == max_tentativas) {
                log("[ERROR] Falha apos " + std::to_string(max_tentativas) +
                    " tentativas: HTTP " + std::to_string(r.status));
                return std::nullopt;
            }
            double espera = backoff_exponencial(tentativa);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "[RETRY %d/%d] HTTP %ld. Aguardando %.1fs...",
                          tentativa, max_tentativas, r.status, espera);
            log(buf);
            dormir(espera);
        } else {
            if (tentativa == max_tentativas) {
                log("[ERROR] Falha de rede apos " + std::to_string(max_tentativas) +
                    " tentativas: " + r.erro_mensagem);
                return std::nullopt;
            }
            double espera = backoff_exponencial(tentativa);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "[RETRY %d/%d] %s. Aguardando %.1fs...",
                          tentativa, max_tentativas, r.erro_mensagem.c_str(), espera);
            log(buf);
            dormir(espera);
        }
    }
    return std::nullopt;
}
