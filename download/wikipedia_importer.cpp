#include "wikipedia_importer.h"
#include "config.h"
#include "file_dataset.h"
#include "http_client.h"
#include "utils.h"
#include <bzlib.h>
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>
namespace fs = std::filesystem;

static std::string trim_copy(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::string para_minusculas(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
static std::string data_do_nome(const std::string& nome) {
    try {
        std::regex re(R"(-(\d{4}-\d{2}-\d{2})-)");
        std::smatch m;
        if (std::regex_search(nome, m, re)) return m[1].str();
    } catch (...) {}
    return "";
}
static std::string projeto_do_nome(const std::string& nome) {
    size_t p = nome.find('-');
    if (p == std::string::npos) return "wiki";
    return nome.substr(0, p);
}
static std::string juntar_url(const std::string& base, const std::string& nome) {
    if (base.empty()) return nome;
    if (base.back() == '/') return base + nome;
    return base + "/" + nome;
}
static std::string extrair_tag(const std::string& bloco, const std::string& nome) {
    std::string abre = "<" + nome;
    size_t p = bloco.find(abre);
    if (p == std::string::npos) return "";
    size_t q = p + abre.size();
    if (q < bloco.size()) {
        char c = bloco[q];
        if (c != '>' && c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '/') return "";
    }
    size_t fim_abre = bloco.find('>', p);
    if (fim_abre == std::string::npos) return "";
    if (fim_abre > p && bloco[fim_abre - 1] == '/') return "";
    std::string fecha = "</" + nome + ">";
    size_t f = bloco.find(fecha, fim_abre + 1);
    if (f == std::string::npos) return "";
    return bloco.substr(fim_abre + 1, f - fim_abre - 1);
}
static std::string html_unescape_basico(std::string s) {
    const char* a[] = {"&amp;","&lt;","&gt;","&quot;","&#39;","&apos;","&nbsp;","&ndash;","&mdash;"};
    const char* b[] = {"&","<",">","\"","'","'"," ","-","-"};
    for (int k = 0; k < 9; ++k) {
        size_t p = 0;
        size_t n = strlen(a[k]);
        while ((p = s.find(a[k], p)) != std::string::npos) { s.replace(p, n, b[k]); p += strlen(b[k]); }
    }
    return s;
}
struct TextoLimpo {
    std::string texto;
    std::vector<std::string> categorias;
    std::vector<std::string> links;
};
static bool eh_redirect(const std::string& texto) {
    size_t i = 0;
    while (i < texto.size() && isspace((unsigned char)texto[i])) i++;
    if (i >= texto.size() || texto[i] != '#') return false;
    std::string ini = para_minusculas(texto.substr(i, 22));
    return ini.rfind("#redirect", 0) == 0 || ini.rfind("#redirecionamento", 0) == 0;
}

static TextoLimpo limpar_wikitexto(const std::string& bruto) {
    TextoLimpo r;
    if (bruto.empty()) return r;
    std::string t = bruto;
    try {
        t = std::regex_replace(t, std::regex(R"(<!--[^-]*-->)"), std::string(" "));
        t = std::regex_replace(t, std::regex(R"(<ref[^>]*/>)", std::regex::icase), std::string(" "));
        t = std::regex_replace(t, std::regex(R"(</?[a-z][^>\n]*>)", std::regex::icase), std::string(" "));
    } catch (...) {}
    try {
        std::regex re_cat(R"(\[\[(?:Category|Categoria):([^\]|]+)(?:\|[^\]]*)?\]\])", std::regex::icase);
        auto ini = std::sregex_iterator(t.begin(), t.end(), re_cat);
        auto fim = std::sregex_iterator();
        for (auto it = ini; it != fim; ++it) {
            std::string c = trim_copy((*it)[1].str());
            if (!c.empty()) r.categorias.push_back(c);
        }
        t = std::regex_replace(t, re_cat, std::string(""));
    } catch (...) {}
    try {
        std::regex re_link(R"(\[\[([^|\]]+)(?:\|[^\]]*)?\]\])");
        auto ini = std::sregex_iterator(t.begin(), t.end(), re_link);
        auto fim = std::sregex_iterator();
        for (auto it = ini; it != fim; ++it) {
            std::string d = trim_copy((*it)[1].str());
            if (d.empty()) continue;
            std::string dl = para_minusculas(d);
            if (dl.rfind("category:", 0) == 0 || dl.rfind("categoria:", 0) == 0) continue;
            if (dl.rfind("file:", 0) == 0 || dl.rfind("image:", 0) == 0) continue;
            if (dl.rfind("imagem:", 0) == 0 || dl.rfind("ficheiro:", 0) == 0) continue;
            r.links.push_back(d);
        }
        t = std::regex_replace(t, std::regex(R"(\[\[[^|\]]*\|([^\]]+)\]\])"), std::string("$1"));
        t = std::regex_replace(t, std::regex(R"(\[\[([^\]]+)\]\])"), std::string("$1"));
    } catch (...) {}
    try {
        t = std::regex_replace(t, std::regex(R"(\[(?:https?:)?//[^\s\]]+\s+([^\]]+)\])"), std::string("$1"));
        t = std::regex_replace(t, std::regex(R"(\[(?:https?:)?//[^\s\]]+\])"), std::string(" "));
    } catch (...) {}
    try {
        std::regex re_tpl(R"(\{\{[^{}]+\}\})");
        for (int k = 0; k < 15; ++k) {
            std::string n = std::regex_replace(t, re_tpl, std::string(""));
            if (n == t) break;
            t = n;
        }
    } catch (...) {}
    size_t p = 0;
    while ((p = t.find("'''''", p)) != std::string::npos) { t.replace(p, 5, "***"); p += 3; }
    p = 0;
    while ((p = t.find("'''", p)) != std::string::npos) { t.replace(p, 3, "**"); p += 2; }
    p = 0;
    while ((p = t.find("''", p)) != std::string::npos) { t.replace(p, 2, "*"); p += 1; }
    t = html_unescape_basico(t);
    std::string out;
    out.reserve(t.size());
    int nl = 0;
    for (char c : t) {
        if (c == '\n') { nl++; if (nl <= 2) out.push_back(c); }
        else { nl = 0; out.push_back(c); }
    }
    t = trim_copy(out);
    if (!r.categorias.empty()) {
        t = trim_copy(t) + "\n\nCategorias: ";
        for (size_t i = 0; i < r.categorias.size(); ++i) {
            if (i) t += ", ";
            t += r.categorias[i];
        }
    }
    r.texto = trim_copy(t);
    return r;
}

static void wsql_exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "erro";
        sqlite3_free(err);
        throw std::runtime_error("SQLite: " + m);
    }
}
struct WikiPagina {
    long long id = 0;
    std::string titulo;
    int ns = 0;
    std::string texto;
    std::vector<std::string> cats;
    std::vector<std::string> links;
};
bool importar_arquivo_wikipedia_bz2(const std::string& caminho_bz2,
        const std::string& db_path, const std::string& projeto_cfg,
        const std::string& data_cfg, const std::string& origem_nome,
        bool apenas_ns0, bool ignorar_redirect, int tamanho_minimo, int batch_size) {
    std::string nome = origem_nome.empty()
        ? fs::path(caminho_bz2).filename().string() : origem_nome;
    std::string projeto = !projeto_cfg.empty() ? projeto_cfg : projeto_do_nome(nome);
    std::string data = !data_cfg.empty() ? data_cfg : data_do_nome(nome);
    if (batch_size < 100) batch_size = 100;
    garantir_pasta_pai(db_path);
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::string m = db ? sqlite3_errmsg(db) : "falha ao abrir";
        if (db) sqlite3_close(db);
        throw std::runtime_error("SQLite: " + db_path + ": " + m);
    }
    wsql_exec(db, "PRAGMA journal_mode=WAL;");
    wsql_exec(db, "PRAGMA synchronous=NORMAL;");
    wsql_exec(db, "PRAGMA busy_timeout=60000;");
    wsql_exec(db, "CREATE TABLE IF NOT EXISTS paginas(page_id INTEGER PRIMARY KEY,"
        "titulo TEXT NOT NULL,namespace INTEGER NOT NULL,projeto TEXT NOT NULL,"
        "data TEXT NOT NULL,origem TEXT NOT NULL,texto TEXT);");
    wsql_exec(db, "CREATE TABLE IF NOT EXISTS progresso(arquivo TEXT PRIMARY KEY,"
        "completo INTEGER NOT NULL DEFAULT 0,"
        "ultimo_page_id INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID;");
    wsql_exec(db, "CREATE TABLE IF NOT EXISTS categorias(page_id INTEGER NOT NULL,"
        "categoria TEXT NOT NULL,PRIMARY KEY(page_id,categoria)) WITHOUT ROWID;");
    wsql_exec(db, "CREATE TABLE IF NOT EXISTS links(page_id INTEGER NOT NULL,"
        "destino TEXT NOT NULL,PRIMARY KEY(page_id,destino)) WITHOUT ROWID;");

    std::string ins_prog = "INSERT OR IGNORE INTO progresso(arquivo,completo,ultimo_page_id)"
        " VALUES('" + nome + "',0,0);";
    wsql_exec(db, ins_prog.c_str());
    long long skip_ate = 0;
    int completo = 0;
    sqlite3_stmt* stq = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT completo,ultimo_page_id FROM progresso WHERE arquivo=?",
            -1, &stq, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stq, 1, nome.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stq) == SQLITE_ROW) {
            completo = sqlite3_column_int(stq, 0);
            skip_ate = sqlite3_column_int64(stq, 1);
        }
    }
    sqlite3_finalize(stq);
    if (completo == 1) {
        log("[WIKI] Pulando (completo): " + nome);
        sqlite3_close(db);
        return true;
    }
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(fs::path(caminho_bz2).wstring().c_str(), L"rb");
#else
    f = fopen(caminho_bz2.c_str(), "rb");
