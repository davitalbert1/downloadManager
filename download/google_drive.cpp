#include "google_drive.h"
#include "config.h"
#include "http_client.h"
#include "utils.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Progresso do dataset Drive: 1 linha viva por arquivo (via \r)
// Regras: infos de download sao POR ARQUIVO (tamanho, baixado, velocidade
// do arquivo, ETA do arquivo). O agregado do dataset (total geral, % geral)
// aparece só no resumo da pasta e no resumo final. Arquivo "ja existe" vira
// 1 linha curta, sem barra de progresso.
struct ProgressoDrive {
    std::atomic<unsigned long long> total_bytes{0}; // soma dos tamanhos conhecidos
    std::atomic<unsigned long long> baixado_bytes{0}; // bytes de rede gravados
    std::atomic<unsigned long long> verificado_bytes{0}; // bytes de arq já existente
    std::atomic<size_t> total_arquivos{0};
    std::atomic<size_t> prontos_arquivos{0};
    std::chrono::steady_clock::time_point inicio = std::chrono::steady_clock::now();
    std::mutex mtx;
};

static unsigned long long drive_tamanho_do_item(const json& item) {
    if (item.contains("size")) {
        try {
            if (item["size"].is_number_unsigned()) return item["size"].get<unsigned long long>();
            if (item["size"].is_number()) return (unsigned long long)item["size"].get<long long>();
            if (item["size"].is_string()) return std::stoull(item["size"].get<std::string>());
        } catch (...) {}
    }
    return 0;
}

// Estado do download de UM arquivo: usado pelo callback de escrita e pela
// barra de progresso por arquivo. Deve ficar ANTES de drive_log_arquivo.
struct ProgressoArquivoDrive {
    FILE* file = nullptr;
    ProgressoDrive* global = nullptr;
    std::string nome;
    unsigned long long total = 0; // tamanho esperado do arquivo (0 = desconhecido)
    unsigned long long feito = 0; // bytes gravados neste arquivo
    unsigned long long tamanho_max = 0; // >0 = aborta quando exceder
    bool estourou_max = false; // limite excedido no meio do stream
    bool imprimiu = false; // barra ja desenhada (precisa fechar com '\n' em erro)
    std::chrono::steady_clock::time_point inicio = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point ultimo_log = std::chrono::steady_clock::now();
};

static void drive_log_arquivo(ProgressoDrive* p, const ProgressoArquivoDrive& a, bool fim) {
    // Barra única POR ARQUIVO, reescrita na mesma linha com \r:
    // [3/465] IMG_3948.JPG | 5.6/10.5 MB 53% | 4.1 MB/s | ETA 00:00:01
    // No fim (ok) quebra a linha. "ja existe" não passa por aqui.
    double dec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - a.inicio).count();
    if (dec < 0.05) dec = 0.05;
    double vel = (double)a.feito / dec; // B/s DESTE arquivo
    char buf[512];
    if (a.total > 0) {
        double pct = 100.0 * (double)a.feito / (double)a.total;
        if (pct > 100.0) pct = 100.0;
        double eta = vel > 1.0 ? ((double)(a.total > a.feito ? a.total - a.feito : 0)) / vel : -1.0;
        std::snprintf(buf, sizeof(buf),
            "[%zu/%zu] %s | %s/%s %5.1f%% | %s/s | ETA %s%c",
            p ? p->prontos_arquivos.load() + 1 : 0,
            p ? p->total_arquivos.load() : 0,
            a.nome.c_str(),
            formatar_bytes(a.feito).c_str(), formatar_bytes(a.total).c_str(),
            pct, formatar_bytes((unsigned long long)vel).c_str(),
            formatar_duracao(eta).c_str(), fim ? '\n' : '\r');
    } else {
        std::snprintf(buf, sizeof(buf),
            "[%zu/%zu] %s | %s baixados | %s/s%c",
            p ? p->prontos_arquivos.load() + 1 : 0,
            p ? p->total_arquivos.load() : 0,
            a.nome.c_str(), formatar_bytes(a.feito).c_str(),
            formatar_bytes((unsigned long long)vel).c_str(), fim ? '\n' : '\r');
    }
    if (p) {
        std::lock_guard<std::mutex> lk(p->mtx);
        fwrite(buf, 1, strlen(buf), stdout);
        fflush(stdout);
    } else {
        fwrite(buf, 1, strlen(buf), stdout);
        fflush(stdout);
    }
}

