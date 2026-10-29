#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>

// Contexto genérico para interpolação de variáveis: {chave} -> valor
using Contexto = std::map<std::string, std::string>;

std::string aplicar_ctx(std::string s, const Contexto& ctx);
bool chave_ctx_interna(const std::string& k);

struct ColunaCfg {
    std::string coluna; // nome da coluna no SQLite (normalizado)
    std::string variavel_api; // nome da variável na API/JSON
    std::string tipo; // "auto" | "real" | "integer" | "text"
};

struct DatasetCfg {
    std::string id; // id da seção (ex: "clima_diario")
    std::string db; // arquivo .db (vazio = padrão do [geral])
    std::string tabela; // tabela no SQLite
    std::string url; // URL base
    std::string url_passado; // URL para datas < hoje (opcional)
    std::string url_futuro; // URL para datas >= hoje (opcional)
    std::string bloco; // caminho do bloco no JSON (daily, hourly, etc)
    std::string params_chave; // query param que lista as variáveis (daily=...)
    std::vector<std::string> vars; // lista de variáveis pedidas
    std::set<std::string> vars_excluir; // variáveis omitidas no passado/archive
    std::vector<ColunaCfg> colunas; // mapeamento/tipagem de colunas
    std::string data_min; // filtro mínimo (YYYY-MM-DD ou "hoje")
    std::string data_max; // filtro máximo (YYYY-MM-DD ou "hoje")
    std::string chave_data = "data"; // nome da coluna/campo de data
    std::string chave_hora = "hora"; // nome da coluna/campo de hora
    bool salvar_hora = false;
    bool salvar_timezone = false;
    std::string granularidade = "dia"; // dia | hora | nenhuma
    std::string api_keys; // chaves rotativas "k1 | k2"
    bool usar_coordenadas = false; // itera [coordenadas] e injeta lat/lon
    bool iterar_datas = true; // fatia o período em start_date/end_date
    std::string metodo = "GET"; // GET, POST, etc.
    std::string corpo; // HTTP body (templates {chave})
    std::string formato = "auto";// auto | series | records | object
    std::string chave_tempo = "time";
    std::string chave_unica; // colunas UNIQUE personalizadas (csv)
    // Num 429 persistente, tenta este dataset (mesma tabela/colunas via
    // coluna.*). Use so_fallback = 1 no dataset alternativo para ele nao
    // ser executado por conta propria.
    std::string fallback;
    bool so_fallback = false;
    // APIs como Visual Crossing devolvem varios dias, cada um com um array
    // "hours"; expandir_horas transforma cada hora em um registro.
    bool expandir_horas = false;
    std::string campo_horas = "hours";
    std::vector<std::string> eixos; // eixos de produto cartesiano (dim:* e/ou coordenadas)
    std::map<std::string, std::string> params; // query params adicionais
    std::map<std::string, std::string> headers; // headers HTTP adicionais
    std::string api_key_em = "query"; // query | header | basic | auto | none
    std::string api_key_header = "Authorization";
    std::string api_key_prefix = "Bearer";
    std::string api_key_param; // nome do parâmetro na query (vazio = [geral])
    std::string api_key_env; // variável de ambiente da chave (vazio = API_KEY)

    // Campos para salvamento em arquivos (APIs de arquivos/jobs assíncronos)
    std::string tipo_saida = "db"; // "db" (sqlite) ou "arquivo" (pasta)
    std::string pasta_saida = "dados_arquivos";
    std::string extensao = "bin";
    std::string dataset_api;
    std::string product_type;
    std::string data_format;
    std::string download_format;
    std::string padrao_arquivo; // template do nome do arquivo final, ex: "{id}_{ano}_{mes}.{extensao}"

    // Fluxo genérico de jobs assíncronos: o POST inicial devolve um id e o
    // resultado é consultado depois. Templates aceitam {base}, {dataset_api}
    // e {job_id}; caminhos relativos são prefixados com a URL base.
    std::string job_executar; // POST que cria o job (vazio = a própria url)
    std::string job_status; // consulta do estado do job
    std::string job_resultado; // consulta do resultado do job
    std::string job_listar_ativos; // lista jobs em andamento (opcional)
    std::string job_listar_ok; // lista jobs concluídos (opcional)
    std::string job_lista_campo = "jobs"; // campo raiz com o array de jobs
    std::string job_campo_dataset; // campo do job com o nome do dataset
    std::string job_campo_id; // campo com o id do job (vazio = detecta)
    std::string job_campo_status = "status"; // campo com o estado do job
    std::string job_campo_asset; // caminho (ex: metadata.results) do objeto com a URL
    std::vector<std::string> job_status_ok; // vazio = lista padrão
    std::vector<std::string> job_status_erro; // vazio = lista padrão
    std::vector<std::string> job_reenviar_se; // trechos de erro que pedem nova tentativa
    double job_poll_intervalo = 10.0; // segundos entre consultas de status
    bool job_sequencial = false; // mantém 1 worker (filas com limite)
    bool job_cancelar_duplicados = true; // cancela jobs antigos do mesmo dataset
    std::vector<std::string> horarios;
    int ano_inicial = 1940;
    int dias_atraso = 0;
    std::string modo_api = "auto";
    bool auto_importar_banco = false; // Importa para o banco SQLite se suportado
    bool evitar_download_se_no_banco = false; // Não baixa se dados já existirem no banco

