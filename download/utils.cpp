#include "utils.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

static std::mutex g_print_mtx;

void log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_print_mtx);
    // Escreve mensagem + '\n' num único fwrite para evitar que outra thread
    // intercale saída entre a mensagem e a quebra de linha.
    std::string linha = msg;
    linha.push_back('\n');
    fwrite(linha.data(), 1, linha.size(), stdout);
    fflush(stdout);
}

std::string cortar(const std::string& s) {
    const char* espacos = " \t\r\n";
    size_t i = s.find_first_not_of(espacos);
    if (i == std::string::npos) return "";
    size_t f = s.find_last_not_of(espacos);
    return s.substr(i, f - i + 1);
}

std::string minusculas(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return v;
}

std::string sem_comentario(const std::string& s) {
    size_t p = s.find_first_of(";#");
    return p == std::string::npos ? s : s.substr(0, p);
}

std::vector<std::string> dividir(const std::string& s, char sep) {
    std::vector<std::string> partes;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) partes.push_back(cortar(item));
    return partes;
}

std::string juntar(const std::vector<std::string>& itens, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < itens.size(); ++i) {
        if (i) out += sep;
        out += itens[i];
    }
    return out;
}

std::string num_str(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    if (res.ec == std::errc()) return std::string(buf, res.ptr);
    std::ostringstream oss;
    oss.precision(std::numeric_limits<double>::max_digits10);
    oss << v;
    return oss.str();
}

std::string normalizar_nome(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
        else if (c == '_' || c == '-' || c == '.' || c == ' ' || c == '/')
            out += '_';
    }
    std::string limpo;
    bool ultimo_sep = true;
    for (char c : out) {
        bool sep = (c == '_');
        if (sep && ultimo_sep) continue;
        limpo += c;
        ultimo_sep = sep;
    }
    while (!limpo.empty() && limpo.back() == '_') limpo.pop_back();
    if (limpo.empty()) limpo = "col";
    if (std::isdigit(static_cast<unsigned char>(limpo.front()))) limpo = "c_" + limpo;
    return limpo;
}

bool coluna_reservada(const std::string& c) {
    return c == "data" || c == "hora" || c == "timezone" || c == "latitude" ||
           c == "longitude" || c == "created_at";
}

std::string normalizar_tipo(const std::string& t) {
    std::string v = cortar(t);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v.empty() || v == "auto") return "auto";
    if (v == "int" || v == "integer" || v == "bool" || v == "boolean") return "integer";
    if (v == "real" || v == "float" || v == "double" || v == "number" || v == "numeric")
        return "real";
    return "text";
}

bool separar_def_coluna(const std::string& def, std::string& coluna,
                        std::string& variavel_api, std::string& tipo) {
    std::string d = cortar(def);
    if (d.empty()) return false;
    size_t igual = d.find('=');
    std::string esq, dir;
    if (igual == std::string::npos) {
        esq = d;
        dir = d;
    } else {
        esq = cortar(d.substr(0, igual));
        dir = cortar(d.substr(igual + 1));
        if (esq.empty()) esq = dir;
        if (dir.empty()) dir = esq;
    }
    size_t dp = dir.find(':');
    std::string var = cortar(dp == std::string::npos ? dir : dir.substr(0, dp));
    std::string tp = dp == std::string::npos ? "auto" : cortar(dir.substr(dp + 1));
    if (var.empty()) var = esq;
    coluna = normalizar_nome(esq);
    variavel_api = var.empty() ? coluna : var;
    tipo = normalizar_tipo(tp);
    return !coluna.empty();
}

int para_int(const std::string& s, int padrao) {
    try {
        return std::stoi(cortar(s));
    } catch (...) {
        return padrao;
    }
}

double para_double(const std::string& s, double padrao) {
    try {
        return std::stod(cortar(s));
    } catch (...) {
        return padrao;
    }
}

bool para_bool(const std::string& s, bool padrao) {
    std::string v = cortar(s);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "1" || v == "true" || v == "sim" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "nao" || v == "não" || v == "no" || v == "off" ||
        v == "desligado")
        return false;
    return padrao;
}