struct TokenAcessoDrive {
    std::string valor;
    std::time_t expira_em = 0;
};

struct ErroHttpDrive : std::runtime_error {
    long codigo;
    ErroHttpDrive(long status, const std::string& mensagem)
        : std::runtime_error(mensagem), codigo(status) {}
};

static constexpr std::time_t MARGEM_TOKEN = 60;

static bool token_vencendo(const TokenAcessoDrive& token) {
    return std::time(nullptr) + MARGEM_TOKEN >= token.expira_em;
}

static std::string drive_base64url_encode(const unsigned char* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* bio = BIO_new(BIO_s_mem());
    if (!b64 || !bio) {
        if (b64) BIO_free(b64);
        if (bio) BIO_free(bio);
        throw std::runtime_error("Falha ao inicializar Base64.");
    }
    b64 = BIO_push(b64, bio);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    if (BIO_write(b64, data, static_cast<int>(len)) <= 0) {
        BIO_free_all(b64);
        throw std::runtime_error("Falha ao codificar Base64.");
    }
    if (BIO_flush(b64) != 1) {
        BIO_free_all(b64);
        throw std::runtime_error("Falha ao finalizar Base64.");
    }
    BUF_MEM* bufferPtr = nullptr;
    BIO_get_mem_ptr(b64, &bufferPtr);
    if (!bufferPtr) {
        BIO_free_all(b64);
        throw std::runtime_error("Falha ao obter resultado Base64.");
    }
    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(b64);

    for (char& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

static std::string drive_base64url_encode(const std::string& input) {
    return drive_base64url_encode(reinterpret_cast<const unsigned char*>(input.data()), input.size());
}

static std::string drive_url_encode(const std::string& value) {
    return escapar_url(value);
}

static std::string drive_assinar_jwt(const std::string& header, const std::string& payload, const std::string& private_key) {
    const std::string header_encoded = drive_base64url_encode(header);
    const std::string payload_encoded = drive_base64url_encode(payload);
    const std::string mensagem = header_encoded + "." + payload_encoded;
    BIO* bio = BIO_new_mem_buf(private_key.data(), static_cast<int>(private_key.size()));
    if (!bio) throw std::runtime_error("Falha ao criar BIO da chave privada.");
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) throw std::runtime_error("Falha ao ler a chave privada do service_account.");

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Falha ao criar contexto OpenSSL.");
    }
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Falha no EVP_DigestSignInit.");
    }
    if (EVP_DigestSignUpdate(ctx, mensagem.data(), mensagem.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Falha no EVP_DigestSignUpdate.");
    }
    size_t signature_length = 0;
    if (EVP_DigestSignFinal(ctx, nullptr, &signature_length) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Falha ao calcular tamanho da assinatura.");
    }
    std::vector<unsigned char> signature(signature_length);
    if (EVP_DigestSignFinal(ctx, signature.data(), &signature_length) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Falha ao gerar assinatura JWT.");
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return (mensagem + "." + drive_base64url_encode(signature.data(), signature_length));
}

static std::string drive_http_post(const std::string& url, const std::string& body,
                                   const std::string& content_type) {
    OpcoesHttp op;
    op.metodo = "POST";
    op.corpo = body;
    op.headers["Content-Type"] = content_type;
    RespostaHttp r = http_requisicao(url, op);
    if (r.erro_rede)
        throw std::runtime_error("Erro CURL: " + r.erro_mensagem);
    if (r.status < 200 || r.status >= 300)
        throw std::runtime_error("HTTP " + std::to_string(r.status) + ": " + r.corpo);
    return r.corpo;
}

static std::string drive_http_get(const std::string& url, const std::string& access_token) {
    OpcoesHttp op;
    op.headers["Authorization"] = "Bearer " + access_token;
    RespostaHttp r = http_requisicao(url, op);
    if (r.erro_rede)
        throw std::runtime_error("Erro CURL: " + r.erro_mensagem);
    if (r.status < 200 || r.status >= 300)
        throw std::runtime_error("HTTP " + std::to_string(r.status) + ": " + r.corpo);
    return r.corpo;
}

static TokenAcessoDrive drive_obter_access_token(const json& service_account) {
    if (!service_account.contains("client_email")) {
        throw std::runtime_error("service_account nao possui client_email.");
    }
    if (!service_account.contains("private_key")) {
        throw std::runtime_error("service_account nao possui private_key.");
    }
    const std::string client_email = service_account.at("client_email").get<std::string>();
    const std::string private_key = service_account.at("private_key").get<std::string>();
    const std::string token_uri = service_account.value("token_uri", "https://oauth2.googleapis.com/token");
    const std::time_t agora = std::time(nullptr);
    json header = {
        {"alg", "RS256"},
        {"typ", "JWT"}
    };
    json payload = {
        {"iss", client_email},
        {"scope", "https://www.googleapis.com/auth/drive.readonly"},
        {"aud", token_uri},
        {"iat", agora},
        {"exp", agora + 3600}
    };
    std::string jwt = drive_assinar_jwt(header.dump(), payload.dump(), private_key);
    std::string body =
        "grant_type=" +
        drive_url_encode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
        "&assertion=" +
        drive_url_encode(jwt);
    std::string response = drive_http_post(token_uri, body, "application/x-www-form-urlencoded");
    json data;
    try {
        data = json::parse(response);
    }
    catch (...) {
        throw std::runtime_error("Resposta invalida do servidor OAuth:\n" + response);
    }
    if (!data.contains("access_token")) {
        throw std::runtime_error("Google nao retornou access_token:\n" + response);
    }
    TokenAcessoDrive token;
    token.valor = data.at("access_token").get<std::string>();
    token.expira_em = agora + static_cast<std::time_t>(data.value("expires_in", 3600));
    return token;
}

static std::vector<json> drive_listar_arquivos(const std::string& folder_id, const std::string& access_token) {
    std::vector<json> arquivos;
    std::string page_token;
    do {
        std::string query = "'" + folder_id + "' in parents and trashed = false";
        std::string url = "https://www.googleapis.com/drive/v3/files";
        url += "?q=" + drive_url_encode(query);
        url += "&fields=" + drive_url_encode("nextPageToken,files(id,name,mimeType,size)");
        url += "&pageSize=1000";
        if (!page_token.empty()) url += "&pageToken=" + drive_url_encode(page_token);
        log("[DRIVE] Consultando listagem de arquivos da pasta " + folder_id + "...");
        std::string response = drive_http_get(url, access_token);
        json data;
        try {
            data = json::parse(response);
        }
        catch (...) {
            throw std::runtime_error("Google retornou JSON invalido:\n" + response);
        }
        if (data.contains("files") && data["files"].is_array()) {
            for (const auto& arquivo : data["files"]) arquivos.push_back(arquivo);
        }
        page_token = data.value("nextPageToken", "");
    } while (!page_token.empty());
    return arquivos;
}

static void drive_baixar_arquivo(const json& arquivo, const fs::path& destino, const std::string& access_token, long long min_bytes = 0,
                               long long max_bytes = 0, bool* pulado_por_tamanho = nullptr,
                               ProgressoDrive* progresso = nullptr) {
    if (pulado_por_tamanho) *pulado_por_tamanho = false;
    if (max_bytes < 0) max_bytes = 0;
    if (!arquivo.contains("id") || !arquivo.contains("name")) {
        throw std::runtime_error("Arquivo retornado pelo Google nao possui id ou name.");
    }
    const std::string id = arquivo.at("id").get<std::string>();
    const std::string nome = arquivo.at("name").get<std::string>();
    const std::string mime = arquivo.value("mimeType", "");
    if (mime == "application/vnd.google-apps.folder") {
        log("[DRIVE] Ignorando pasta: " + nome);
        return;
    }
    if (mime.rfind("application/vnd.google-apps.", 0) == 0) {
        log("[DRIVE] Ignorando arquivo nativo doc/sheet Google: " + nome);
        return;
    }
    fs::path nome_seguro = fs::path(sanitizar_nome_arquivo(nome)).filename();
    if (nome_seguro.empty()) throw std::runtime_error("Nome de arquivo invalido.");
    fs::path caminho_final = destino / nome_seguro;

    unsigned long long tam_esperado = drive_tamanho_do_item(arquivo);

    if (max_bytes > 0 && tam_esperado > static_cast<unsigned long long>(max_bytes)) {
        if (pulado_por_tamanho) *pulado_por_tamanho = true;
        if (progresso) progresso->prontos_arquivos.fetch_add(1);
        log("[DRIVE SKIP] " + nome + " (" + formatar_bytes(tam_esperado) +
            ") excede tamanho_max (" + formatar_bytes((unsigned long long)max_bytes) + ")");
        return;
    }

    if (tam_esperado > 0)
        log("[DRIVE BAIXANDO] " + nome + " (" + formatar_bytes(tam_esperado) + ") -> " +
            caminho_final.string());
    else
        log("[DRIVE BAIXANDO] " + nome + " (tamanho desconhecido) -> " + caminho_final.string());

    ProgressoArquivoDrive ctx_prog;
    ctx_prog.global = progresso;
    ctx_prog.nome = nome;
    ctx_prog.total = tam_esperado;

    OpcoesDownload op;
    op.url = "https://www.googleapis.com/drive/v3/files/" + drive_url_encode(id) + "?alt=media";
    op.caminho = caminho_final.string();
    op.tag = nome;
    op.headers["Authorization"] = "Bearer " + access_token;
    op.tamanho_min = min_bytes;
    op.tamanho_max = max_bytes;
    op.permitir_reuso = true;
    op.progresso = false;
    op.logar_skip = false;
    op.ao_progresso = [&](long long atual, long long total) {
        if (atual > 0) {
            auto novo = static_cast<unsigned long long>(atual);
            if (novo > ctx_prog.feito) {
                if (progresso) progresso->baixado_bytes.fetch_add(novo - ctx_prog.feito);
                ctx_prog.feito = novo;
            }
        }
        if (total > 0) ctx_prog.total = static_cast<unsigned long long>(total);
        auto agora = std::chrono::steady_clock::now();
        double desde_log = std::chrono::duration<double>(agora - ctx_prog.ultimo_log).count();
        if (progresso && desde_log >= 0.5) {
            ctx_prog.ultimo_log = agora;
            drive_log_arquivo(progresso, ctx_prog, false);
            ctx_prog.imprimiu = true;
        }
    };

    ResultadoDownload r = http_baixar_arquivo(op);

    if (r.ja_existia) {
        if (progresso) {
            progresso->baixado_bytes.fetch_add(static_cast<unsigned long long>(r.bytes));
            progresso->prontos_arquivos.fetch_add(1);
        }
        // "ja existe" = 1 linha curta, sem barra de progresso.
        log("[DRIVE JA EXISTE] " + nome + " (" +
            formatar_bytes(static_cast<unsigned long long>(r.bytes)) + ")");
        return;
    }
    if (r.pulado_tamanho) {
        if (ctx_prog.imprimiu) {
            fputc('\n', stdout);
            fflush(stdout);
        }
        if (pulado_por_tamanho) *pulado_por_tamanho = true;
        if (progresso) progresso->prontos_arquivos.fetch_add(1);
        log("[DRIVE SKIP] " + nome + " excede tamanho_max (" +
            formatar_bytes(static_cast<unsigned long long>(max_bytes)) + ")");
        return;
    }
    if (!r.ok) {
        if (ctx_prog.imprimiu) {
            fputc('\n', stdout);
            fflush(stdout);
        }
        if (r.status >= 400) {
            std::string mensagem = "Google Drive retornou HTTP " + std::to_string(r.status);
            if (!r.corpo_erro.empty()) mensagem += ": " + r.corpo_erro;
            throw ErroHttpDrive(r.status, mensagem);
        }
        throw std::runtime_error("Erro CURL ao baixar: " + r.erro);
    }

    if (progresso) {
        // Finaliza a barra do arquivo com '\n' e credita no progresso global.
        drive_log_arquivo(progresso, ctx_prog, true);
        progresso->prontos_arquivos.fetch_add(1);
    } else {
        log("[DRIVE OK] Concluido: " + nome);
    }
}

static void drive_remover_temporarios(const fs::path& pasta) {
    std::error_code ec;
    if (!fs::exists(pasta, ec)) return;
    for (auto it = fs::recursive_directory_iterator(pasta, fs::directory_options::skip_permission_denied, ec);
        it != fs::recursive_directory_iterator();
        it.increment(ec)
    ) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec)) {
            std::string nome = it->path().filename().string();
            if (nome.size() >= 5 && nome.substr(nome.size() - 5) == ".temp") {
                fs::remove(it->path(), ec);
            }
        }
    }
}

