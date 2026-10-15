#include "google_drive.h"
#include "config.h"
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
#include <curl/curl.h>
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

static std::string drive_formatar_bytes(unsigned long long b) {
    const char* un[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, un[u]);
    return buf;
}

static std::string drive_formatar_tempo(double seg) {
    if (!(seg >= 0.0) || seg > 99 * 3600.0) return "--:--:--";
    long s = (long)seg;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
    return buf;
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
            "  [%zu/%zu] %s | %s/%s %5.1f%% | %s/s | ETA %s%c",
            p ? p->prontos_arquivos.load() + 1 : 0,
            p ? p->total_arquivos.load() : 0,
            a.nome.c_str(),
            drive_formatar_bytes(a.feito).c_str(), drive_formatar_bytes(a.total).c_str(),
            pct, drive_formatar_bytes((unsigned long long)vel).c_str(),
            drive_formatar_tempo(eta).c_str(), fim ? '\n' : '\r');
    } else {
        std::snprintf(buf, sizeof(buf),
            "[%zu/%zu] %s | %s baixados | %s/s%c",
            p ? p->prontos_arquivos.load() + 1 : 0,
            p ? p->total_arquivos.load() : 0,
            a.nome.c_str(), drive_formatar_bytes(a.feito).c_str(),
            drive_formatar_bytes((unsigned long long)vel).c_str(), fim ? '\n' : '\r');
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

static std::string drive_find_ca_bundle() {
    const char* env = std::getenv("CURL_CA_BUNDLE");
    if (env && *env && fs::exists(fs::path(env))) return env;
    const std::vector<std::string> candidates = {
        R"(D:\msys64\mingw64\etc\ssl\certs\ca-bundle.crt)",
        R"(C:\msys64\mingw64\etc\ssl\certs\ca-bundle.crt)",
        R"(D:\msys64\mingw64\etc\ssl\cert.pem)",
        R"(C:\msys64\mingw64\etc\ssl\cert.pem)",
        R"(D:\msys64\usr\etc\ssl\certs\ca-bundle.crt)",
        R"(C:\msys64\usr\etc\ssl\certs\ca-bundle.crt)",
        R"(D:\msys64\usr\ssl\certs\ca-bundle.crt)",
        R"(C:\msys64\usr\ssl\certs\ca-bundle.crt)",
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(fs::path(c), ec)) return c;
    }
    return "";
}

static void drive_aplicar_ca_bundle(CURL* curl) {
    static const std::string ca_bundle = drive_find_ca_bundle();
    if (!ca_bundle.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
    } else {
        curl_easy_setopt(curl, CURLOPT_CAPATH, nullptr);
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    }
}

static size_t drive_write_string_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    auto* output = static_cast<std::string*>(userp);
    output->append(static_cast<char*>(contents), total);
    return total;
}

// Callback de escrita com progresso: grava, conta bytes, atualiza o total
// global e redesenha a barra POR ARQUIVO (via \r) a cada ~500ms.
static size_t drive_write_file_progress_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* ctx = static_cast<ProgressoArquivoDrive*>(userp);
    if (!ctx || !ctx->file) return 0;
    size_t n = size * nmemb;
    if (ctx->tamanho_max > 0 && ctx->feito + (unsigned long long)n > ctx->tamanho_max) {
        ctx->estourou_max = true;
        return 0; // aborta (CURLE_WRITE_ERROR) sem gravar o excesso
    }
    size_t gravados = fwrite(contents, 1, n, ctx->file);
    ctx->feito += (unsigned long long)gravados;
    if (ctx->global) ctx->global->baixado_bytes.fetch_add((unsigned long long)gravados);
    auto agora = std::chrono::steady_clock::now();
    double desde_log = std::chrono::duration<double>(agora - ctx->ultimo_log).count();
    if (desde_log >= 0.5 && ctx->global) {
        ctx->ultimo_log = agora;
        drive_log_arquivo(ctx->global, *ctx, false);
        ctx->imprimiu = true;
    }
    return gravados;
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
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Falha ao inicializar CURL para URL encode.");
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
    if (!encoded) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Falha ao fazer URL encode.");
    }
    std::string result(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
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

static std::string drive_http_post(const std::string& url, const std::string& body, const std::string& content_type) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Nao foi possivel inicializar CURL.");
    drive_aplicar_ca_bundle(curl);
    std::string response;
    struct curl_slist* headers = nullptr;
    std::string content_header = "Content-Type: " + content_type;
    headers = curl_slist_append(headers, content_header.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, drive_write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string("Erro CURL: ") + curl_easy_strerror(result));
    }
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(http_code) + ": " + response);
    }
    return response;
}