Data para_data(const std::string& s) {
    Data d;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &d.ano, &d.mes, &d.dia) != 3 || d.mes < 1 ||
        d.mes > 12 || d.dia < 1 || d.dia > 31)
        throw std::runtime_error("data invalida: " + s);
    return d;
}

long dias_de_civil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

Data civil_de_dias(long z) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = static_cast<long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    Data r;
    r.ano = static_cast<int>(y + (m <= 2));
    r.mes = static_cast<int>(m);
    r.dia = static_cast<int>(d);
    return r;
}

std::string data_texto(const Data& d) {
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", d.ano, d.mes, d.dia);
    return b;
}

std::tm agora_local() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    #ifdef _WIN32
        localtime_s(&tmv, &t);
    #else
        localtime_r(&t, &tmv);
    #endif
    return tmv;
}

std::string hoje() {
    std::tm tmv = agora_local();
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return b;
}

std::string agora_iso() {
    std::tm tmv = agora_local();
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tmv);
    return b;
}

std::vector<std::string> expandir_periodo(const std::string& inicio, const std::string& fim) {
    try {
        Data a = para_data(inicio), b = para_data(fim);
        long ia = dias_de_civil(a.ano, a.mes, a.dia);
        long ib = dias_de_civil(b.ano, b.mes, b.dia);
        std::vector<std::string> datas;
        for (long i = ia; i <= ib; ++i) datas.push_back(data_texto(civil_de_dias(i)));
        return datas;
    } catch (const std::exception& e) {
        log(std::string("[WARNING] Erro ao expandir datas: ") + e.what());
        return {inicio, fim};
    }
}

std::string sanitizar_nome_arquivo(const std::string& nome, size_t tam_max) {
    std::string safe = nome;
    const std::string invalidChars = "\\/:*?\"<>|";
    for (char& c : safe) {
        if (invalidChars.find(c) != std::string::npos || static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127) {
            c = '_';
        }
    }
    // Remove múltiplos espaços/sublinhados repetidos
    std::string limpo;
    bool ultimo_esp = false;
    for (char c : safe) {
        if (c == ' ' || c == '\t') {
            if (!ultimo_esp) limpo += ' ';
            ultimo_esp = true;
        } else {
            limpo += c;
            ultimo_esp = false;
        }
    }
    limpo = cortar(limpo);
    if (limpo.empty()) limpo = "arquivo";

    // Evitar nomes de dispositivos reservados no Windows
    static const std::set<std::string> reservados = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
    };
    std::string check = minusculas(limpo);
    size_t dotPos = check.find('.');
    std::string base = (dotPos == std::string::npos) ? check : check.substr(0, dotPos);
    if (reservados.count(base)) limpo = "_" + limpo;
    if (limpo.size() > tam_max) limpo = limpo.substr(0, tam_max);
    return limpo;
}

std::string obter_nome_arquivo_da_url(const std::string& url) {
    std::string clean = url;
    size_t q = clean.find('?');
    if (q != std::string::npos) clean = clean.substr(0, q);
    size_t h = clean.find('#');
    if (h != std::string::npos) clean = clean.substr(0, h);

    size_t slash = clean.find_last_of("/\\");
    std::string name = (slash != std::string::npos) ? clean.substr(slash + 1) : clean;
    if (name.empty()) name = "arquivo.bin";
    return sanitizar_nome_arquivo(name);
}

bool garantir_pasta_pai(const std::string& caminho_arquivo) {
    std::string caminho = cortar(caminho_arquivo);
    if (caminho.empty()) return false;
    // Caminhos especiais do SQLite (memoria, vazio) nao tem pasta.
    std::string baixo = minusculas(caminho);
    if (baixo == ":memory:" || baixo.rfind("file:", 0) == 0) return true;
    try {
        fs::path p(caminho);
        fs::path pai = p.parent_path();
        if (pai.empty() || pai == "." || pai == "./") return true; // ex: "clima.db" -> pasta atual
        std::error_code ec;
        bool criado = fs::create_directories(pai, ec);
        if (ec) {
            log("[WARNING] Nao foi possivel criar pasta '" + pai.string() +
                "' para o banco '" + caminho + "': " + ec.message());
            return false;
        }
        if (criado) log("[DB] Pasta criada: " + pai.string() + " (para " + caminho + ")");
        return true;
    } catch (const std::exception& e) {
        log(std::string("[WARNING] Nao foi possivel criar pasta para '") + caminho +
            "': " + e.what());
        return false;
    }
}

