#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

using Dicionario = std::map<std::string, std::string>;

struct Modelo {
    std::string chave;
    std::vector<std::string> estaticos; // n+1 partes para n placeholders
    std::vector<std::string> nomes;
    std::string prefixo;
    size_t peso = 0;
};

std::map<std::string, Dicionario> g_dic;
std::vector<Modelo> g_modelos;
std::string g_idioma = "pt";
std::mutex g_init_mtx;

std::string minusculas(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string normalizar_idioma(const std::string& codigo) {
    std::string s = minusculas(codigo);
    size_t fim = s.find_first_of("-_.");
    if (fim != std::string::npos) s = s.substr(0, fim);
    if (s == "pt" || s == "en" || s == "es") return s;
    if (!s.empty() && s.rfind("pt", 0) == 0) return "pt";
    if (!s.empty() && s.rfind("en", 0) == 0) return "en";
    if (!s.empty() && s.rfind("es", 0) == 0) return "es";
    return "";
}

std::string detectar_idioma_ambiente() {
    const char* nomes[] = {"DOWNLOAD_LANG", "APP_LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
    for (const char* nome : nomes) {
        const char* valor = std::getenv(nome);
        if (!valor || !*valor) continue;
        std::string bruto(valor);
        // LANGUAGE pode ser "en_US:pt_BR:es"; usa a primeira variante conhecida.
        size_t ini = 0;
        while (ini <= bruto.size()) {
            size_t fim = bruto.find(':', ini);
            std::string parte = bruto.substr(ini, fim == std::string::npos ? std::string::npos : fim - ini);
            std::string norm = normalizar_idioma(parte);
            if (!norm.empty()) return norm;
            if (fim == std::string::npos) break;
            ini = fim + 1;
        }
    }
    return "";
}

fs::path pasta_executavel() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(std::string(buf, n)).parent_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    std::error_code ec;
    return fs::current_path(ec);
}

void achatar_json(const nlohmann::json& j, const std::string& prefixo,
                  Dicionario& destino) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string chave = prefixo.empty() ? it.key() : prefixo + "." + it.key();
            achatar_json(it.value(), chave, destino);
        }
    } else if (j.is_string()) {
        destino[prefixo] = j.get<std::string>();
    }
}

void carregar_json(const fs::path& caminho, Dicionario& destino) {
    std::ifstream arq(caminho, std::ios::binary);
    if (!arq) return;
    try {
        nlohmann::json j = nlohmann::json::parse(arq);
        achatar_json(j, "", destino);
    } catch (const std::exception&) {
        // Catálogo inválido é ignorado; a mensagem original continua sendo usada.
    }
}