static std::string drive_http_get(const std::string& url, const std::string& access_token) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Nao foi possivel inicializar CURL.");
    drive_aplicar_ca_bundle(curl);
    std::string response;
    struct curl_slist* headers = nullptr;
    std::string authorization = "Authorization: Bearer " + access_token;
    headers = curl_slist_append(headers, authorization.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, drive_write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string("Erro CURL: ") + curl_easy_strerror(result));
    }
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(http_code) + ": " + response);
    }
    return response;
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
        log("  [DRIVE] Consultando listagem de arquivos da pasta " + folder_id + "...");
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
        log("  [DRIVE] Ignorando pasta: " + nome);
        return;
    }
    if (mime.rfind("application/vnd.google-apps.", 0) == 0) {
        log("  [DRIVE] Ignorando arquivo nativo doc/sheet Google: " + nome);
        return;
    }
    fs::path nome_seguro = fs::path(sanitizar_nome_arquivo(nome)).filename();
    if (nome_seguro.empty()) throw std::runtime_error("Nome de arquivo invalido.");
    fs::path caminho_final = destino / nome_seguro;
    fs::path caminho_temp = caminho_final.string() + ".temp";

    // Se já existe e tamanho é válido, pula (e credita no progresso global)
    std::error_code ec;
    unsigned long long tam_esperado = drive_tamanho_do_item(arquivo);
    if (fs::exists(caminho_final, ec)) {
        auto sz = fs::file_size(caminho_final, ec);
        if (!ec && sz > 0 && (min_bytes <= 0 || sz >= static_cast<unsigned long long>(min_bytes))) {
            if (max_bytes > 0 && sz > static_cast<unsigned long long>(max_bytes)) {
                if (pulado_por_tamanho) *pulado_por_tamanho = true;
                if (progresso) progresso->prontos_arquivos.fetch_add(1);
                log("[DRIVE SKIP] " + nome + " (" + drive_formatar_bytes((unsigned long long)sz) +
                    ") excede tamanho_max (" + drive_formatar_bytes((unsigned long long)max_bytes) + ")");
                return;
            }
            if (progresso) {
                progresso->baixado_bytes.fetch_add((unsigned long long)sz);
                progresso->prontos_arquivos.fetch_add(1);
            }
            // "ja existe" = 1 linha curta, sem barra de progresso.
            log("[DRIVE JA EXISTE] " + nome + " (" +
                drive_formatar_bytes((unsigned long long)sz) + ")");
            return;
        }
    }

    if (tam_esperado > 0)
        log("[DRIVE BAIXANDO] " + nome + " (" + drive_formatar_bytes(tam_esperado) + ") -> " + caminho_final.string());
    else
        log("[DRIVE BAIXANDO] " + nome + " (tamanho desconhecido) -> " + caminho_final.string());
    // Skip antecipado via metadados (Drive informa size sem baixar nada)
    if (max_bytes > 0 && tam_esperado > static_cast<unsigned long long>(max_bytes)) {
        if (pulado_por_tamanho) *pulado_por_tamanho = true;
        if (progresso) progresso->prontos_arquivos.fetch_add(1);
        log("[DRIVE SKIP] " + nome + " (" + drive_formatar_bytes(tam_esperado) +
            ") excede tamanho_max (" + drive_formatar_bytes((unsigned long long)max_bytes) + ")");
        return;
    }
    std::string url = "https://www.googleapis.com/drive/v3/files/" + drive_url_encode(id) + "?alt=media";
    FILE* file = _wfopen(caminho_temp.wstring().c_str(), L"wb");
    if (!file) {
        throw std::runtime_error("Nao foi possivel criar arquivo temporario: " + caminho_temp.string());
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        throw std::runtime_error("Nao foi possivel inicializar CURL.");
    }
    drive_aplicar_ca_bundle(curl);
    struct curl_slist* headers = nullptr;
    std::string authorization = "Authorization: Bearer " + access_token;
    headers = curl_slist_append(headers, authorization.c_str());
    ProgressoArquivoDrive ctx_prog;
    ctx_prog.file = file;
    ctx_prog.global = progresso;
    ctx_prog.nome = nome;
    ctx_prog.total = tam_esperado;
    ctx_prog.tamanho_max = max_bytes > 0 ? static_cast<unsigned long long>(max_bytes) : 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, drive_write_file_progress_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx_prog);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    fclose(file);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    // Em erro, fecha a barra pendente (se houver) para o log nao colar na
    // linha do progresso.
    if (ctx_prog.estourou_max && max_bytes > 0) {
        if (ctx_prog.imprimiu) {
            fputc('\n', stdout);
            fflush(stdout);
        }
        std::error_code remove_ec;
        fs::remove(caminho_temp, remove_ec);
        if (pulado_por_tamanho) *pulado_por_tamanho = true;
        if (progresso) progresso->prontos_arquivos.fetch_add(1);
        log("[DRIVE SKIP] " + nome + " excede tamanho_max (" +
            drive_formatar_bytes((unsigned long long)max_bytes) + ") - interrompido no stream");
        return;
    }
    if (result != CURLE_OK) {
        if (ctx_prog.imprimiu) {
            fputc('\n', stdout);
            fflush(stdout);
        }
        std::error_code remove_ec;
        fs::remove(caminho_temp, remove_ec);
        throw std::runtime_error(std::string("Erro CURL ao baixar: ") + curl_easy_strerror(result));
    }
    if (http_code < 200 || http_code >= 300) {
        if (ctx_prog.imprimiu) {
            fputc('\n', stdout);
            fflush(stdout);
        }
        std::string corpo;
        {
            std::ifstream erro_file(caminho_temp, std::ios::binary);
            if (erro_file) {
                char buffer[512];
                erro_file.read(buffer, sizeof(buffer));
                corpo.assign(buffer, static_cast<size_t>(erro_file.gcount()));
            }
        }
        std::error_code remove_ec;
        fs::remove(caminho_temp, remove_ec);
        std::string mensagem = "Google Drive retornou HTTP " + std::to_string(http_code);
        if (!corpo.empty()) mensagem += ": " + corpo;
        throw ErroHttpDrive(http_code, mensagem);
    }

    fs::remove(caminho_final, ec);
    fs::rename(caminho_temp, caminho_final, ec);
    if (ec) {
        throw std::runtime_error("Nao foi possivel renomear arquivo temporario: " + ec.message());
    }
    if (progresso) {
        // Finaliza a barra do arquivo com '\n' e credita no progresso global.
        drive_log_arquivo(progresso, ctx_prog, true);
        progresso->prontos_arquivos.fetch_add(1);
    } else {
        log("  [DRIVE OK] Concluido: " + nome);
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

static void drive_renovar_token_se_preciso(TokenAcessoDrive& token, const json& service_account) {
    std::lock_guard<std::mutex> lk(g_drive_token_mtx);
    if (!token_vencendo(token)) return;
    log("  [DRIVE] Token de acesso renovando...");
    try {
        token = drive_obter_access_token(service_account);
    } catch (const std::exception& e) {
        log(std::string("  [ERRO TOKEN DRIVE] ") + e.what());
    }
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
        log("  [ERRO DRIVE] Profundidade maxima excedida (possivel ciclo).");
        return;
    }
    if (!item.contains("id") || !item.contains("name")) {
        erros.fetch_add(1);
        log("  [ERRO ARQUIVO DRIVE] Item sem id/name retornado pelo Google.");
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
            log("  [ERRO PASTA DRIVE] " + nome + ": " + ec.message());
            return;
        }
        log("  [DRIVE PASTA] " + sub.string());
        drive_renovar_token_se_preciso(token, service_account);
        std::vector<json> filhos;
        try {
            filhos = drive_listar_arquivos(id, token.valor);
        } catch (const std::exception& e) {
            erros.fetch_add(1);
            log(std::string("  [ERRO LISTAR DRIVE] ") + nome + ": " + e.what());
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
        log("  [DRIVE] " + std::to_string(filhos.size()) + " itens em '" + nome +
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
        log("  [DRIVE] download_workers=" + std::to_string(n_dl) +
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
            std::string resumo = "  [DRIVE PASTA OK] " + nome + ": " +
                std::to_string((progresso ? progresso->prontos_arquivos.load() : sucesso.load()) - prontos_antes) +
                " arquivos, " +
                drive_formatar_bytes((progresso ? progresso->baixado_bytes.load() : 0ULL) - bytes_antes) +
                " em " + drive_formatar_tempo(dec);
            log(resumo);
        }
        return;
    }

    for (int tentativa = 0; tentativa < 2; ++tentativa) {
        drive_renovar_token_se_preciso(token, service_account);
        try {
            bool pulado_max = false;
            drive_baixar_arquivo(item, destino_local, token.valor, tamanho_min_bytes, tamanho_max_bytes,
                                 &pulado_max, progresso);
            if (pulado_max) {
                if (pulados) pulados->fetch_add(1);
            } else {
                sucesso.fetch_add(1);
            }
            return;
        } catch (const ErroHttpDrive& e) {
            if (e.codigo == 401 && tentativa == 0) {
                log("  [DRIVE HTTP 401] Renovando token de acesso...");
                try {
                    std::lock_guard<std::mutex> lk(g_drive_token_mtx);
                    token = drive_obter_access_token(service_account);
                } catch (...) {}
                continue;
            }
            erros.fetch_add(1);
            log(std::string("  [ERRO ARQUIVO DRIVE] ") + e.what());
            return;
        } catch (const std::exception& e) {
            erros.fetch_add(1);
            log(std::string("  [ERRO ARQUIVO DRIVE] ") + e.what());
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
        arquivos = drive_listar_arquivos(folder_id, token.valor);
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
    log("  [DRIVE] Baixando pasta por pasta (conclui uma antes de ir p/ proxima)...");

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
                    log("[DRIVE SKIP] " + a.value("name", "?") + " (" + drive_formatar_bytes(sz) +
                        ") excede tamanho_max (" +
                        drive_formatar_bytes((unsigned long long)ds.tamanho_max_bytes) + ")");
                    continue;
                }
            }
            arquivos_raiz.push_back(a);
            progresso.total_arquivos.fetch_add(1);
            progresso.total_bytes.fetch_add(drive_tamanho_do_item(a));
        }
        log("  [DRIVE] Raiz: " + std::to_string(arquivos.size()) + " itens (" +
            std::to_string(arquivos_raiz.size()) + " arquivos, " +
            std::to_string(subpastas_raiz.size()) + " subpastas" +
            (pulados ? ", " + std::to_string(pulados) + " pulados (tamanho)" : "") + ")");
        int n_dl = CFG.download_workers < 1 ? 1 : CFG.download_workers;
        if ((size_t)n_dl > arquivos_raiz.size() && !arquivos_raiz.empty())
            n_dl = (int)arquivos_raiz.size();
        std::atomic<size_t> suc_at{0}, err_at{0}, pul_at{0};
        log("  [DRIVE] download_workers=" + std::to_string(n_dl) +
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
            std::to_string(erros) + " erros em " + drive_formatar_tempo(dec) +
            " | " + drive_formatar_bytes(progresso.baixado_bytes.load()) +
            " a " + drive_formatar_bytes((unsigned long long)vel) + "/s");
        log("============================================================");
    }
    return erros == 0;
}
