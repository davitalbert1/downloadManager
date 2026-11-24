#pragma once
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// Sistema de tradução i18n baseado em catálogos JSON (pt, en, es).
//
// As chaves dos catálogos são as próprias mensagens em português (estilo
// gettext), com placeholders no formato {nome}. Se uma tradução não existir,
// a mensagem original é mantida — o programa nunca quebra por falta de chave.
//
// Uso recomendado em código novo:
//   std::string msg = i18n::t("Baixando {arquivo}", {{"arquivo", nome}});
//
// Para código legado, `log()` e os streams `i18n::cout` / `i18n::cerr`
// traduzem automaticamente cada linha já montada, inclusive mensagens com
// valores dinâmicos que casem com um modelo do catálogo.
namespace i18n {

using Variaveis = std::map<std::string, std::string>;

// Carrega os dicionários de `diretorio` (ou de ./i18n, <exe>/i18n, ...).
// `idioma` vazio ou "auto" detecta por DOWNLOAD_LANG/LC_ALL/LANG e usa pt
// como último recurso. Pode ser chamado mais de uma vez.
void iniciar(const std::string& idioma = "", const std::string& diretorio = "");

// Troca o idioma atual (pt, en, es ou variantes como pt-BR/en_US).
// Retorna false quando o catálogo do idioma não foi encontrado.
bool definir_idioma(const std::string& codigo);

std::string idioma_atual();
std::vector<std::string> idiomas_disponiveis();

// Traduz uma chave/mensagem exata, aplicando os placeholders informados.
std::string t(const std::string& chave);
std::string t(const std::string& chave, const Variaveis& vars);

// Traduz uma mensagem já formatada (legado). Tenta a mensagem completa,
// depois cada linha e, por fim, modelos com {placeholders}. Sem correspondência,
// devolve a mensagem original.
std::string traduzir(const std::string& mensagem);

// Stream que acumula até \n, \r ou flush, traduz a linha e escreve no destino.
class SaidaTraduzida {
public:
    explicit SaidaTraduzida(std::ostream& destino) : destino_(destino) {}
    ~SaidaTraduzida() {
        std::lock_guard<std::mutex> lk(mtx_);
        descarregar_locked();
    }

    SaidaTraduzida(const SaidaTraduzida&) = delete;
    SaidaTraduzida& operator=(const SaidaTraduzida&) = delete;

    SaidaTraduzida& operator<<(const std::string& v) {
        std::lock_guard<std::mutex> lk(mtx_);
        acrescentar_locked(v);
        return *this;
    }
    SaidaTraduzida& operator<<(const char* v) {
        return operator<<(std::string(v ? v : ""));
    }
    SaidaTraduzida& operator<<(char v) {
        return operator<<(std::string(1, v));
    }
    SaidaTraduzida& operator<<(std::ostream& (*manip)(std::ostream&)) {
        std::lock_guard<std::mutex> lk(mtx_);
        descarregar_locked();
        manip(destino_);
        return *this;
    }
    SaidaTraduzida& operator<<(std::ios_base& (*manip)(std::ios_base&)) {
        std::lock_guard<std::mutex> lk(mtx_);
        manip(destino_);
        return *this;
    }
    template <typename T>
    SaidaTraduzida& operator<<(const T& v) {
        std::ostringstream tmp;
        tmp.copyfmt(destino_);
        tmp << v;
        return operator<<(tmp.str());
    }

    void flush() {
        std::lock_guard<std::mutex> lk(mtx_);
        descarregar_locked();
        destino_.flush();
    }

private:
    void acrescentar_locked(const std::string& texto);
    void descarregar_locked();

    std::ostream& destino_;
    std::string buffer_;
    std::mutex mtx_;
};

// Fluxos globais traduzidos. Substituem std::cout/std::cerr em código legado.
// Alocados sem destrutor para evitar ordem de destruição estática; cada linha
// já é descarregada e traduzida no operador <<.
inline SaidaTraduzida& cout = *new SaidaTraduzida(std::cout);
inline SaidaTraduzida& cerr = *new SaidaTraduzida(std::cerr);

} // namespace i18n