#endif
    if (!f) {
        log("[WIKI][ERRO] Nao foi possivel abrir: " + caminho_bz2);
        sqlite3_close(db);
        return false;
    }
    int bzerr = BZ_OK;
    BZFILE* bz = BZ2_bzReadOpen(&bzerr, f, 0, 0, nullptr, 0);
    if (!bz || bzerr != BZ_OK) {
        log("[WIKI][ERRO] BZ2_bzReadOpen falhou: " + caminho_bz2);
        fclose(f);
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt* st_pag = nullptr;
    sqlite3_stmt* st_cat = nullptr;
    sqlite3_stmt* st_lnk = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO paginas"
        "(page_id,titulo,namespace,projeto,data,origem,texto) VALUES(?,?,?,?,?,?,?)",
        -1, &st_pag, nullptr);
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO categorias(page_id,categoria) VALUES(?,?)",
        -1, &st_cat, nullptr);
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO links(page_id,destino) VALUES(?,?)",
        -1, &st_lnk, nullptr);
    std::vector<WikiPagina> lote;
    lote.reserve((size_t)batch_size);
    long long checkpoint = skip_ate;
    long long inseridas = 0, lidas = 0, puladas = 0;
    bool erro_bz = false;
    std::vector<char> buf(1 << 20);
    std::string carry;
    carry.reserve(8 << 20);
    log("[WIKI] Importando " + nome + " (resume page_id > " + std::to_string(skip_ate) + ") ...");


    auto flush = [&]() {
        if (lote.empty()) return;
        wsql_exec(db, "BEGIN IMMEDIATE;");
        for (auto& pg : lote) {
            sqlite3_bind_int64(st_pag, 1, pg.id);
            sqlite3_bind_text(st_pag, 2, pg.titulo.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st_pag, 3, pg.ns);
            sqlite3_bind_text(st_pag, 4, projeto.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st_pag, 5, data.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st_pag, 6, nome.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st_pag, 7, pg.texto.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st_pag);
            sqlite3_reset(st_pag);
            sqlite3_clear_bindings(st_pag);
            for (auto& c : pg.cats) {
                sqlite3_bind_int64(st_cat, 1, pg.id);
                sqlite3_bind_text(st_cat, 2, c.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st_cat);
                sqlite3_reset(st_cat);
                sqlite3_clear_bindings(st_cat);
            }
            for (auto& l : pg.links) {
                sqlite3_bind_int64(st_lnk, 1, pg.id);
                sqlite3_bind_text(st_lnk, 2, l.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st_lnk);
                sqlite3_reset(st_lnk);
                sqlite3_clear_bindings(st_lnk);
            }
        }
        sqlite3_stmt* up = nullptr;
        sqlite3_prepare_v2(db, "UPDATE progresso SET ultimo_page_id=MAX(ultimo_page_id,?)"
            " WHERE arquivo=?", -1, &up, nullptr);
        sqlite3_bind_int64(up, 1, checkpoint);
        sqlite3_bind_text(up, 2, nome.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(up);
        sqlite3_finalize(up);
        wsql_exec(db, "COMMIT;");
        lote.clear();
    };

    while (true) {
        int n = BZ2_bzRead(&bzerr, bz, buf.data(), (int)buf.size());
        if (n > 0) carry.append(buf.data(), (size_t)n);
        while (true) {
            size_t ini = carry.find("<page");
            if (ini == std::string::npos) {
                if (carry.size() > 64) {
                    size_t lt = carry.rfind('<');
                    if (lt != std::string::npos && carry.size() - lt <= 64) carry = carry.substr(lt);
                    else carry = carry.substr(carry.size() - 8);
                }
                break;
            }
            size_t fa = carry.find('>', ini);
            if (fa == std::string::npos) { carry = carry.substr(ini); break; }
            size_t fim = carry.find("</page>", fa);
            if (fim == std::string::npos) {
                if (ini > 0) carry = carry.substr(ini);
                break;
            }
            std::string bloco = carry.substr(ini, fim + 7 - ini);
            carry.erase(0, fim + 7);
            lidas++;
            std::string titulo = html_unescape_basico(trim_copy(extrair_tag(bloco, "title")));
            std::string ns_s = trim_copy(extrair_tag(bloco, "ns"));
            std::string id_s = trim_copy(extrair_tag(bloco, "id"));
            std::string traw = extrair_tag(bloco, "text");
            int ns = 0;
            long long pid = 0;
            try { ns = ns_s.empty() ? 0 : std::stoi(ns_s); } catch (...) { ns = 0; }
            try { pid = id_s.empty() ? 0 : std::stoll(id_s); } catch (...) { pid = 0; }
            if (pid == 0) { puladas++; continue; }
            if (pid <= skip_ate) { puladas++; continue; }
            if (apenas_ns0 && ns != 0) { puladas++; continue; }
            if (ignorar_redirect && eh_redirect(traw)) {
                puladas++;
                if (pid > checkpoint) checkpoint = pid;
                continue;
            }
            TextoLimpo lp = limpar_wikitexto(traw);
            if ((int)lp.texto.size() < tamanho_minimo) {
                puladas++;
                if (pid > checkpoint) checkpoint = pid;
                continue;
            }
            WikiPagina pg;
            pg.id = pid;
            pg.titulo = titulo;
            pg.ns = ns;
            pg.texto = lp.texto;
            pg.cats = lp.categorias;
            pg.links = lp.links;
            lote.push_back(pg);
            if (pid > checkpoint) checkpoint = pid;
            inseridas++;
            if ((long long)lote.size() >= batch_size) {
                flush();
                log("[WIKI] " + nome + ": ckpt " + std::to_string(checkpoint)
                    + " | +" + std::to_string(inseridas));
            }
        }
        if (bzerr == BZ_STREAM_END) break;
        if (bzerr != BZ_OK && n <= 0) {
            log("[WIKI][ERRO] Dump truncado (" + nome + "). Parcial...");
            erro_bz = true;
            break;
        }
        if (n <= 0 && bzerr == BZ_OK) break;
    }
    flush();
    BZ2_bzReadClose(&bzerr, bz);
    fclose(f);
    if (!erro_bz) {
        sqlite3_stmt* up = nullptr;
        sqlite3_prepare_v2(db, "UPDATE progresso SET completo=1 WHERE arquivo=?", -1, &up, nullptr);
        sqlite3_bind_text(up, 1, nome.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(up);
        sqlite3_finalize(up);
        wsql_exec(db, "CREATE INDEX IF NOT EXISTS idx_categorias_nome ON categorias(categoria);");
        wsql_exec(db, "CREATE INDEX IF NOT EXISTS idx_links_destino ON links(destino);");
    }
    sqlite3_finalize(st_pag);
    sqlite3_finalize(st_cat);
    sqlite3_finalize(st_lnk);
    sqlite3_close(db);
    log("[WIKI] " + nome + (erro_bz ? " PARCIAL" : " OK") + ": lidas=" + std::to_string(lidas)
        + " ins=" + std::to_string(inseridas) + " skip=" + std::to_string(puladas));
    return !erro_bz;
}
static std::vector<std::string> listar_xml_bz2_remoto(const std::string& base_url) {
    std::vector<std::string> out;
    RespostaHttp r = http_requisicao(base_url, "GET", {}, "", "", "");
    if (r.erro_rede || r.status < 200 || r.status >= 300 || r.corpo.empty()) {
        log("[WIKI][ERRO] Listagem falhou " + base_url + " http=" + std::to_string(r.status));
        return out;
    }
    try {
        std::regex re_href("href\\s*=\\s*\"([^\"]+\\.xml\\.bz2)\"", std::regex::icase);
        auto ini = std::sregex_iterator(r.corpo.begin(), r.corpo.end(), re_href);
        auto fim = std::sregex_iterator();
        std::set<std::string> vistos;
        for (auto it = ini; it != fim; ++it) {
            std::string h = (*it)[1].str();
            size_t p = h.find_last_of('/');
            std::string n2 = (p == std::string::npos) ? h : h.substr(p + 1);
            if (!n2.empty() && vistos.insert(n2).second) out.push_back(n2);
        }
    } catch (...) {}
    std::sort(out.begin(), out.end());
    return out;
}
void executar_dataset_wikipedia(const DatasetCfg& ds) {
    std::string base_url = cortar(ds.url);
    std::string pasta = resolver_pasta_saida(ds.pasta_saida, "wikipedia_dump", "WIKI_PASTA");
    if (pasta.empty()) pasta = "wikipedia_dump";
    std::error_code ec;
    fs::create_directories(fs::path(pasta), ec);
    std::string db = ds.db.empty() ? CFG.db_path : ds.db;
    log("============================================================");
    log("WIKIPEDIA DUMPS -> SQLITE: " + ds.id);
    log(std::string("Origem: ") + (base_url.empty() ? "(somente local)" : base_url));
    log("Pasta: " + pasta);
    log("Banco: " + db);
    log("============================================================");
    std::vector<std::string> arquivos;
    if (ds.listar_remoto && !base_url.empty()) {
        arquivos = listar_xml_bz2_remoto(base_url);
        log("[WIKI] " + std::to_string(arquivos.size()) + " .xml.bz2 listados.");
    }
    if (arquivos.empty()) {
        for (auto& e : fs::directory_iterator(fs::path(pasta), ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            std::string n2 = e.path().filename().string();
            std::string l = para_minusculas(n2);
            if (l.size() >= 8 && l.compare(l.size() - 8, 8, ".xml.bz2") == 0) arquivos.push_back(n2);
        }
        std::sort(arquivos.begin(), arquivos.end());
        if (!arquivos.empty()) log("[WIKI] " + std::to_string(arquivos.size()) + " dumps locais.");
    }
    if (arquivos.empty()) {
        log("[WIKI][ERRO] Nenhum .xml.bz2 (pasta vazia / lista remota falhou).");
        return;
    }
    if (!base_url.empty()) {
        // Fase 1 (I/O de rede): downloads em paralelo com download_workers.
        int n_dl = CFG.download_workers < 1 ? 1 : CFG.download_workers;
        if ((size_t)n_dl > arquivos.size()) n_dl = (int)arquivos.size();
        log("[WIKI] download_workers=" + std::to_string(n_dl) +
            " process_workers=" + std::to_string(CFG.process_workers) +
            " p/ " + std::to_string(arquivos.size()) + " dumps");
        {
            std::atomic<size_t> idx{0};
            std::vector<std::thread> th;
            th.reserve((size_t)std::max(1, n_dl));
            for (int i = 0; i < n_dl; ++i) {
                th.emplace_back([&] {
                    while (true) {
                        size_t k = idx.fetch_add(1);
                        if (k >= arquivos.size()) break;
                        const std::string n2 = arquivos[k];
                        fs::path dest = fs::path(pasta) / n2;
                        std::error_code ec2;
                        if (fs::exists(dest, ec2) && fs::file_size(dest, ec2) > 0) {
                            if (ds.tamanho_max_bytes > 0) {
                                auto sz = fs::file_size(dest, ec2);
                                if (!ec2 && sz > static_cast<unsigned long long>(ds.tamanho_max_bytes)) {
                                    log("[WIKI][SKIP] " + n2 + " (" + std::to_string(sz) +
                                        " bytes) excede tamanho_max");
                                    continue;
                                }
                            }
                            log("[WIKI] OK (local): " + n2);
                            continue;
                        }
                        log("[WIKI] Baixando: " + n2);
                        bool pulado_max = false;
                        bool ok = baixar_arquivo_seguro(juntar_url(base_url, n2), dest.string(),
                            "", ds.tamanho_min_bytes, ds.headers, CFG.http_timeout,
                            ds.tamanho_max_bytes, &pulado_max);
                        if (!ok && !pulado_max) log("[WIKI][ERRO] Falha no download: " + n2);
                    }
                });
            }
            for (auto& t : th) t.join();
        }
    }
    // Fase 2 (CPU + SQLite): importações em paralelo com process_workers.
    // Cada arquivo tem seu próprio .db transaction + progresso (resume por
    // arquivo), e o SQLite abre 1 conexão por thread (thread_local em
    // get_connection_para / abrir_banco_wiki). busy_timeout + WAL evitam
    // SQLITE_BUSY entre workers.
    int n_proc = CFG.process_workers < 1 ? 1 : CFG.process_workers;
    if ((size_t)n_proc > arquivos.size() && !arquivos.empty())
        n_proc = (int)arquivos.size();
    std::atomic<size_t> ok_n{0}, fail_n{0};
    log("[WIKI] Importando " + std::to_string(arquivos.size()) +
        " dumps com process_workers=" + std::to_string(n_proc));
    {
        std::atomic<size_t> idx{0};
        std::vector<std::thread> th;
        th.reserve((size_t)std::max(1, n_proc));
        for (int i = 0; i < n_proc; ++i) {
            th.emplace_back([&] {
                while (true) {
                    size_t k = idx.fetch_add(1);
                    if (k >= arquivos.size()) break;
                    const std::string n2 = arquivos[k];
                    fs::path p = fs::path(pasta) / n2;
                    std::error_code ec2;
                    if (!fs::exists(p, ec2)) { fail_n.fetch_add(1); continue; }
                    try {
                        bool ok = importar_arquivo_wikipedia_bz2(p.string(), db, ds.projeto, ds.data_dump,
                            n2, ds.apenas_ns0, ds.ignorar_redirect, ds.tamanho_minimo, ds.wiki_batch);
                        if (ok) ok_n.fetch_add(1); else fail_n.fetch_add(1);
                    } catch (const std::exception& e) {
                        log(std::string("[WIKI][ERRO] ") + n2 + ": " + e.what());
                        fail_n.fetch_add(1);
                    }
                }
            });
        }
        for (auto& t : th) t.join();
    }
    log("[WIKI] Concluido: " + std::to_string(ok_n.load()) + " OK, "
        + std::to_string(fail_n.load()) + " falhas. Banco: " + db);
}
