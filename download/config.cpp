#include "config.h"
#include "utils.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <regex>
#include <sstream>

Config CFG;
static std::map<std::string, std::string> g_env_map;

bool chave_ctx_interna(const std::string& k) {
    return k == "start" || k == "end" || k == "start_date" || k == "end_date" ||
           k == "hoje" || k == "timezone" || k == "vars" || k == "lat" || k == "lon" ||
           k == "latitude" || k == "longitude" || k == "api_key" || k == "data" ||
           k == "metodo" || k == "id" || k == "tabela";
}

std::string aplicar_ctx(std::string s, const Contexto& ctx) {
    for (const auto& kv : ctx) {
        std::string token = "{" + kv.first + "}";
        size_t p = 0;
        while ((p = s.find(token, p)) != std::string::npos) {
            s.replace(p, token.size(), kv.second);
            p += kv.second.size();
        }
    }
    return s;
}

void carregar_env(const std::string& caminho) {
    std::ifstream arq(caminho);
    if (!arq) return;
    std::string linha;
    while (std::getline(arq, linha)) {
        linha = cortar(sem_comentario(linha));
        if (linha.empty()) continue;
        size_t pos = linha.find('=');
        if (pos == std::string::npos) continue;
        std::string k = cortar(linha.substr(0, pos));
        std::string v = cortar(linha.substr(pos + 1));
        if (!v.empty() && (v.front() == '"' || v.front() == '\'') && v.front() == v.back()) {
            v = v.substr(1, v.size() - 2);
        }
        if (!k.empty()) {
            g_env_map[k] = v;
            #ifdef _WIN32
                _putenv_s(k.c_str(), v.c_str());
            #else
                setenv(k.c_str(), v.c_str(), 0);
            #endif
        }
    }
}

std::string obter_env_ou_config(const char* nome_env, const std::string& valor_cfg, const std::string& padrao) {
    if (!valor_cfg.empty()) return valor_cfg;
    const char* env_v = std::getenv(nome_env);
    if (env_v && *env_v) return std::string(env_v);
    auto it = g_env_map.find(nome_env);
    if (it != g_env_map.end() && !it->second.empty()) return it->second;
    return padrao;
}

static int env_int(const char* nome, int padrao) {
    const char* v = std::getenv(nome);
    if (!v || !*v) return padrao;
    try {
        return std::stoi(v);
    } catch (...) {
        return padrao;
    }
}

static double env_double(const char* nome, double padrao) {
    const char* v = std::getenv(nome);
    if (!v || !*v) return padrao;
    try {
        return std::stod(v);
    } catch (...) {
        return padrao;
    }
}

bool Ini::obter(const std::string& sec, const std::string& chave, std::string& saida) const {
    auto it = valores.find(sec);
    if (it == valores.end()) return false;
    auto it2 = it->second.find(chave);
    if (it2 == it->second.end()) return false;
    saida = it2->second;
    return true;
}

std::string Ini::obter_ou(const std::string& sec, const std::string& chave, const std::string& padrao) const {
    std::string v;
    return obter(sec, chave, v) ? v : padrao;
}

std::vector<std::string> Ini::lista(const std::string& sec) const {
    auto it = listas.find(sec);
    if (it == listas.end()) return {};
    return it->second;
}

bool Ini::tem_secao(const std::string& sec) const {
    return valores.count(sec) || listas.count(sec);
}

static bool eh_secao_dataset(const std::string& s) {
    return s.rfind("dataset:", 0) == 0;
}

static bool eh_secao_dim(const std::string& s) {
    return s.rfind("dim:", 0) == 0 || s.rfind("eixo:", 0) == 0;
}

static bool eh_secao_coordenadas(const std::string& s) {
    return s == "coordenadas" || s == "coordinates";
}

// Primeiro valor nao vazio entre os aliases PT/EN de uma chave.
static std::string primeiro_valor(const Ini& ini, const std::string& secao,
                                  std::initializer_list<const char*> chaves) {
    for (const char* chave : chaves) {
        std::string v = cortar(ini.obter_ou(secao, chave));
        if (!v.empty()) return v;
    }
    return "";
}