static std::mutex g_drive_token_mtx;

// Copia o token sob lock: a renovação pode acontecer em outra thread.
static std::string drive_token_valido(TokenAcessoDrive& token, const json& service_account) {
    std::lock_guard<std::mutex> lk(g_drive_token_mtx);
    if (token_vencendo(token)) {
        log("[DRIVE] Token de acesso renovando...");
        try {
            token = drive_obter_access_token(service_account);
        } catch (const std::exception& e) {
            log(std::string("[ERRO TOKEN DRIVE] ") + e.what());
        }
    }
    return token.valor;
}

// Baixa um item do Drive em DFS por pasta: ao entrar numa pasta, lista os
// filhos UMA vez, baixa primeiro todos os arquivos dela e SÓ DEPOIS desce
// para cada subpasta (uma de cada vez). Sem pré-varredura global: o total
// (%/ETA) é estimado de forma incremental a partir dos sizes conhecidos.
// download_workers = nº de threads paralelas p/ arquivos dentro da pasta.
// process_workers é reservado p/ futuro pós-processamento (ex: checksum/index).
static void drive_baixar_item_recursivo(const json& item, const fs::path& destino_local,
                                        TokenAcessoDrive& token, const json& service_account,
                                        long long tamanho_min_bytes, long long tamanho_max_bytes,
                                        std::atomic<size_t>& sucesso,
                                        std::atomic<size_t>& erros,
                                        int download_workers,
                                        std::atomic<size_t>* pulados = nullptr,
                                        ProgressoDrive* progresso = nullptr,
                                        int profundidade = 0) {
    if (profundidade > 64) {
        erros.fetch_add(1);
        log("[ERRO DRIVE] Profundidade maxima excedida (possivel ciclo).");
        return;
    }
    if (!item.contains("id") || !item.contains("name")) {
        erros.fetch_add(1);
        log("[ERRO ARQUIVO DRIVE] Item sem id/name retornado pelo Google.");
        return;
    }
    const std::string id = item.at("id").get<std::string>();
    const std::string nome = item.at("name").get<std::string>();
    const std::string mime = item.value("mimeType", "");

    if (mime == "application/vnd.google-apps.folder") {
        fs::path sub = destino_local / fs::path(sanitizar_nome_arquivo(nome)).filename();
        std::error_code ec;
        fs::create_directories(sub, ec);
        if (ec) {
            erros.fetch_add(1);
            log("[ERRO PASTA DRIVE] " + nome + ": " + ec.message());
            return;
        }
        log("[DRIVE PASTA] " + sub.string());
        std::vector<json> filhos;
        try {
            filhos = drive_listar_arquivos(id, drive_token_valido(token, service_account));
        } catch (const std::exception& e) {
            erros.fetch_add(1);
            log(std::string("[ERRO LISTAR DRIVE] ") + nome + ": " + e.what());
            return;
        }
        // DFS por pasta: separa arquivos e subpastas da listagem ÚNICA;
        // soma os sizes conhecidos ao total incremental; baixa TODOS os
        // arquivos desta pasta antes de descer para a próxima subpasta.
        std::vector<json> subpastas;
        subpastas.reserve(filhos.size());
        if (progresso) {
            for (const auto& f : filhos) {
                if (f.value("mimeType", "") == "application/vnd.google-apps.folder") {
                    subpastas.push_back(f);
                    continue;
                }
                if (f.value("mimeType", "").rfind("application/vnd.google-apps.", 0) == 0) continue;
                progresso->total_arquivos.fetch_add(1);
                progresso->total_bytes.fetch_add(drive_tamanho_do_item(f));
            }
        } else {
            for (const auto& f : filhos)
                if (f.value("mimeType", "") == "application/vnd.google-apps.folder")
                    subpastas.push_back(f);
        }
        log("[DRIVE] " + std::to_string(filhos.size()) + " itens em '" + nome +
            "' (" + std::to_string(filhos.size() - subpastas.size()) + " arquivos, " +
            std::to_string(subpastas.size()) + " subpastas)");
        // Snapshot antes do conteúdo desta pasta: resumo por pasta no fim.
        size_t prontos_antes = progresso ? progresso->prontos_arquivos.load() : sucesso.load();
        unsigned long long bytes_antes = progresso ? progresso->baixado_bytes.load() : 0;
        auto inicio_pasta = std::chrono::steady_clock::now();
        // Arquivos da pasta em paralelo (download_workers); subpastas em
        // sequência (DFS por pasta) para não explodir nº de threads.
        std::vector<json> arquivos_pasta;
        arquivos_pasta.reserve(filhos.size());
        for (const auto& f : filhos) {
            if (f.value("mimeType", "") == "application/vnd.google-apps.folder") continue;
            if (f.value("mimeType", "").rfind("application/vnd.google-apps.", 0) == 0) continue;
            arquivos_pasta.push_back(f);
        }
        int n_dl = download_workers < 1 ? 1 : download_workers;
        if ((size_t)n_dl > arquivos_pasta.size() && !arquivos_pasta.empty())
            n_dl = (int)arquivos_pasta.size();
        log("[DRIVE] download_workers=" + std::to_string(n_dl) +
            " process_workers=" + std::to_string(CFG.process_workers) +
            " p/ " + std::to_string(arquivos_pasta.size()) + " arquivos em '" + nome + "'");
        {
            std::atomic<size_t> idx{0};
            std::vector<std::thread> th;
            th.reserve((size_t)n_dl);
            for (int i = 0; i < n_dl; ++i) {
                th.emplace_back([&] {
                    while (true) {
                        size_t k = idx.fetch_add(1);
                        if (k >= arquivos_pasta.size()) break;
                        drive_baixar_item_recursivo(arquivos_pasta[k], sub, token, service_account,
                                                    tamanho_min_bytes, tamanho_max_bytes, sucesso, erros,
                                                    download_workers, pulados, progresso, profundidade + 1);
                    }
                });
            }
            for (auto& t : th) t.join();
        }
        for (const auto& s : subpastas)
            drive_baixar_item_recursivo(s, sub, token, service_account,
                                        tamanho_min_bytes, tamanho_max_bytes, sucesso, erros,
                                        download_workers, pulados, progresso, profundidade + 1);
        {
            // Resumo da pasta: consolidado dos arquivos concluídos aqui dentro.
            double dec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - inicio_pasta).count();
            std::string resumo = "[DRIVE PASTA OK] " + nome + ": " +
                std::to_string((progresso ? progresso->prontos_arquivos.load() : sucesso.load()) - prontos_antes) +
                " arquivos, " +
                formatar_bytes((progresso ? progresso->baixado_bytes.load() : 0ULL) - bytes_antes) +
                " em " + formatar_duracao(dec);
            log(resumo);
        }
        return;
    }

    for (int tentativa = 0; tentativa < 2; ++tentativa) {
        try {
            bool pulado_max = false;
            drive_baixar_arquivo(item, destino_local, drive_token_valido(token, service_account),
                                 tamanho_min_bytes, tamanho_max_bytes,
                                 &pulado_max, progresso);
            if (pulado_max) {
                if (pulados) pulados->fetch_add(1);
            } else {
                sucesso.fetch_add(1);
            }
            return;
        } catch (const ErroHttpDrive& e) {
            if (e.codigo == 401 && tentativa == 0) {
                log("[DRIVE HTTP 401] Renovando token de acesso...");
                try {
                    std::lock_guard<std::mutex> lk(g_drive_token_mtx);
                    token = drive_obter_access_token(service_account);
                } catch (...) {}
                continue;
            }
            erros.fetch_add(1);
            log(std::string("[ERRO ARQUIVO DRIVE] ") + e.what());
            return;
        } catch (const std::exception& e) {
            erros.fetch_add(1);
            log(std::string("[ERRO ARQUIVO DRIVE] ") + e.what());
            return;
        }
    }
}