    // Suporte genérico a extração de mídias, links e arquivos em cascata (HTML, regex, JSON assets, M3U/M3U8)
    bool baixar_arquivos_vinculados = false; // Baixa URLs encontradas no corpo ou nos campos
    std::string campo_url_arquivo; // Nome do campo JSON que contém a URL do arquivo a baixar
    std::string campo_nome_arquivo; // Nome do campo JSON para o nome do arquivo local
    std::vector<std::string> lista_urls; // URLs diretas listadas na seção
    std::string padrao_extracao; // Regex para extrair URLs em páginas HTML/texto (ou "html_imgs", "html_links", "m3u")
    bool usar_yt_dlp = false; // Se true e for stream/video suportado, usa yt-dlp como fallback
    int tamanho_min_bytes = 0; // Se > 0, descarta arquivos menores que isso (evita imagens corrompidas/placeholders)
    long long tamanho_max_bytes = -1; // -1 = herda do [geral]; 0 = sem limite; >0 = pula maiores.

    // Suporte ao Google Drive
    std::string drive_folder_id;
    std::string drive_service_account;

    // Follow links detalhados para APIs com endpoint mestre-detalhe (ex: PokeAPI, Swapi, Rick&Morty)
    bool seguir_url_detalhe = false; // Se true, acessa cada 'url' do registro para baixar o objeto completo
    std::string campo_url_detalhe = "url"; // Campo que traz a URL de detalhe (padrao: "url")
    bool baixar_todas_imagens = false; // Se true, busca todas as URLs de imagem (.png, .jpg, .svg) nos objetos
    std::string pasta_imagens = "imagens"; // Subpasta/pasta para salvar imagens encontradas

    // Wikipedia/Wikimedia dumps (.xml.bz2 -> SQLite, espelha F:\wikipedia)
    // tipo_saida = wikipedia
    std::string projeto; // ex: enwiki (vazio = deduz do nome do arquivo)
    std::string data_dump; // ex: 2026-07-01 (vazio = deduz do nome/URL)
    bool apenas_ns0 = true; // so namespace 0 (artigos)
    bool ignorar_redirect = true; // pula #REDIRECT/#REDIRECIONAMENTO
    int tamanho_minimo = 20; // descarta texto limpo menor que isso
    int wiki_batch = 2000; // paginas por COMMIT
    bool listar_remoto = true; // lista o diretorio remoto; 0 = so pasta local
};

struct DimensaoCfg {
    std::string nome;
    std::vector<std::string> campos;
    std::vector<std::vector<std::string>> linhas;
};

struct Config {
    std::string db_path = "clima.db";
    int max_sql_dates_por_lote = 900;
    int download_workers = 5;
    int process_workers = 2;
    int chunk_dias = 30;
    double requests_per_second = 4.0;
    int max_retries = 8;
    double http_timeout = 120.0;
    double jitter_inicial_max = 5.0;
    double backoff_inicial = 2.0;
    double backoff_maximo = 60.0;
    double min_rps = 0.5;
    double cooldown_429 = 30.0;
    int circuit_limit_429 = 3;
    double espera_min_429 = 5.0;
    double limite_minuto = 600.0;
    double limite_hora = 5000.0;
    double limite_dia = 10000.0;
    double custo_vars_ref = 10.0;
    double custo_dias_ref = 14.0;
    double custo_dias_fator = 1.5;
    double cooldown_quota = 300.0;
    long long tamanho_max_bytes = 0; // Limite global (bytes). Dataset pode sobrescrever.
    std::string api_keys;
    std::string api_key_param = "apikey";
    std::string fuso_horario = "America/Sao_Paulo";
    std::string periodo_inicio = "1940-01-01";
    std::string periodo_fim = "2025-12-31";

    std::vector<std::pair<double, double>> coordenadas;
    std::vector<DimensaoCfg> dimensoes;
    std::vector<DatasetCfg> datasets;
};

extern Config CFG;

struct Ini {
    std::map<std::string, std::map<std::string, std::string>> valores;
    std::map<std::string, std::vector<std::string>> listas;
    std::vector<std::string> ordem_secoes;

    bool obter(const std::string& sec, const std::string& chave, std::string& saida) const;
    std::string obter_ou(const std::string& sec, const std::string& chave,
                         const std::string& padrao = "") const;
    std::vector<std::string> lista(const std::string& sec) const;
    bool tem_secao(const std::string& sec) const;
};

void carregar_env(const std::string& caminho = ".env");
std::string obter_env_ou_config(const char* nome_env, const std::string& valor_cfg,
                                const std::string& padrao = "");
Ini ler_ini(const std::string& caminho);
void carregar_config(const std::string& caminho);

// Contextos de download do dataset: coordenadas (com fallback 0,0) ou eixos [dim:*].
std::vector<Contexto> contextos_de_download(const DatasetCfg& ds);
// Chave estável do contexto usada em existentes/chave_existentes.
std::string chave_contexto(const Contexto& ctx);
// true para datasets baixados pelo engine generico (SQLite).
bool dataset_generico(const DatasetCfg& ds);
std::string resolver_data_limite(const std::string& v, const std::string& padrao);
bool data_no_intervalo(const std::string& d, const DatasetCfg& ds);
