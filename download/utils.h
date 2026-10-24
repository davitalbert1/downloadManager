#pragma once
#include <cstddef>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <ctime>

// Manipulação e sanitização de strings
std::string cortar(const std::string& s);
std::string minusculas(std::string v);
std::string sem_comentario(const std::string& s);
// Expande ~, %VAR%, $VAR/${VAR} e remove aspas externas; preserva path absoluto.
std::string expandir_caminho(const std::string& caminho);
// Resolve pasta de saída: cfg explícito tem prioridade; se vazio/igual ao padrão,
// usa env (se informado); nunca junta CWD a path absoluto. Retorna string pronta p/ fs::path.
std::string resolver_pasta_saida(const std::string& valor_cfg, const std::string& padrao,
                                 const char* nome_env = nullptr);
std::vector<std::string> dividir(const std::string& s, char sep);
std::string juntar(const std::vector<std::string>& itens, const std::string& sep);
std::string num_str(double v);
std::string normalizar_nome(const std::string& s);
bool coluna_reservada(const std::string& c);
std::string normalizar_tipo(const std::string& t);
bool separar_def_coluna(const std::string& def, std::string& coluna,
                        std::string& variavel_api, std::string& tipo);

int para_int(const std::string& s, int padrao);
double para_double(const std::string& s, double padrao);
bool para_bool(const std::string& s, bool padrao);

// Sanitização de nomes de arquivos para qualquer SO (Windows/Linux)
std::string sanitizar_nome_arquivo(const std::string& nome, size_t tam_max = 180);
std::string obter_nome_arquivo_da_url(const std::string& url);

// Tamanhos de arquivo: aceita bytes puros ou sufixos KB/MB/GB/TB (ex: "500MB",
// "1.5GB", "102400", "0" = sem limite). Retorna bytes; <=0 = sem limite.
long long parse_tamanho_bytes(const std::string& s, long long padrao = 0);
std::string formatar_bytes(long long b);

// RNG thread-local (evita disputa e resultados iguais entre workers)
std::mt19937& rng_thread_local();
// Formata segundos como HH:MM:SS; retorna "--:--:--" para valores invalidos.
std::string formatar_duracao(double segundos);
int dias_no_mes(int ano, int mes);

// Executa fn(0..total-1) em n_workers threads. Se n_workers <= 1, roda inline.
void executar_paralelo(int n_workers, size_t total, const std::function<void(size_t)>& fn);

// Utilitários de Data e Hora (Howard Hinnant civil days)
struct Data {
    int ano = 0, mes = 0, dia = 0;
};

Data para_data(const std::string& s);
long dias_de_civil(int y, int m, int d);
Data civil_de_dias(long z);
std::string data_texto(const Data& d);
std::tm agora_local();
std::string hoje();
std::string agora_iso();
std::vector<std::string> expandir_periodo(const std::string& inicio, const std::string& fim);

// Thread-safe logger
void log(const std::string& msg);

// Garante que a pasta-pai de um arquivo exista (cria `a/b/` para `a/b/c.db`).
// Retorna true se OK ou se nao ha pasta a criar; false se falhou.
bool garantir_pasta_pai(const std::string& caminho_arquivo);

// Extração genérica de links e mídias de documentos HTML, texto e playlists M3U/M3U8
std::vector<std::string> extrair_urls_de_conteudo(const std::string& conteudo, const std::string& base_url, const std::string& modo);