bool executar_dataset_google_drive(const DatasetCfg& ds) {
    std::string folder_id = ds.drive_folder_id;
    if (folder_id.empty()) {
        auto it_p = ds.params.find("folder_id");
        if (it_p != ds.params.end()) folder_id = it_p->second;
    }
    if (folder_id.empty()) folder_id = obter_env_ou_config("DRIVE_FOLDER_ID", "", "");

    std::string sa_file = ds.drive_service_account;
    if (sa_file.empty()) {
        auto it_p = ds.params.find("service_account");
        if (it_p != ds.params.end()) sa_file = it_p->second;
    }
    if (sa_file.empty()) sa_file = obter_env_ou_config("DRIVE_SERVICE_ACCOUNT", "", "service_account.json");

    std::string dest_str = resolver_pasta_saida(ds.pasta_saida, "dados_drive", "PASTA_SAIDA");
    fs::path dest(dest_str);

    log("============================================================");
    log("DOWNLOAD GOOGLE DRIVE: " + ds.id);
    log("Folder ID: " + (folder_id.empty() ? "(vazio)" : folder_id));
    log("Destino: " + dest.string());
    log("Service Account: " + sa_file);
    log("============================================================");

    if (folder_id.empty()) {
        log("[ERRO] Nenhum folder_id configurado para o Google Drive em " + ds.id);
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        log("[ERRO] Falha ao criar pasta de destino: " + ec.message());
        return false;
    }
    drive_remover_temporarios(dest);

    // Localiza service_account.json
    fs::path sa_path = sa_file;
    if (!fs::exists(sa_path, ec)) sa_path = fs::path("..") / sa_file;
    if (!fs::exists(sa_path, ec)) {
        log("[ERRO] service_account.json nao encontrado em " + fs::path(sa_file).string() + " nem em " + sa_path.string());
        return false;
    }

    std::ifstream in(sa_path);
    if (!in) {
        log("[ERRO] Nao foi possivel abrir service_account.json.");
        return false;
    }
    json service_account;
    try {
        service_account = json::parse(in);
    } catch (const std::exception& e) {
        log(std::string("[ERRO] JSON da Service Account invalido: ") + e.what());
        return false;
    }

    std::string client_email = service_account.value("client_email", "");
    if (client_email.empty()) {
        log("[ERRO] client_email nao encontrado no JSON da Service Account.");
        return false;
    }
    log("Service Account autenticada: " + client_email);

    TokenAcessoDrive token;
    try {
        token = drive_obter_access_token(service_account);
    } catch (const std::exception& e) {
        log(std::string("[ERRO] Falha na autenticacao OAuth Google: ") + e.what());
        return false;
    }

    std::vector<json> arquivos;
    try {
        arquivos = drive_listar_arquivos(folder_id, drive_token_valido(token, service_account));
    } catch (const std::exception& e) {
        log(std::string("[ERRO] Falha ao listar arquivos do Google Drive: ") + e.what());
        return false;
    }

    log("Total de itens encontrados na pasta do Drive: " + std::to_string(arquivos.size()));
    if (arquivos.empty()) {
        log("[AVISO] Nenhum arquivo encontrado no Drive. Verifique se a pasta foi compartilhada com " + client_email);
        return true;
    }

    // DFS por pasta a partir da raiz: sem pré-varredura global. O progresso
    // total (%/ETA) cresce de forma incremental à medida que cada pasta é
    // listada (sizes conhecidos somados na entrada da pasta).
    ProgressoDrive progresso;
    progresso.inicio = std::chrono::steady_clock::now();
    log("[DRIVE] Baixando pasta por pasta (conclui uma antes de ir p/ proxima)...");

    size_t sucesso = 0, erros = 0, pulados = 0;
    {
        std::vector<json> subpastas_raiz;
        subpastas_raiz.reserve(arquivos.size());
        std::vector<json> arquivos_raiz;
        arquivos_raiz.reserve(arquivos.size());
        for (const auto& a : arquivos) {
            if (a.value("mimeType", "") == "application/vnd.google-apps.folder") {
                subpastas_raiz.push_back(a);
                continue;
            }
            if (a.value("mimeType", "").rfind("application/vnd.google-apps.", 0) == 0) continue;
            // Skip antecipado: metadado size ja diz que excede o limite (sem baixar nada)
            if (ds.tamanho_max_bytes > 0) {
                unsigned long long sz = drive_tamanho_do_item(a);
                if (sz > static_cast<unsigned long long>(ds.tamanho_max_bytes)) {
                    ++pulados;
                    progresso.prontos_arquivos.fetch_add(1);
                    log("[DRIVE SKIP] " + a.value("name", "?") + " (" + formatar_bytes(sz) +
                        ") excede tamanho_max (" +
                        formatar_bytes((unsigned long long)ds.tamanho_max_bytes) + ")");
                    continue;
                }
            }
            arquivos_raiz.push_back(a);
            progresso.total_arquivos.fetch_add(1);
            progresso.total_bytes.fetch_add(drive_tamanho_do_item(a));
        }
        log("[DRIVE] Raiz: " + std::to_string(arquivos.size()) + " itens (" +
            std::to_string(arquivos_raiz.size()) + " arquivos, " +
            std::to_string(subpastas_raiz.size()) + " subpastas" +
            (pulados ? ", " + std::to_string(pulados) + " pulados (tamanho)" : "") + ")");
        int n_dl = CFG.download_workers < 1 ? 1 : CFG.download_workers;
        if ((size_t)n_dl > arquivos_raiz.size() && !arquivos_raiz.empty())
            n_dl = (int)arquivos_raiz.size();
        std::atomic<size_t> suc_at{0}, err_at{0}, pul_at{0};
        log("[DRIVE] download_workers=" + std::to_string(n_dl) +
            " process_workers=" + std::to_string(CFG.process_workers) +
            " p/ raiz (" + std::to_string(arquivos_raiz.size()) + " arquivos)");
        {
            std::atomic<size_t> idx{0};
            std::vector<std::thread> th;
            th.reserve((size_t)std::max(1, n_dl));
            for (int i = 0; i < n_dl; ++i) {
                th.emplace_back([&] {
                    while (true) {
                        size_t k = idx.fetch_add(1);
                        if (k >= arquivos_raiz.size()) break;
                        drive_baixar_item_recursivo(arquivos_raiz[k], dest, token, service_account,
                                                    ds.tamanho_min_bytes, ds.tamanho_max_bytes, suc_at, err_at,
                                                    n_dl, &pul_at, &progresso);
                    }
                });
            }
            for (auto& t : th) t.join();
        }
        for (const auto& s : subpastas_raiz)
            drive_baixar_item_recursivo(s, dest, token, service_account,
                                        ds.tamanho_min_bytes, ds.tamanho_max_bytes, suc_at, err_at,
                                        CFG.download_workers < 1 ? 1 : CFG.download_workers,
                                        &pul_at, &progresso);
        sucesso = suc_at.load();
        erros = err_at.load();
        pulados += pul_at.load();
    }

    {
        double dec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - progresso.inicio).count();
        if (dec < 0.05) dec = 0.05;
        double vel = (double)progresso.baixado_bytes.load() / dec;
        log("------------------------------------------------------------");
        log("CONCLUIDO DRIVE: " + std::to_string(sucesso) + " baixados/verificados, " +
            std::to_string(pulados) + " pulados (tamanho), " +
            std::to_string(erros) + " erros em " + formatar_duracao(dec) +
            " | " + formatar_bytes(progresso.baixado_bytes.load()) +
            " a " + formatar_bytes((unsigned long long)vel) + "/s");
        log("============================================================");
    }
    return erros == 0;
}