Ini ler_ini(const std::string& caminho) {
    Ini ini;
    std::ifstream arquivo(caminho);
    if (!arquivo) {
        std::cerr << "[WARNING] Arquivo de configuracao nao encontrado: " << caminho
                  << " (usando valores padrao)\n";
        return ini;
    }
    auto registrar_secao = [&](const std::string& secao) {
        if (!secao.empty() &&
            std::find(ini.ordem_secoes.begin(), ini.ordem_secoes.end(), secao) == ini.ordem_secoes.end())
            ini.ordem_secoes.push_back(secao);
    };
    std::string linha, secao;
    while (std::getline(arquivo, linha)) {
        linha = cortar(sem_comentario(linha));
        if (linha.empty()) continue;
        if (linha.front() == '[' && linha.back() == ']') {
            secao = cortar(linha.substr(1, linha.size() - 2));
            registrar_secao(secao);
            continue;
        }
        if (secao.empty()) continue;
        registrar_secao(secao);
        if (eh_secao_dataset(secao)) {
            size_t igual = linha.find('=');
            if (igual == std::string::npos) {
                for (auto& token : dividir(linha, ',')) {
                    if (token.empty() || token.find('=') != std::string::npos) continue;
                    ini.listas[secao].push_back(token);
                }
                continue;
            }
            std::string chave = cortar(linha.substr(0, igual));
            std::string valor = cortar(linha.substr(igual + 1));
            if (chave.empty()) continue;
            if (chave == "pasta_saida" || chave == "pasta_imagens") valor = expandir_caminho(valor);
            if (chave == "vars" || chave == "colunas" || chave == "params_excluir" ||
                chave == "lista_vars" || chave == "colunas_extra") {
                for (auto& token : dividir(valor, ',')) {
                    if (token.empty()) continue;
                    size_t dp = token.find(':');
                    std::string nome = cortar(dp == std::string::npos ? token : token.substr(0, dp));
                    if (!nome.empty()) ini.listas[secao].push_back(token);
                }
                ini.valores[secao][chave] = valor;
                continue;
            }
            ini.valores[secao][chave] = valor;
            continue;
        }
        if (eh_secao_coordenadas(secao) || eh_secao_dim(secao)) {
            size_t igual = linha.find('=');
            if (igual != std::string::npos) {
                std::string chave = cortar(linha.substr(0, igual));
                std::string valor = cortar(linha.substr(igual + 1));
                if (chave == "colunas" || chave == "campos" || chave == "keys") {
                    ini.valores[secao][chave] = valor;
                    continue;
                }
            }
            if (!linha.empty()) ini.listas[secao].push_back(linha);
            continue;
        }
        size_t igual = linha.find('=');
        if (igual == std::string::npos) continue;
        std::string chave = cortar(linha.substr(0, igual));
        std::string valor = cortar(linha.substr(igual + 1));
        if (!chave.empty()) ini.valores[secao][chave] = valor;
    }
    return ini;
}

static const DimensaoCfg* achar_dim(const std::string& nome) {
    for (const auto& d : CFG.dimensoes) {
        if (d.nome == nome) return &d;
    }
    return nullptr;
}

static std::vector<Contexto> contextos_coordenadas(bool com_fallback) {
    std::vector<Contexto> out;
    if (CFG.coordenadas.empty() && com_fallback)
        out.push_back({{"latitude", num_str(0.0)}, {"longitude", num_str(0.0)},
                       {"lat", num_str(0.0)}, {"lon", num_str(0.0)}});
    for (const auto& c : CFG.coordenadas) {
        Contexto ctx;
        ctx["latitude"] = num_str(c.first);
        ctx["longitude"] = num_str(c.second);
        ctx["lat"] = ctx["latitude"];
        ctx["lon"] = ctx["longitude"];
        out.push_back(std::move(ctx));
    }
    return out;
}

static std::vector<Contexto> contextos_do_eixo(const std::string& eixo) {
    std::vector<Contexto> out;
    if (eixo == "coordenadas" || eixo == "coordinates") return contextos_coordenadas(false);
    const DimensaoCfg* dim = achar_dim(eixo);
    if (!dim) {
        log("[WARNING] Eixo '" + eixo + "' nao encontrado ([dim:" + eixo + "]).");
        return out;
    }
    for (const auto& linha : dim->linhas) {
        Contexto ctx;
        ctx[dim->nome] = juntar(linha, ",");
        if (!dim->campos.empty()) {
            for (size_t i = 0; i < dim->campos.size() && i < linha.size(); ++i)
                ctx[dim->campos[i]] = linha[i];
        } else if (linha.size() == 1) {
            ctx[dim->nome] = linha[0];
        } else {
            for (size_t i = 0; i < linha.size(); ++i) {
                ctx[dim->nome + "_" + std::to_string(i)] = linha[i];
            }
        }
        out.push_back(std::move(ctx));
    }
    return out;
}

static std::vector<Contexto> expandir_contextos(const DatasetCfg& ds) {
    if (ds.eixos.empty()) return {Contexto{}};
    std::vector<Contexto> acc{{}};
    for (const auto& eixo : ds.eixos) {
        auto pecas = contextos_do_eixo(eixo);
        if (pecas.empty()) return {};
        std::vector<Contexto> nxt;
        nxt.reserve(acc.size() * pecas.size());
        for (const auto& a : acc) {
            for (const auto& b : pecas) {
                Contexto m = a;
                m.insert(b.begin(), b.end());
                nxt.push_back(std::move(m));
            }
        }
        acc.swap(nxt);
    }
    return acc;
}