std::vector<std::string> extrair_urls_de_conteudo(const std::string& conteudo, const std::string& base_url, const std::string& modo) {
    std::vector<std::string> urls;
    std::set<std::string> vistas;
    std::string m = minusculas(modo);

    auto resolver_url = [&](const std::string& rel) -> std::string {
        if (rel.empty()) return "";
        if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) return rel;
        if (rel.rfind("//", 0) == 0) return "https:" + rel;
        if (base_url.empty()) return rel;

        std::string base = base_url;
        size_t query_pos = base.find('?');
        if (query_pos != std::string::npos) base = base.substr(0, query_pos);

        if (rel.front() == '/') {
            size_t proto_end = base.find("://");
            if (proto_end != std::string::npos) {
                size_t host_end = base.find('/', proto_end + 3);
                std::string host = (host_end != std::string::npos) ? base.substr(0, host_end) : base;
                return host + rel;
            }
        }
        size_t last_slash = base.find_last_of('/');
        if (last_slash != std::string::npos) {
            return base.substr(0, last_slash + 1) + rel;
        }
        return base + "/" + rel;
    };

    if (m == "m3u" || m == "m3u8" || m == "playlist") {
        std::istringstream stream(conteudo);
        std::string line;
        while (std::getline(stream, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::string u = resolver_url(line);
            if (!u.empty() && vistas.insert(u).second) urls.push_back(u);
        }
        return urls;
    }

    if (m == "html_imgs" || m == "imgs" || m == "imagens") {
        // 1. Extração de tags <img ... src="...">
        std::regex img_regex(R"(<img[^>]+src=["']([^"']+)["'])", std::regex::icase);
        auto b = std::sregex_iterator(conteudo.begin(), conteudo.end(), img_regex);
        auto e = std::sregex_iterator();
        for (auto it = b; it != e; ++it) {
            std::string src = (*it)[1].str();
            std::string u = resolver_url(src);
            if (!u.empty() && vistas.insert(u).second) urls.push_back(u);
        }

        // 2. Extração de srcset
        std::regex srcset_regex(R"(srcset=["']([^"']+)["'])", std::regex::icase);
        b = std::sregex_iterator(conteudo.begin(), conteudo.end(), srcset_regex);
        for (auto it = b; it != e; ++it) {
            std::string ss = (*it)[1].str();
            for (auto& parte : dividir(ss, ',')) {
                auto pedacos = dividir(cortar(parte), ' ');
                if (!pedacos.empty()) {
                    std::string u = resolver_url(pedacos[0]);
                    if (!u.empty() && vistas.insert(u).second) urls.push_back(u);
                }
            }
        }
        return urls;
    }

    if (m == "html_links" || m == "links" || m == "hrefs") {
        std::regex link_regex(R"(<a[^>]+href=["']([^"']+)["'])", std::regex::icase);
        auto b = std::sregex_iterator(conteudo.begin(), conteudo.end(), link_regex);
        auto e = std::sregex_iterator();
        for (auto it = b; it != e; ++it) {
            std::string href = (*it)[1].str();
            if (href.rfind("javascript:", 0) == 0 || href.rfind("mailto:", 0) == 0 || href == "#") continue;
            std::string u = resolver_url(href);
            if (!u.empty() && vistas.insert(u).second) urls.push_back(u);
        }
        return urls;
    }

    // Modo regex customizado
    try {
        std::regex custom_regex(modo, std::regex::icase);
        auto b = std::sregex_iterator(conteudo.begin(), conteudo.end(), custom_regex);
        auto e = std::sregex_iterator();
        for (auto it = b; it != e; ++it) {
            std::string match = (it->size() > 1) ? (*it)[1].str() : (*it)[0].str();
            std::string u = resolver_url(match);
            if (!u.empty() && vistas.insert(u).second) urls.push_back(u);
        }
    } catch (...) {}

    return urls;
}