bool nome_placeholder_valido(const std::string& nome) {
    if (nome.empty()) return false;
    for (char c : nome) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

Modelo montar_modelo(const std::string& chave) {
    Modelo m;
    m.chave = chave;
    std::string estatico;
    size_t i = 0;
    while (i < chave.size()) {
        char c = chave[i];
        if (c == '{') {
            if (i + 1 < chave.size() && chave[i + 1] == '{') {
                estatico.push_back('{');
                i += 2;
                continue;
            }
            size_t fim = chave.find('}', i + 1);
            if (fim != std::string::npos) {
                std::string nome = chave.substr(i + 1, fim - i - 1);
                if (nome_placeholder_valido(nome)) {
                    m.estaticos.push_back(estatico);
                    estatico.clear();
                    m.nomes.push_back(nome);
                    i = fim + 1;
                    continue;
                }
            }
        } else if (c == '}' && i + 1 < chave.size() && chave[i + 1] == '}') {
            estatico.push_back('}');
            i += 2;
            continue;
        }
        estatico.push_back(c);
        ++i;
    }
    m.estaticos.push_back(estatico);
    if (m.nomes.empty()) return Modelo{};
    m.prefixo = m.estaticos.front();
    for (const auto& s : m.estaticos) m.peso += s.size();
    // Modelos só com placeholders casariam com qualquer texto; ignoramos.
    if (m.peso == 0) return Modelo{};
    return m;
}

void reconstruir_modelos() {
    std::set<std::string> chaves;
    for (const auto& par : g_dic) {
        for (const auto& item : par.second) chaves.insert(item.first);
    }
    g_modelos.clear();
    for (const auto& chave : chaves) {
        Modelo m = montar_modelo(chave);
        if (!m.chave.empty()) g_modelos.push_back(std::move(m));
    }
    std::sort(g_modelos.begin(), g_modelos.end(), [](const Modelo& a, const Modelo& b) {
        if (a.peso != b.peso) return a.peso > b.peso;
        return a.chave > b.chave;
    });
}

std::string desescapar(const std::string& texto) {
    std::string saida;
    saida.reserve(texto.size());
    for (size_t i = 0; i < texto.size(); ++i) {
        if (texto[i] == '{' && i + 1 < texto.size() && texto[i + 1] == '{') {
            saida.push_back('{');
            ++i;
        } else if (texto[i] == '}' && i + 1 < texto.size() && texto[i + 1] == '}') {
            saida.push_back('}');
            ++i;
        } else {
            saida.push_back(texto[i]);
        }
    }
    return saida;
}

std::string aplicar_variaveis(const std::string& texto, const i18n::Variaveis& vars) {
    if (vars.empty() || texto.find('{') == std::string::npos) return texto;
    std::string saida;
    saida.reserve(texto.size());
    for (size_t i = 0; i < texto.size();) {
        if (texto[i] == '{') {
            size_t fim = texto.find('}', i + 1);
            if (fim != std::string::npos) {
                std::string nome = texto.substr(i + 1, fim - i - 1);
                auto it = vars.find(nome);
                if (it != vars.end()) {
                    saida += it->second;
                    i = fim + 1;
                    continue;
                }
            }
        }
        saida.push_back(texto[i]);
        ++i;
    }
    return saida;
}

const std::string* procurar(const std::string& chave) {
    auto it = g_dic.find(g_idioma);
    if (it != g_dic.end()) {
        auto jt = it->second.find(chave);
        if (jt != it->second.end()) return &jt->second;
    }
    auto pt = g_dic.find("pt");
    if (pt != g_dic.end()) {
        auto jt = pt->second.find(chave);
        if (jt != pt->second.end()) return &jt->second;
    }
    return nullptr;
}

bool casar_modelo(const std::string& msg, const Modelo& m, i18n::Variaveis& vals) {
    const std::string& primeiro = m.estaticos.front();
    if (msg.size() < primeiro.size() || msg.compare(0, primeiro.size(), primeiro) != 0) return false;
    size_t pos = primeiro.size();
    for (size_t i = 0; i < m.nomes.size(); ++i) {
        const std::string& seg = m.estaticos[i + 1];
        if (i + 1 == m.nomes.size() && seg.empty()) {
            vals[m.nomes[i]] = msg.substr(pos);
            pos = msg.size();
        } else {
            size_t p = msg.find(seg, pos);
            if (p == std::string::npos) return false;
            vals[m.nomes[i]] = msg.substr(pos, p - pos);
            pos = p + seg.size();
        }
    }
    return pos == msg.size();
}

std::string traduzir_linha(const std::string& linha) {
    if (linha.empty() || g_dic.empty()) return linha;

    if (const std::string* direta = procurar(linha)) {
        return desescapar(*direta);
    }

    for (const Modelo& m : g_modelos) {
        if (linha.size() < m.prefixo.size() ||
            linha.compare(0, m.prefixo.size(), m.prefixo) != 0) {
            continue;
        }
        i18n::Variaveis vals;
        if (!casar_modelo(linha, m, vals)) continue;
        const std::string* traducao = procurar(m.chave);
        if (!traducao) continue;
        return desescapar(aplicar_variaveis(*traducao, vals));
    }
    return linha;
}

} // namespace