std::vector<Contexto> contextos_de_download(const DatasetCfg& ds) {
    if (!ds.usar_coordenadas) return expandir_contextos(ds);
    return contextos_coordenadas(true);
}

std::string chave_contexto(const Contexto& ctx) {
    auto ilat = ctx.find("latitude");
    auto ilon = ctx.find("longitude");
    if (ilat != ctx.end() && ilon != ctx.end()) return ilat->second + "," + ilon->second;
    std::string ch;
    for (const auto& kv : ctx) {
        if (chave_ctx_interna(kv.first)) continue;
        if (!ch.empty()) ch += "|";
        ch += kv.first + "=" + kv.second;
    }
    return ch;
}

bool dataset_generico(const DatasetCfg& ds) {
    return ds.tipo_saida != "arquivo" && ds.tipo_saida != "drive" &&
           ds.tipo_saida != "google_drive" && ds.tipo_saida != "wikipedia" &&
           ds.tipo_saida != "wiki" && ds.tipo_saida != "video" &&
           ds.tipo_saida != "stream" && ds.drive_folder_id.empty();
}

std::string resolver_data_limite(const std::string& v, const std::string& padrao) {
    std::string s = cortar(v);
    if (s.empty()) return padrao;
    std::string low = s;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (low == "hoje" || low == "today" || low == "now") return hoje();
    return s;
}

bool data_no_intervalo(const std::string& d, const DatasetCfg& ds) {
    std::string mn = resolver_data_limite(ds.data_min, "");
    std::string mx = resolver_data_limite(ds.data_max, "");
    if (!mn.empty() && d < mn) return false;
    if (!mx.empty() && d > mx) return false;
    return true;
}

