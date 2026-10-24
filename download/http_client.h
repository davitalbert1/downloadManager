#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

struct DatasetCfg;

using json = nlohmann::json;

// Autenticação declarada pelo dataset: onde a chave vai (query, header,
// basic ou auto), nome do header/parâmetro e prefixo (ex: "Bearer").
struct AuthApi {
    std::string modo = "auto"; // auto | query | header | basic | none
    std::string header = "Authorization";
    std::string prefixo = "Bearer";
    std::string param = "apikey";
};

AuthApi auth_do_dataset(const DatasetCfg& ds);

// URLs assinadas (S3/GCS/Azure) já carregam a credencial; header extra
// costuma invalidar a assinatura.
bool url_presignada(const std::string& url);
std::string url_com_chave(const std::string& url, const AuthApi& auth,
                          const std::string& chave);

struct OpcoesHttp {
    std::string metodo = "GET";
    std::map<std::string, std::string> headers;
    std::string corpo;
    const AuthApi* auth = nullptr;
    std::string chave_api;
    double timeout_sec = 0.0; // 0 = CFG.http_timeout
    double connect_timeout_sec = 30.0;
    bool verificar_ssl = true;
    bool aceitar_gzip = true;
    bool user_agent_navegador = false;
    std::string range;
};

struct RespostaHttp {
    bool erro_rede = false;
    std::string erro_mensagem;
    long status = 0;
    std::string corpo;
    double retry_after = -1.0;
    std::string location;
    long long content_length = -1;
    std::string content_type;
};

// Callbacks opcionais: escrita recebe o bloco e seu tamanho; retornar menos
// que o tamanho aborta. Progresso retorna != 0 para abortar.
struct CallbacksHttp {
    std::function<size_t(char*, size_t)> escrita;
    std::function<int(long long, long long)> progresso;
};

RespostaHttp http_executar(const std::string& url, const OpcoesHttp& op = {},
                           const CallbacksHttp& cb = {});
RespostaHttp http_requisicao(const std::string& url, const OpcoesHttp& op = {});

struct OpcoesDownload {
    std::string url;
    std::string caminho;
    std::string tag;
    std::map<std::string, std::string> headers;
    const AuthApi* auth = nullptr;
    std::string chave_api;
    double timeout_sec = 1800.0;
    long long tamanho_min = 0;
    long long tamanho_max = 0;
    bool verificar_ssl = true;
    bool user_agent_navegador = false;
    bool detectar_pagina_erro = false;
    bool permitir_reuso = false;
    bool progresso = true;
    bool logar_skip = true;
    bool* pulado_por_tamanho = nullptr;
    std::function<void(long long, long long)> ao_progresso;
};

struct ResultadoDownload {
    bool ok = false;
    bool ja_existia = false;
    bool pulado_tamanho = false;
    bool pagina_erro = false;
    long status = 0;
    std::string erro;
    std::string corpo_erro;
    long long bytes = 0;
    double segundos = 0.0;
};

ResultadoDownload http_baixar_arquivo(const OpcoesDownload& op);

std::string escapar_url(const std::string& s);
std::string montar_url(const std::string& base,
                       const std::map<std::string, std::string>& params);

std::optional<json> baixar_com_retry(const std::string& url,
                                     const std::map<std::string, std::string>& params,
                                     int max_tentativas, double custo,
                                     const std::string& chave_api,
                                     const std::string& metodo = "GET",
                                     const std::map<std::string, std::string>& headers = {},
                                     const std::string& corpo = "",
                                     const AuthApi& auth = {});