namespace i18n {

void iniciar(const std::string& idioma, const std::string& diretorio) {
    std::lock_guard<std::mutex> lk(g_init_mtx);

    std::map<std::string, Dicionario> novos;
    std::vector<fs::path> candidatos;
    auto adicionar = [&](const fs::path& p) {
        if (!p.empty()) candidatos.push_back(p);
    };

    if (!diretorio.empty()) adicionar(fs::path(diretorio));
    if (const char* env = std::getenv("DOWNLOAD_I18N_DIR"); env && *env) {
        adicionar(fs::path(env));
    }
    fs::path exe = pasta_executavel();
    adicionar(exe / "i18n");
    adicionar(exe.parent_path() / "i18n");

    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        adicionar(cwd / "i18n");
        adicionar(cwd.parent_path() / "i18n");
    }

    for (const fs::path& dir : candidatos) {
        if (!fs::is_directory(dir, ec)) continue;
        bool achou = false;
        for (const char* lang : {"pt", "en", "es"}) {
            fs::path arquivo = dir / (std::string(lang) + ".json");
            if (fs::is_regular_file(arquivo, ec)) {
                carregar_json(arquivo, novos[lang]);
                achou = true;
            }
        }
        if (achou) break;
    }

    g_dic = std::move(novos);
    reconstruir_modelos();

    std::string escolhido = normalizar_idioma(idioma);
    if (escolhido.empty()) escolhido = detectar_idioma_ambiente();
    if (escolhido.empty()) escolhido = "pt";
    g_idioma = escolhido;
}

bool definir_idioma(const std::string& codigo) {
    std::lock_guard<std::mutex> lk(g_init_mtx);
    std::string norm = normalizar_idioma(codigo);
    if (norm.empty()) return false;
    g_idioma = norm;
    return g_dic.count(norm) > 0;
}

std::string idioma_atual() {
    return g_idioma;
}

std::vector<std::string> idiomas_disponiveis() {
    std::vector<std::string> out;
    for (const char* lang : {"pt", "en", "es"}) {
        if (g_dic.count(lang)) out.emplace_back(lang);
    }
    return out;
}

std::string t(const std::string& chave) {
    return t(chave, {});
}

std::string t(const std::string& chave, const Variaveis& vars) {
    const std::string* texto = procurar(chave);
    return aplicar_variaveis(texto ? desescapar(*texto) : chave, vars);
}

std::string traduzir(const std::string& mensagem) {
    if (mensagem.empty() || g_dic.empty()) return mensagem;

    std::string saida;
    saida.reserve(mensagem.size() + 16);
    size_t ini = 0;
    while (ini < mensagem.size()) {
        size_t fim = mensagem.find_first_of("\r\n", ini);
        std::string linha = mensagem.substr(
            ini, fim == std::string::npos ? std::string::npos : fim - ini);
        std::string terminador;
        if (fim != std::string::npos) {
            if (mensagem[fim] == '\r' && fim + 1 < mensagem.size() &&
                mensagem[fim + 1] == '\n') {
                terminador = "\r\n";
            } else {
                terminador.assign(1, mensagem[fim]);
            }
        }
        saida += traduzir_linha(linha);
        saida += terminador;
        if (fim == std::string::npos) break;
        ini = fim + terminador.size();
    }
    return saida;
}

void SaidaTraduzida::acrescentar_locked(const std::string& texto) {
    buffer_ += texto;
    size_t pos;
    while ((pos = buffer_.find_first_of("\r\n")) != std::string::npos) {
        std::string linha = buffer_.substr(0, pos);
        std::string terminador(1, buffer_[pos]);
        buffer_.erase(0, pos + 1);
        if (terminador[0] == '\r' && !buffer_.empty() && buffer_[0] == '\n') {
            terminador += '\n';
            buffer_.erase(0, 1);
        }
        destino_ << traduzir(linha) << terminador;
        destino_.flush();
    }
}

void SaidaTraduzida::descarregar_locked() {
    if (buffer_.empty()) {
        destino_.flush();
        return;
    }
    destino_ << traduzir(buffer_);
    buffer_.clear();
    destino_.flush();
}

} // namespace i18n