static std::vector<DatasetCfg> montar_datasets_genericos(const Ini& ini) {
    std::vector<DatasetCfg> out;
    for (const auto& secao : ini.ordem_secoes) {
        if (!eh_secao_dataset(secao)) continue;
        DatasetCfg d;
        d.id = secao.substr(8);
        d.db = ini.obter_ou(secao, "db");
        std::string tab = ini.obter_ou(secao, "tabela", d.id);
        d.tabela = normalizar_nome(tab.empty() ? d.id : tab);
        d.url = cortar(ini.obter_ou(secao, "url"));
        d.url_passado = cortar(ini.obter_ou(secao, "url_passado"));
        d.url_futuro = cortar(ini.obter_ou(secao, "url_futuro"));
        d.api_keys = cortar(ini.obter_ou(secao, "api_keys"));

        // Campos para dataset do tipo arquivo / assíncronos
        d.tipo_saida = primeiro_valor(ini, secao, {"tipo_saida", "output_type"});
        if (d.tipo_saida.empty()) {
            std::string temp_val;
            if (ini.obter(secao, "pasta_saida", temp_val) || ini.obter(secao, "dataset_api", temp_val) ||
                ini.obter(secao, "extensao", temp_val) || ini.obter(secao, "padrao_arquivo", temp_val)) {
                d.tipo_saida = "arquivo";
            } else {
                d.tipo_saida = "db";
            }
        }
        d.pasta_saida = cortar(ini.obter_ou(secao, "pasta_saida", "dados_arquivos"));
        d.extensao = cortar(ini.obter_ou(secao, "extensao", "bin"));
        if (d.extensao.empty()) d.extensao = cortar(ini.obter_ou(secao, "format", "bin"));
        d.dataset_api = cortar(ini.obter_ou(secao, "dataset_api"));
        d.product_type = cortar(ini.obter_ou(secao, "product_type"));
        d.data_format = cortar(ini.obter_ou(secao, "data_format", d.extensao));
        d.download_format = cortar(ini.obter_ou(secao, "download_format"));
        d.padrao_arquivo = cortar(ini.obter_ou(secao, "padrao_arquivo"));
        d.job_executar = cortar(ini.obter_ou(secao, "job_executar"));
        d.job_status = cortar(ini.obter_ou(secao, "job_status"));
        d.job_resultado = cortar(ini.obter_ou(secao, "job_resultado"));
        d.job_listar_ativos = cortar(ini.obter_ou(secao, "job_listar_ativos"));
        d.job_listar_ok = cortar(ini.obter_ou(secao, "job_listar_ok"));
        d.job_lista_campo = cortar(ini.obter_ou(secao, "job_lista_campo", "jobs"));
        d.job_campo_dataset = cortar(ini.obter_ou(secao, "job_campo_dataset"));
        d.job_campo_id = cortar(ini.obter_ou(secao, "job_campo_id"));
        d.job_campo_status = cortar(ini.obter_ou(secao, "job_campo_status", "status"));
        d.job_campo_asset = cortar(ini.obter_ou(secao, "job_campo_asset"));
        auto ler_lista_cfg = [&](const char* chave, std::vector<std::string>& destino) {
            for (auto& item : dividir(ini.obter_ou(secao, chave), ',')) {
                std::string v = minusculas(cortar(item));
                if (!v.empty()) destino.push_back(v);
            }
        };
        ler_lista_cfg("job_status_ok", d.job_status_ok);
        ler_lista_cfg("job_status_erro", d.job_status_erro);
        ler_lista_cfg("job_reenviar_se", d.job_reenviar_se);
        {
            std::string v = cortar(ini.obter_ou(secao, "job_poll_intervalo"));
            if (!v.empty()) {
                try {
                    d.job_poll_intervalo = std::stod(v);
                } catch (...) {}
            }
        }
        d.job_sequencial = para_bool(ini.obter_ou(secao, "job_sequencial", "0"), false);
        d.job_cancelar_duplicados =
            para_bool(ini.obter_ou(secao, "job_cancelar_duplicados", "1"), true);
        d.ano_inicial = para_int(ini.obter_ou(secao, "ano_inicial", "1940"), 1940);
        d.dias_atraso = para_int(ini.obter_ou(secao, "dias_atraso", "0"), 0);
        d.auto_importar_banco = para_bool(ini.obter_ou(secao, "auto_importar_banco", "0"), false);
        d.evitar_download_se_no_banco = para_bool(ini.obter_ou(secao, "evitar_download_se_no_banco", "0"), false);

        d.baixar_arquivos_vinculados = para_bool(ini.obter_ou(secao, "baixar_arquivos_vinculados", "0"), false);
        d.campo_url_arquivo = cortar(ini.obter_ou(secao, "campo_url_arquivo"));
        d.campo_nome_arquivo = cortar(ini.obter_ou(secao, "campo_nome_arquivo"));
        d.padrao_extracao = cortar(ini.obter_ou(secao, "padrao_extracao"));
        d.usar_yt_dlp = para_bool(ini.obter_ou(secao, "usar_yt_dlp", "0"), false);
        d.tamanho_min_bytes = para_int(ini.obter_ou(secao, "tamanho_min_bytes", "0"), 0);
        { // tamanho maximo: aceita bytes ou sufixos (KB/MB/GB/TB); aliases PT/EN
            std::string tmax = primeiro_valor(ini, secao,
                {"tamanho_max_bytes", "tamanho_maximo", "tamanho_max", "max_bytes",
                 "max_file_size", "max_size", "tamanho_limite"});
            if (!tmax.empty()) d.tamanho_max_bytes = parse_tamanho_bytes(tmax, 0);
            // vazio = herda do [geral] em carregar_config()
        }

        d.drive_folder_id = cortar(ini.obter_ou(secao, "drive_folder_id"));
        if (d.drive_folder_id.empty()) d.drive_folder_id = cortar(ini.obter_ou(secao, "folder_id"));
        d.drive_service_account = cortar(ini.obter_ou(secao, "drive_service_account"));
        if (d.drive_service_account.empty()) d.drive_service_account = cortar(ini.obter_ou(secao, "service_account"));

        d.seguir_url_detalhe = para_bool(ini.obter_ou(secao, "seguir_url_detalhe", "0"), false);
        if (!d.seguir_url_detalhe) d.seguir_url_detalhe = para_bool(ini.obter_ou(secao, "follow_detail_url", "0"), false);
        d.campo_url_detalhe = cortar(ini.obter_ou(secao, "campo_url_detalhe", "url"));
        if (d.campo_url_detalhe.empty()) d.campo_url_detalhe = "url";
        d.baixar_todas_imagens = para_bool(ini.obter_ou(secao, "baixar_todas_imagens", "0"), false);
        if (!d.baixar_todas_imagens) d.baixar_todas_imagens = para_bool(ini.obter_ou(secao, "download_all_images", "0"), false);
        d.pasta_imagens = cortar(ini.obter_ou(secao, "pasta_imagens", "imagens"));
        if (d.pasta_imagens.empty()) d.pasta_imagens = "imagens";

        std::string urls_str = ini.obter_ou(secao, "urls");
        if (!urls_str.empty()) {
            for (auto& u : dividir(urls_str, ',')) {
                if (!u.empty()) d.lista_urls.push_back(cortar(u));
            }
        }

        // Expansão de episódios/faixas de URLs para vídeos ou datasets sequenciais
        int ep_ini = para_int(ini.obter_ou(secao, "ep_inicio", ini.obter_ou(secao, "episodio_inicio", "0")), 0);
        int ep_fim = para_int(ini.obter_ou(secao, "ep_fim", ini.obter_ou(secao, "episodio_fim", "0")), 0);

        if (ep_ini == 0 && ep_fim == 0 && !d.url.empty()) {
            static const std::regex reg_range(R"([?&]ep=[{\[]?(\d+)\.\.(\d+)[}\]]?)", std::regex::icase);
            std::smatch m;
            if (std::regex_search(d.url, m, reg_range)) {
                ep_ini = std::stoi(m[1].str());
                ep_fim = std::stoi(m[2].str());
            }
        }

        if (ep_ini > 0 && ep_fim >= ep_ini && !d.url.empty()) {
            std::string url_base = d.url;
            bool tem_token_ep = (url_base.find("{ep}") != std::string::npos ||
                                 url_base.find("{episodio}") != std::string::npos);

            std::string prefixo_url;
            char sep_ep = '?';

            if (!tem_token_ep) {
                // Remove qualquer ep=... existente da base para evitar conflitos e duplicações
                size_t pos_ep = url_base.find("?ep=");
                if (pos_ep != std::string::npos) {
                    prefixo_url = url_base.substr(0, pos_ep);
                    sep_ep = '?';
                } else {
                    pos_ep = url_base.find("&ep=");
                    if (pos_ep != std::string::npos) {
                        prefixo_url = url_base.substr(0, pos_ep);
                        sep_ep = '&';
                    } else {
                        prefixo_url = url_base;
                        sep_ep = (prefixo_url.find('?') == std::string::npos) ? '?' : '&';
                    }
                }
            }

            for (int ep = ep_ini; ep <= ep_fim; ++ep) {
                std::string u;
                if (tem_token_ep) {
                    u = aplicar_ctx(url_base, {{"ep", std::to_string(ep)}, {"episodio", std::to_string(ep)}});
                } else {
                    u = prefixo_url + sep_ep + "ep=" + std::to_string(ep);
                }
                d.lista_urls.push_back(u);
            }
            d.url.clear();
        }

        std::string hrs_str = ini.obter_ou(secao, "horarios");
        if (!hrs_str.empty()) d.horarios = dividir(hrs_str, ',');

        d.projeto = cortar(ini.obter_ou(secao, "projeto"));
        d.data_dump = cortar(ini.obter_ou(secao, "data_dump"));
        d.apenas_ns0 = para_bool(ini.obter_ou(secao, "apenas_ns0", "1"), true);
        d.ignorar_redirect = para_bool(ini.obter_ou(secao, "ignorar_redirect", "1"), true);
        d.tamanho_minimo = para_int(ini.obter_ou(secao, "tamanho_minimo", "20"), 20);
        d.wiki_batch = para_int(ini.obter_ou(secao, "wiki_batch", "2000"), 2000);
        d.listar_remoto = para_bool(ini.obter_ou(secao, "listar_remoto", "1"), true);

        d.bloco = primeiro_valor(ini, secao, {"bloco", "block"});
        d.params_chave = cortar(ini.obter_ou(secao, "params_chave", d.bloco));

        auto add_coluna = [&](const std::string& c, const std::string& v, const std::string& tp) {
            for (auto& cc : d.colunas) {
                if (cc.coluna == c) {
                    cc.variavel_api = v;
                    cc.tipo = tp;
                    return;
                }
            }
            d.colunas.push_back(ColunaCfg{c, v, tp});
        };
        auto add_var = [&](const std::string& v) {
            if (v.empty() || v == "time" || v == "timezone") return;
            if (std::find(d.vars.begin(), d.vars.end(), v) == d.vars.end()) d.vars.push_back(v);
        };
        auto it_sec = ini.valores.find(secao);
        if (it_sec != ini.valores.end()) {
            std::map<std::string, std::string> defs;
            for (const auto& kv : it_sec->second) {
                std::string resto;
                if (kv.first.rfind("coluna.", 0) == 0) resto = kv.first.substr(7);
                else if (kv.first.rfind("column.", 0) == 0) resto = kv.first.substr(7);
                else continue;
                resto = cortar(resto);
                if (!resto.empty()) defs[resto] = kv.second;
            }
            for (const auto& kv : defs) {
                std::string c_ign, v_api, tp;
                std::string def_fmt = kv.first + "=" + kv.second;
                if (separar_def_coluna(def_fmt, c_ign, v_api, tp)) {
                    std::string col = normalizar_nome(kv.first);
                    add_coluna(col, v_api, tp);
                    add_var(v_api);
                }
            }
        }
        for (const auto& item : ini.lista(secao)) {
            std::string c, v, tp;
            if (separar_def_coluna(item, c, v, tp)) {
                add_coluna(c, v, tp);
                add_var(v);
            } else {
                add_var(item);
            }
        }
        std::string excl = ini.obter_ou(secao, "vars_excluir");
        if (excl.empty()) excl = ini.obter_ou(secao, "params_excluir");
        if (!excl.empty()) {
            for (auto& v : dividir(excl, ',')) {
                if (!v.empty()) d.vars_excluir.insert(v);
            }
        }
        d.data_min = primeiro_valor(ini, secao, {"data_min", "start_date"});
        d.data_max = primeiro_valor(ini, secao, {"data_max", "end_date"});

        d.chave_data = normalizar_nome(ini.obter_ou(secao, "chave_data", "data"));
        d.chave_hora = normalizar_nome(ini.obter_ou(secao, "chave_hora", "hora"));
        if (d.chave_data.empty()) d.chave_data = "data";
        if (d.chave_hora.empty()) d.chave_hora = "hora";

        std::string gran = minusculas(primeiro_valor(ini, secao, {"granularidade", "granularity"}));
        if (gran == "hora" || gran == "hourly" || gran == "horario" || gran == "hour")
            d.granularidade = "hora";
        else if (gran == "dia" || gran == "daily" || gran == "diario" || gran == "day")
            d.granularidade = "dia";
        else if (gran == "nenhuma" || gran == "none" || gran == "raw")
            d.granularidade = "nenhuma";
        else
            d.granularidade = (d.bloco == "hourly" ? "hora" : "dia");

        d.salvar_hora = para_bool(ini.obter_ou(secao, "salvar_hora"), d.granularidade == "hora");
        d.salvar_timezone = para_bool(ini.obter_ou(secao, "salvar_timezone"), false);

        d.metodo = cortar(ini.obter_ou(secao, "metodo", "GET"));
        std::transform(d.metodo.begin(), d.metodo.end(), d.metodo.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        d.corpo = primeiro_valor(ini, secao, {"corpo", "body"});
        d.formato = minusculas(primeiro_valor(ini, secao, {"formato", "format"}));
        if (d.formato.empty()) d.formato = "auto";
        d.chave_tempo = cortar(ini.obter_ou(secao, "chave_tempo", "time"));
        if (d.chave_tempo.empty()) d.chave_tempo = "time";
        d.chave_unica = primeiro_valor(ini, secao, {"chave_unica", "unique_key"});
        d.api_key_em = minusculas(cortar(ini.obter_ou(secao, "api_key_em", "query")));
        d.api_key_header = cortar(ini.obter_ou(secao, "api_key_header", "Authorization"));
        d.api_key_prefix = cortar(ini.obter_ou(secao, "api_key_prefix", "Bearer"));
        d.api_key_param = cortar(ini.obter_ou(secao, "api_key_param"));
        d.api_key_env = cortar(ini.obter_ou(secao, "api_key_env"));

        if (it_sec != ini.valores.end()) {
            for (const auto& kv : it_sec->second) {
                if (kv.first.rfind("param.", 0) == 0 && kv.first.size() > 6)
                    d.params[kv.first.substr(6)] = kv.second;
                else if (kv.first.rfind("header.", 0) == 0 && kv.first.size() > 7)
                    d.headers[kv.first.substr(7)] = kv.second;
            }
        }
        std::string eixos_s = primeiro_valor(ini, secao, {"eixos", "axes"});
        if (!eixos_s.empty()) {
            for (auto& e : dividir(eixos_s, ',')) {
                if (!e.empty()) d.eixos.push_back(e);
            }
        }
        std::string uc = primeiro_valor(ini, secao, {"usar_coordenadas", "use_coordinates"});
        if (!uc.empty()) {
            d.usar_coordenadas = para_bool(uc, false);
        } else if (!d.eixos.empty()) {
            d.usar_coordenadas = false;
            for (const auto& e : d.eixos) {
                if (e == "coordenadas" || e == "coordinates") d.usar_coordenadas = true;
            }
        } else {
            d.usar_coordenadas = !CFG.coordenadas.empty();
        }

        std::string idatas = primeiro_valor(ini, secao, {"iterar_datas", "iterate_dates"});
        if (!idatas.empty()) d.iterar_datas = para_bool(idatas, true);
        else d.iterar_datas = (d.granularidade != "nenhuma");

        if (d.usar_coordenadas) {
            bool tem_coord = false;
            for (const auto& e : d.eixos) {
                if (e == "coordenadas" || e == "coordinates") tem_coord = true;
            }
            if (!tem_coord) d.eixos.insert(d.eixos.begin(), "coordenadas");
        }

        if (d.tipo_saida == "arquivo" && d.url.empty()) d.url = obter_env_ou_config("API_URL", "", "");

        bool eh_drive = (d.tipo_saida == "drive" || d.tipo_saida == "google_drive" ||
                         !d.drive_folder_id.empty());
        bool eh_wiki = (d.tipo_saida == "wikipedia" || d.tipo_saida == "wiki");
        bool eh_video = (d.tipo_saida == "video" || d.tipo_saida == "stream");
        if (d.url.empty() && !eh_drive && !eh_wiki && !eh_video && d.lista_urls.empty()) {
            std::cerr << "[WARNING] Dataset '" << d.id << "' ignorado (url vazia).\n";
            continue;
        }

        out.push_back(std::move(d));
    }
    return out;
}

void carregar_config(const std::string& caminho) {
    Ini ini = ler_ini(caminho);
    std::string v;

    auto obter_geral = [&](const char* chave) -> bool {
        return ini.obter("geral", chave, v) || ini.obter("general", chave, v);
    };
    auto le_texto = [&](const char* chave, std::string& alvo) {
        if (obter_geral(chave)) alvo = v;
    };
    auto le_int = [&](const char* chave, const char* env, const char* env_legado, int& alvo) {
        if (obter_geral(chave)) alvo = para_int(v, alvo);
        if (env_legado) alvo = env_int(env_legado, alvo);
        if (env) alvo = env_int(env, alvo);
    };
    auto le_dbl = [&](const char* chave, const char* env, const char* env_legado, double& alvo) {
        if (obter_geral(chave)) alvo = para_double(v, alvo);
        if (env_legado) alvo = env_double(env_legado, alvo);
        if (env) alvo = env_double(env, alvo);
    };

    le_texto("db_path", CFG.db_path);
    le_int("max_sql_dates_por_lote", "DOWNLOAD_MAX_SQL_DATES", nullptr, CFG.max_sql_dates_por_lote);
    le_int("download_workers", "DOWNLOAD_WORKERS", "OPENMETEO_DOWNLOAD_WORKERS", CFG.download_workers);
    le_int("process_workers", "PROCESS_WORKERS", "OPENMETEO_PROCESS_WORKERS", CFG.process_workers);
    le_int("chunk_dias", "CHUNK_DIAS", "OPENMETEO_CHUNK_DIAS", CFG.chunk_dias);
    le_dbl("requests_per_second", "REQUESTS_PER_SECOND", "OPENMETEO_RPS", CFG.requests_per_second);
    le_int("max_retries", "MAX_RETRIES", "OPENMETEO_RETRIES", CFG.max_retries);
    le_dbl("http_timeout", "HTTP_TIMEOUT", "OPENMETEO_TIMEOUT", CFG.http_timeout);
    le_dbl("jitter_inicial_max", "JITTER_INICIAL_MAX", "OPENMETEO_JITTER", CFG.jitter_inicial_max);
    le_dbl("backoff_inicial", "BACKOFF_INICIAL", "OPENMETEO_BACKOFF", CFG.backoff_inicial);
    le_dbl("backoff_maximo", "BACKOFF_MAXIMO", "OPENMETEO_BACKOFF_MAX", CFG.backoff_maximo);
    le_dbl("min_rps", "MIN_RPS", "OPENMETEO_MIN_RPS", CFG.min_rps);
    le_dbl("cooldown_429", "COOLDOWN_429", "OPENMETEO_COOLDOWN_429", CFG.cooldown_429);
    le_int("circuit_limit_429", "CIRCUIT_LIMIT_429", "OPENMETEO_429_CIRCUIT_LIMIT", CFG.circuit_limit_429);
    le_dbl("espera_min_429", "ESPERA_MIN_429", "OPENMETEO_429_ESPERA_MIN", CFG.espera_min_429);
    le_dbl("limite_minuto", "LIMITE_MINUTO", "OPENMETEO_LIMITE_MINUTO", CFG.limite_minuto);
    le_dbl("limite_hora", "LIMITE_HORA", "OPENMETEO_LIMITE_HORA", CFG.limite_hora);
    le_dbl("limite_dia", "LIMITE_DIA", "OPENMETEO_LIMITE_DIA", CFG.limite_dia);
    le_dbl("custo_vars_ref", "CUSTO_VARS_REF", nullptr, CFG.custo_vars_ref);
    le_dbl("custo_dias_ref", "CUSTO_DIAS_REF", nullptr, CFG.custo_dias_ref);
    le_dbl("custo_dias_fator", "CUSTO_DIAS_FATOR", nullptr, CFG.custo_dias_fator);
    le_dbl("cooldown_quota", "COOLDOWN_QUOTA", "OPENMETEO_COOLDOWN_QUOTA", CFG.cooldown_quota);
    le_texto("api_keys", CFG.api_keys);
    le_texto("api_key_param", CFG.api_key_param);
    le_texto("fuso_horario", CFG.fuso_horario);
    le_texto("periodo_inicio", CFG.periodo_inicio);
    le_texto("periodo_fim", CFG.periodo_fim);
    { // limite global de tamanho (vale p/ todos os datasets que nao definirem o proprio)
        std::string gmax;
        for (const char* chave : {"tamanho_max_bytes", "tamanho_maximo", "tamanho_max",
                                  "max_bytes", "max_file_size", "max_size"}) {
            if (ini.obter("geral", chave, gmax) || ini.obter("general", chave, gmax)) break;
        }
        if (!gmax.empty()) CFG.tamanho_max_bytes = parse_tamanho_bytes(gmax, 0);
        // env tem prioridade se o ini nao definiu (0 = nao definido)
        if (CFG.tamanho_max_bytes <= 0) {
            const char* ev = std::getenv("TAMANHO_MAX_BYTES");
            if (!ev || !*ev) ev = std::getenv("MAX_FILE_SIZE");
            if (!ev || !*ev) ev = std::getenv("MAX_SIZE");
            if (ev && *ev) CFG.tamanho_max_bytes = parse_tamanho_bytes(ev, 0);
        }
    }

    auto coord_linhas = ini.lista("coordenadas");
    if (coord_linhas.empty()) coord_linhas = ini.lista("coordinates");
    if (!coord_linhas.empty()) {
        std::vector<std::pair<double, double>> coords;
        for (const auto& l : coord_linhas) {
            auto partes = dividir(l, ',');
            if (partes.size() < 2) {
                std::cerr << "[WARNING] Coordenada invalida ignorada: " << l << "\n";
                continue;
            }
            try {
                coords.emplace_back(std::stod(partes[0]), std::stod(partes[1]));
            } catch (...) {
                std::cerr << "[WARNING] Coordenada invalida ignorada: " << l << "\n";
            }
        }
        CFG.coordenadas = std::move(coords);
    }

    CFG.dimensoes.clear();
    for (const auto& secao : ini.ordem_secoes) {
        if (!eh_secao_dim(secao)) continue;
        DimensaoCfg dim;
        dim.nome = secao.rfind("dim:", 0) == 0 ? secao.substr(4) : secao.substr(5);
        std::string cols = primeiro_valor(ini, secao, {"colunas", "campos", "keys"});
        if (!cols.empty()) dim.campos = dividir(cols, ',');
        for (const auto& l : ini.lista(secao)) {
            auto partes = dividir(l, ',');
            partes.erase(std::remove_if(partes.begin(), partes.end(), [](const std::string& s) {
                return s.empty();
            }), partes.end());
            if (!partes.empty()) dim.linhas.push_back(std::move(partes));
        }
        if (dim.linhas.empty()) {
            std::cerr << "[WARNING] Dimensao '" << dim.nome << "' vazia, ignorada.\n";
            continue;
        }
        CFG.dimensoes.push_back(std::move(dim));
    }

    if (CFG.download_workers < 1) CFG.download_workers = 1;
    if (CFG.process_workers < 1) CFG.process_workers = 1;
    if (CFG.chunk_dias < 1) CFG.chunk_dias = 1;
    if (CFG.max_sql_dates_por_lote < 1) CFG.max_sql_dates_por_lote = 1;
    if (CFG.requests_per_second <= 0.0) CFG.requests_per_second = 1.0;
    if (CFG.max_retries < 1) CFG.max_retries = 1;
    if (CFG.limite_minuto < 0.0) CFG.limite_minuto = 0.0;
    if (CFG.limite_hora < 0.0) CFG.limite_hora = 0.0;
    if (CFG.limite_dia < 0.0) CFG.limite_dia = 0.0;
    if (CFG.custo_vars_ref <= 0.0) CFG.custo_vars_ref = 10.0;
    if (CFG.custo_dias_ref <= 0.0) CFG.custo_dias_ref = 14.0;
    if (CFG.custo_dias_fator <= 0.0) CFG.custo_dias_fator = 1.5;
    if (CFG.cooldown_quota <= 0.0) CFG.cooldown_quota = 300.0;
    if (CFG.api_key_param.empty()) CFG.api_key_param = "apikey";
    if (CFG.min_rps <= 0.0) CFG.min_rps = CFG.requests_per_second * 0.1;
    if (CFG.min_rps > CFG.requests_per_second) CFG.min_rps = CFG.requests_per_second;
    CFG.datasets = montar_datasets_genericos(ini);
    std::cout << "[CFG] download_workers=" << CFG.download_workers
              << " process_workers=" << CFG.process_workers
              << " (fonte: " << caminho << ")\n";
    if (CFG.tamanho_max_bytes > 0)
        std::cout << "[CFG] tamanho_max_bytes global=" << formatar_bytes(CFG.tamanho_max_bytes)
                  << " (" << CFG.tamanho_max_bytes << " bytes)\n";
    for (auto& d : CFG.datasets) {
        if (d.db.empty()) d.db = CFG.db_path;
        if (d.api_keys.empty()) d.api_keys = CFG.api_keys;
        if (d.tamanho_max_bytes < 0) d.tamanho_max_bytes = CFG.tamanho_max_bytes; // herda global
    }
}
