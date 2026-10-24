#include "video_downloader.h"
#include "http_client.h"
#include "rate_limit.h"
#include "utils.h"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>
#ifdef _WIN32
    #include <windows.h>
    #define POPEN_FN _popen
    #define PCLOSE_FN _pclose
#else
    #define POPEN_FN popen
    #define PCLOSE_FN pclose
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
    // Conteúdo embutido do extrator JavaScript (usado caso o arquivo não exista no disco)
    const char* const SCRIPT_EXTRATOR_EMBUTIDO = R"JS(const { createRequire } = require('module');
    const fs = require('fs');
    const os = require('os');
    const path = require('path');

    function resolvePatchright() {
    const roots = [];
    const home = os.homedir();
    for (const base of [
        path.join(home, '.vscode-server', 'extensions'),
        path.join(home, '.vscode', 'extensions')
    ]) {
        if (!fs.existsSync(base)) continue;
        const dirs = fs.readdirSync(base)
        .filter(d => d.startsWith('danielsanmedium.dscodegpt-'))
        .sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' }));
        const newest = dirs[dirs.length - 1];
        if (newest) roots.push(path.join(base, newest, 'standalone') + path.sep);
    }
    roots.push(process.cwd() + path.sep);

    for (const root of roots) {
        try {
        const mod = createRequire(root)('patchright');
        const chromium = mod?.chromium ?? mod?.default?.chromium;
        if (chromium) return chromium;
        } catch (e) {}
    }
    try {
        const pw = require('playwright');
        if (pw?.chromium) return pw.chromium;
    } catch (e) {}

    return null;
    }

    async function extractVideo(targetUrl) {
    const chromium = resolvePatchright();
    if (!chromium) {
        return { ok: false, error: 'Neither patchright nor playwright chromium could be resolved.' };
    }

    let browser;
    try {
        browser = await chromium.launch({
        headless: true,
        channel: 'chromium',
        args: ['--no-sandbox', '--disable-dev-shm-usage', '--enable-unsafe-swiftshader']
        });
    } catch (err) {
        try {
        browser = await chromium.launch({
            headless: true,
            args: ['--no-sandbox', '--disable-dev-shm-usage', '--enable-unsafe-swiftshader']
        });
        } catch (err2) {
        return { ok: false, error: 'Failed to launch browser: ' + err2.message };
        }
    }

    try {
        const ctx = await browser.newContext({
        locale: 'en-US',
        userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36'
        });
        const page = await ctx.newPage();

        let capturedM3u8 = null;
        let capturedReferer = null;
        let capturedOrigin = null;

        page.on('request', req => {
        const u = req.url();
        if ((u.includes('.m3u8') || u.includes('/master') || u.includes('/playlist')) && !capturedM3u8) {
            if (!u.includes('ping') && !u.includes('analytics') && !u.includes('beacon')) {
            capturedM3u8 = u;
            const headers = req.headers();
            capturedReferer = headers['referer'] || '';
            capturedOrigin = headers['origin'] || '';
            }
        }
        });

        await page.goto(targetUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });

        let pageTitle = '';
        try {
        pageTitle = await page.title();
        } catch (e) {}

        let animeTitle = '';
        try {
        animeTitle = await page.evaluate(() => {
            const og = document.querySelector('meta[property="og:title"]');
            if (og && og.content) return og.content;
            const breadcrumbLinks = Array.from(document.querySelectorAll('a[href*="/watch/"], nav a, ol li a'));
            for (const a of breadcrumbLinks) {
            const t = a.innerText.trim();
            if (t && t !== 'Home' && t !== 'TV' && t !== 'Movie' && t !== 'Enma' && !t.includes('Episode')) {
                return t;
            }
            }
            return '';
        });
        } catch (e) {}

        let cleanTitle = animeTitle;
        try {
        const parsed = new URL(targetUrl);
        const ep = parsed.searchParams.get('ep');
        const segments = parsed.pathname.split('/').filter(Boolean);
        let slug = segments[segments.length - 1] || 'video';
        slug = slug.replace(/-\d+$/, '').replace(/-/g, ' ');

        if (!cleanTitle || cleanTitle.toLowerCase().includes('enma') || cleanTitle.toLowerCase().includes('best hianime')) {
            cleanTitle = slug;
        } else if (cleanTitle.toLowerCase().includes('watch ') && cleanTitle.includes(' Episode')) {
            const m = cleanTitle.match(/Watch\s+(.*?)\s+Episode/i);
            if (m && m[1]) cleanTitle = m[1];
        }
        if (ep) {
            cleanTitle = cleanTitle + ' - Ep ' + ep;
        }
        } catch (e) {}

        for (let i = 0; i < 30; i++) {
        if (capturedM3u8) break;
        await page.waitForTimeout(500);
        }

        if (!capturedM3u8) {
        try {
            const vSrc = await page.evaluate(() => {
            const v = document.querySelector('video');
            if (v) return v.src || (v.querySelector('source') ? v.querySelector('source').src : '');
            return '';
            });
            if (vSrc) {
            capturedM3u8 = vSrc;
            }
        } catch (e) {}
        }

        await browser.close();

        if (!capturedM3u8) {
        return { ok: false, error: 'No video stream captured within timeout.' };
        }

        return {
        ok: true,
        url: capturedM3u8,
        title: cleanTitle || animeTitle || pageTitle || 'video',
        referer: capturedReferer || targetUrl,
        origin: capturedOrigin || ''
        };
    } catch (err) {
        if (browser) await browser.close().catch(() => {});
        return { ok: false, error: err.message };
    }
    }

    const targetUrl = process.argv[2];
    if (!targetUrl) {
    console.log(JSON.stringify({ ok: false, error: 'No URL provided' }));
    process.exit(1);
    }

    extractVideo(targetUrl).then(res => {
    console.log(JSON.stringify(res));
    process.exit(res.ok ? 0 : 1);
    }).catch(err => {
    console.log(JSON.stringify({ ok: false, error: err.message }));
    process.exit(1);
    });
    )JS";

    // Encontra executável no sistema (PATH e locais conhecidos)
    std::string encontrar_executavel(const std::string& nome, const std::vector<std::string>& caminhos_extras = {}) {
        std::error_code ec;
        for (const auto& c : caminhos_extras) {
            if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
        }
        #ifdef _WIN32
            std::string cmd = "where.exe " + nome + " 2>nul";
        #else
            std::string cmd = "which " + nome + " 2>/dev/null";
        #endif
        FILE* pipe = POPEN_FN(cmd.c_str(), "r");
        if (!pipe) return "";
        char buffer[512];
        std::string resultado;
        if (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) resultado = buffer;
        PCLOSE_FN(pipe);
        while (!resultado.empty() && (resultado.back() == '\r' || resultado.back() == '\n' || resultado.back() == ' '))
            resultado.pop_back();
        return resultado;
    }

    std::string localizar_node() {
        return encontrar_executavel("node", {
            "C:\\Program Files\\nodejs\\node.exe",
            "C:\\Program Files (x86)\\nodejs\\node.exe",
            "D:\\nodejs\\node.exe"
        });
    }

    std::string localizar_ffmpeg() {
        return encontrar_executavel("ffmpeg", {
            "D:\\msys64\\mingw64\\bin\\ffmpeg.exe",
            "C:\\msys64\\mingw64\\bin\\ffmpeg.exe",
            "C:\\ffmpeg\\bin\\ffmpeg.exe",
            "D:\\ffmpeg\\bin\\ffmpeg.exe"
        });
    }

    // Garante existência de download/video_extractor.cjs
    std::string obter_caminho_extrator() {
        std::error_code ec;
        std::vector<std::string> candidatos = {
            "download/video_extractor.cjs",
            "video_extractor.cjs",
            "../download/video_extractor.cjs"
        };
        for (const auto& c : candidatos) {
            if (fs::exists(c, ec)) return c;
        }

        std::string destino = "download/video_extractor.cjs";
        try {
            fs::create_directories("download", ec);
            std::ofstream out(destino);
            if (out) {
                out << SCRIPT_EXTRATOR_EMBUTIDO;
                return destino;
            }
        } catch (...) {}

        // Fallback: salva na raiz
        destino = "video_extractor.cjs";
        std::ofstream out(destino);
        if (out) {
            out << SCRIPT_EXTRATOR_EMBUTIDO;
            return destino;
        }
        return "";
    }

    // Resolve URLs relativas
    std::string resolver_url_hls(const std::string& base_url, const std::string& rel) {
        if (rel.empty()) return "";
        if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) return rel;
        if (rel.rfind("//", 0) == 0) return "https:" + rel;
        if (base_url.empty()) return rel;

        std::string base = base_url;
        std::string query_base;
        size_t qpos = base.find('?');
        if (qpos != std::string::npos) {
            query_base = base.substr(qpos);
            base = base.substr(0, qpos);
        }

        std::string resultado;
        if (rel.front() == '/') {
            size_t proto_end = base.find("://");
            if (proto_end != std::string::npos) {
                size_t host_end = base.find('/', proto_end + 3);
                std::string host = (host_end != std::string::npos) ? base.substr(0, host_end) : base;
                resultado = host + rel;
            } else {
                resultado = rel;
            }
        } else {
            size_t last_slash = base.find_last_of('/');
            if (last_slash != std::string::npos) {
                resultado = base.substr(0, last_slash + 1) + rel;
            } else {
                resultado = base + "/" + rel;
            }
        }

        // Se o recurso relativo não tem query, mas a base tinha parâmetros (ex: tokens de CDN), preserva
        if (!query_base.empty() && resultado.find('?') == std::string::npos && rel.find('?') == std::string::npos) {
            resultado += query_base;
        }
        return resultado;
    }

    // Localiza o início dos dados reais de mídia (removendo máscaras PNG/JPEG de CDNs)
    size_t encontrar_inicio_midia(const unsigned char* dados, size_t tamanho) {
        if (tamanho < 16) return 0;
        // MPEG-TS direto
        if (dados[0] == 0x47) return 0;

        // Máscara PNG: \x89PNG\r\n\x1a\n
        if (dados[0] == 0x89 && dados[1] == 'P' && dados[2] == 'N' && dados[3] == 'G' &&
            dados[4] == 0x0D && dados[5] == 0x0A && dados[6] == 0x1A && dados[7] == 0x0A) {
            size_t limite = std::min(tamanho, static_cast<size_t>(4096));
            for (size_t i = 8; i + 8 <= limite; ++i) {
                if (dados[i] == 'I' && dados[i + 1] == 'E' && dados[i + 2] == 'N' && dados[i + 3] == 'D') {
                    size_t pos_depois_iend = i + 8; // 'IEND' (4 bytes) + CRC (4 bytes)
                    if (pos_depois_iend < tamanho) return pos_depois_iend;
                }
            }
        }

        // Máscara JPEG: \xFF\xD8\xFF ... \xFF\xD9
        if (dados[0] == 0xFF && dados[1] == 0xD8 && dados[2] == 0xFF) {
            size_t limite = std::min(tamanho, static_cast<size_t>(4096));
            for (size_t i = 2; i + 2 <= limite; ++i) {
                if (dados[i] == 0xFF && dados[i + 1] == 0xD9) return i + 2;
            }
        }

        // Procura sync byte 0x47 com stride de 188 bytes para MPEG-TS
        size_t limite = std::min(tamanho, static_cast<size_t>(4096));
        for (size_t i = 0; i + 188 < tamanho && i < limite; ++i) {
            if (dados[i] == 0x47 && (i + 188 >= tamanho || dados[i + 188] == 0x47)) return i;
        }

        return 0;
    }

    // Descriptografia AES-128-CBC para HLS com EXT-X-KEY
    bool descriptografar_aes128(const std::vector<uint8_t>& cifrado,
                                const uint8_t key[16],
                                const uint8_t iv[16],
                                std::vector<uint8_t>& decifrado) {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;

        if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        decifrado.resize(cifrado.size() + 16);
        int len = 0;
        int plain_len = 0;

        if (EVP_DecryptUpdate(ctx, decifrado.data(), &len, cifrado.data(), static_cast<int>(cifrado.size())) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }
        plain_len = len;

        if (EVP_DecryptFinal_ex(ctx, decifrado.data() + len, &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            decifrado = cifrado;
            return true;
        }
        plain_len += len;
        decifrado.resize(plain_len);
        EVP_CIPHER_CTX_free(ctx);
        return true;
    }

    // Estrutura de variante HLS no master playlist
    struct VarianteHLS {
        std::string uri;
        long long bandwidth = 0;
        int largura = 0;
        int altura = 0;
        std::string resolucao;
    };

    // Estrutura de segmento HLS
    struct SegmentoHLS {
        std::string url;
        double duracao = 0.0;
        int seq = 0;
        std::string key_uri;
        std::vector<uint8_t> iv;
    };

    struct InfoStreamHLS {
        std::vector<SegmentoHLS> segmentos;
        std::string playlist_url;
    };

    // Analisa playlist Master e seleciona a melhor variante
    std::string selecionar_melhor_variante(const std::string& master_conteudo,
                                        const std::string& master_url,
                                        const std::string& qualidade_preferida) {
        std::istringstream stream(master_conteudo);
        std::string linha;
        std::vector<VarianteHLS> variantes;
        VarianteHLS atual;
        bool esperando_uri = false;

        while (std::getline(stream, linha)) {
            while (!linha.empty() && (linha.back() == '\r' || linha.back() == '\n' || linha.back() == ' '))
                linha.pop_back();
            if (linha.empty()) continue;

            if (linha.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
                atual = VarianteHLS();
                esperando_uri = true;

                // Extrai BANDWIDTH
                size_t pos_bw = linha.find("BANDWIDTH=");
                if (pos_bw != std::string::npos) {
                    atual.bandwidth = std::strtoll(linha.c_str() + pos_bw + 10, nullptr, 10);
                }

                // Extrai RESOLUTION
                size_t pos_res = linha.find("RESOLUTION=");
                if (pos_res != std::string::npos) {
                    std::string res_str = linha.substr(pos_res + 11);
                    size_t virgula = res_str.find(',');
                    if (virgula != std::string::npos) res_str = res_str.substr(0, virgula);
                    atual.resolucao = res_str;
                    size_t xpos = res_str.find('x');
                    if (xpos != std::string::npos) {
                        atual.largura = std::atoi(res_str.substr(0, xpos).c_str());
                        atual.altura = std::atoi(res_str.substr(xpos + 1).c_str());
                    }
                }
            } else if (esperando_uri && linha[0] != '#') {
                atual.uri = resolver_url_hls(master_url, linha);
                variantes.push_back(atual);
                esperando_uri = false;
            }
        }

        if (variantes.empty()) return master_url;

        // Ordena da maior resolução/bandwidth para a menor
        std::sort(variantes.begin(), variantes.end(), [](const VarianteHLS& a, const VarianteHLS& b) {
            if (a.altura != b.altura) return a.altura > b.altura;
            return a.bandwidth > b.bandwidth;
        });

        if (!qualidade_preferida.empty() && qualidade_preferida != "melhor") {
            for (const auto& v : variantes) {
                if (v.resolucao.find(qualidade_preferida) != std::string::npos ||
                    std::to_string(v.altura) + "p" == qualidade_preferida) {
                    return v.uri;
                }
            }
        }

        const auto& escolhida = variantes.front();
        if (escolhida.altura > 0) {
            std::cout << "[STREAM] Qualidade selecionada: " << escolhida.altura << "p ("
                    << escolhida.resolucao << ", " << (escolhida.bandwidth / 1000) << " kbps)\n";
        }
        return escolhida.uri;
    }

    // Analisa playlist de mídia HLS
    bool analisar_media_playlist(const std::string& conteudo,
                                const std::string& playlist_url,
                                InfoStreamHLS& info) {
        info.playlist_url = playlist_url;
        std::istringstream stream(conteudo);
        std::string linha;
        double duracao_atual = 0.0;
        std::string key_uri_atual;
        std::vector<uint8_t> iv_atual;
        int seq = 0;

        while (std::getline(stream, linha)) {
            while (!linha.empty() && (linha.back() == '\r' || linha.back() == '\n' || linha.back() == ' '))
                linha.pop_back();
            if (linha.empty()) continue;

            if (linha.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
                seq = std::atoi(linha.substr(22).c_str());
            } else if (linha.rfind("#EXT-X-KEY:", 0) == 0) {
                size_t pos_uri = linha.find("URI=\"");
                if (pos_uri != std::string::npos) {
                    size_t fim_uri = linha.find('"', pos_uri + 5);
                    if (fim_uri != std::string::npos) {
                        key_uri_atual = resolver_url_hls(playlist_url, linha.substr(pos_uri + 5, fim_uri - (pos_uri + 5)));
                    }
                }
                size_t pos_iv = linha.find("IV=0x");
                if (pos_iv != std::string::npos) {
                    std::string iv_hex = linha.substr(pos_iv + 5, 32);
                    iv_atual.clear();
                    for (size_t i = 0; i + 1 < iv_hex.size(); i += 2) {
                        unsigned int byte_val = 0;
                        std::sscanf(iv_hex.substr(i, 2).c_str(), "%x", &byte_val);
                        iv_atual.push_back(static_cast<uint8_t>(byte_val));
                    }
                } else {
                    iv_atual.clear();
                }
            } else if (linha.rfind("#EXTINF:", 0) == 0) {
                duracao_atual = std::atof(linha.substr(8).c_str());
            } else if (linha[0] != '#') {
                SegmentoHLS seg;
                seg.url = resolver_url_hls(playlist_url, linha);
                seg.duracao = duracao_atual;
                seg.seq = seq++;
                seg.key_uri = key_uri_atual;
                seg.iv = iv_atual;
                info.segmentos.push_back(seg);
                duracao_atual = 0.0;
            }
        }

        return !info.segmentos.empty();
    }
}

bool eh_midia_direta(const std::string& url) {
    std::string u_low = minusculas(url);
    size_t q = u_low.find('?');
    if (q != std::string::npos) u_low = u_low.substr(0, q);
    return (u_low.rfind(".mp4") == u_low.size() - 4 ||
            u_low.rfind(".mkv") == u_low.size() - 4 ||
            u_low.rfind(".webm") == u_low.size() - 5 ||
            u_low.rfind(".ts") == u_low.size() - 3 ||
            u_low.rfind(".m3u8") == u_low.size() - 5 ||
            u_low.rfind(".m3u") == u_low.size() - 4 ||
            u_low.rfind(".mov") == u_low.size() - 4 ||
            u_low.rfind(".avi") == u_low.size() - 4);
}

bool baixar_video(const OpcoesVideo& op) {
    if (op.url.empty()) {
        std::cerr << "[ERRO] URL de vídeo vazia informada.\n";
        return false;
    }

    std::string pasta_saida_str = resolver_pasta_saida(op.pasta_saida, "videos", "PASTA_SAIDA");
    fs::path pasta_saida(pasta_saida_str);
    std::error_code ec;
    fs::create_directories(pasta_saida, ec);

    std::cout << "\n============================================================\n";
    std::cout << "[VIDEO DOWNLOADER] Processando link: " << op.url << "\n";
    std::cout << "Diretório de destino: " << pasta_saida.string() << "\n";
    std::cout << "============================================================\n";

    std::string stream_url = op.url;
    std::string titulo_video = op.nome_arquivo;
    std::map<std::string, std::string> headers_stream = op.headers;

    // Se não for link direto de mídia (.m3u8, .mp4, etc.), usa o extrator headless / web
    if (!eh_midia_direta(op.url)) {
        std::string node_exe = localizar_node();
        if (node_exe.empty()) {
            std::cerr << "[ERRO] Node.js não foi encontrado no sistema (necessário para extrair links dinâmicos como enma.lol).\n";
            return false;
        }

        std::string extrator = obter_caminho_extrator();
        if (extrator.empty()) {
            std::cerr << "[ERRO] Não foi possível preparar o script extrator.\n";
            return false;
        }

        std::cout << "[EXTRATOR] Resolvendo stream de vídeo via navegador headless...\n";
        std::string cmd = "\"\"" + node_exe + "\" \"" + extrator + "\" \"" + op.url + "\"\"";
        FILE* pipe = POPEN_FN(cmd.c_str(), "r");
        if (!pipe) {
            std::cerr << "[ERRO] Falha ao iniciar processo do extrator.\n";
            return false;
        }

        char buffer[1024];
        std::string saida_json;
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            saida_json += buffer;
        }
        int status_pipe = PCLOSE_FN(pipe);

        json j;
        try {
            j = json::parse(saida_json);
        } catch (...) {
            std::cerr << "[ERRO] Resposta inválida do extrator: " << saida_json << "\n";
            return false;
        }

        if (status_pipe != 0 || !j.value("ok", false)) {
            std::string err_msg = j.value("error", "Falha desconhecida na extração do vídeo");
            std::cerr << "[ERRO] " << err_msg << "\n";
            return false;
        }

        stream_url = j.value("url", "");
        if (titulo_video.empty()) {
            titulo_video = j.value("title", "");
        }

        std::string referer = j.value("referer", "");
        if (!referer.empty()) headers_stream["Referer"] = referer;
        std::string origin = j.value("origin", "");
        if (!origin.empty()) headers_stream["Origin"] = origin;

        std::cout << "[OK] Stream capturado com sucesso!\n";
        std::cout << "     Título: " << (titulo_video.empty() ? "(padrão)" : titulo_video) << "\n";
    }

    // Se o link for um arquivo direto de mídia (ex: .mp4, .mkv)
    std::string stream_low = minusculas(stream_url);
    size_t qpos = stream_low.find('?');
    if (qpos != std::string::npos) stream_low = stream_low.substr(0, qpos);

    if (stream_low.rfind(".m3u8") == std::string::npos && stream_low.rfind(".m3u") == std::string::npos) {
        // Download de arquivo de mídia único
        std::string nome_final = titulo_video;
        if (nome_final.empty()) nome_final = obter_nome_arquivo_da_url(stream_url);
        if (nome_final.find('.') == std::string::npos) nome_final += ".mp4";
        nome_final = sanitizar_nome_arquivo(nome_final);

        fs::path dest = pasta_saida / nome_final;
        OpcoesDownload dl_op;
        dl_op.url = stream_url;
        dl_op.caminho = dest.string();
        dl_op.tag = nome_final;
        dl_op.headers = headers_stream;
        dl_op.progresso = true;

        std::cout << "[DOWNLOAD] Baixando arquivo de mídia direto: " << nome_final << "\n";
        ResultadoDownload r = http_baixar_arquivo(dl_op);
        if (r.ok) {
            std::cout << "\n[SUCESSO] Vídeo baixado com sucesso: " << dest.string()
                      << " (" << formatar_bytes(r.bytes) << ")\n";
            return true;
        }
        std::cerr << "\n[ERRO] Falha no download do vídeo: " << r.erro << "\n";
        return false;
    }

    // Processamento de Stream HLS (.m3u8)
    std::cout << "[HLS] Baixando playlist m3u8...\n";
    OpcoesHttp req_op;
    req_op.headers = headers_stream;
    req_op.timeout_sec = 30.0;
    RespostaHttp resp_m3u8 = http_requisicao(stream_url, req_op);
    if (resp_m3u8.erro_rede || resp_m3u8.status < 200 || resp_m3u8.status >= 300 || resp_m3u8.corpo.empty()) {
        std::cerr << "[ERRO] Falha ao obter playlist m3u8 (HTTP " << resp_m3u8.status << ")\n";
        return false;
    }

    std::string media_playlist_url = stream_url;
    std::string media_conteudo = resp_m3u8.corpo;

    // Se for playlist master, escolhe a variante de melhor qualidade
    if (resp_m3u8.corpo.find("#EXT-X-STREAM-INF") != std::string::npos) {
        media_playlist_url = selecionar_melhor_variante(resp_m3u8.corpo, stream_url, op.qualidade);
        if (media_playlist_url != stream_url) {
            RespostaHttp resp_var = http_requisicao(media_playlist_url, req_op);
            if (resp_var.status >= 200 && resp_var.status < 300 && !resp_var.corpo.empty()) {
                media_conteudo = resp_var.corpo;
            } else {
                std::cerr << "[ERRO] Falha ao baixar variante HLS selecionada.\n";
                return false;
            }
        }
    }

    InfoStreamHLS stream_info;
    if (!analisar_media_playlist(media_conteudo, media_playlist_url, stream_info)) {
        std::cerr << "[ERRO] Não foi possível extrair segmentos da playlist HLS.\n";
        return false;
    }

    size_t total_segmentos = stream_info.segmentos.size();
    std::cout << "[HLS] " << total_segmentos << " segmentos encontrados na playlist.\n";

    // Nome base do arquivo de saída
    std::string nome_base = titulo_video;
    if (nome_base.empty()) {
        nome_base = "video_" + agora_iso();
        std::replace(nome_base.begin(), nome_base.end(), ':', '-');
    }
    nome_base = sanitizar_nome_arquivo(nome_base);

    // Pasta temporária para segmentos
    fs::path tmp_dir = pasta_saida / (".tmp_" + nome_base + "_parts");
    fs::create_directories(tmp_dir, ec);

    // Carrega chaves de criptografia se necessário
    std::map<std::string, std::vector<uint8_t>> cache_chaves;
    for (const auto& seg : stream_info.segmentos) {
        if (!seg.key_uri.empty() && cache_chaves.find(seg.key_uri) == cache_chaves.end()) {
            RespostaHttp r_key = http_requisicao(seg.key_uri, req_op);
            if (r_key.status == 200 && r_key.corpo.size() == 16) {
                cache_chaves[seg.key_uri] = std::vector<uint8_t>(r_key.corpo.begin(), r_key.corpo.end());
            } else {
                std::cerr << "[AVISO] Falha ao obter chave AES-128 para o stream.\n";
            }
        }
    }

    int workers_count = op.max_workers > 0 ? op.max_workers : CFG.download_workers;
    if (workers_count < 1) workers_count = 6;
    if (workers_count > 16) workers_count = 16;

    std::atomic<size_t> idx_global{0};
    std::atomic<size_t> baixados{0};
    std::atomic<size_t> falhas{0};
    std::atomic<long long> total_bytes_baixados{0};

    auto t_inicio = std::chrono::steady_clock::now();
    std::mutex mtx_log;

    std::cout << "[DOWNLOAD] Baixando " << total_segmentos << " segmentos com "
              << workers_count << " workers...\n";

    std::vector<std::thread> workers;
    for (int w = 0; w < workers_count; ++w) {
        workers.emplace_back([&, w] {
            while (true) {
                size_t i = idx_global.fetch_add(1);
                if (i >= total_segmentos) break;

                const auto& seg = stream_info.segmentos[i];
                char nome_part[64];
                std::snprintf(nome_part, sizeof(nome_part), "part_%06zu.ts", i);
                fs::path arq_part = tmp_dir / nome_part;

                // Resume: se já baixado com sucesso
                if (fs::exists(arq_part, ec) && fs::file_size(arq_part, ec) > 512) {
                    baixados.fetch_add(1);
                    continue;
                }

                bool sucesso_seg = false;
                for (int tentativa = 0; tentativa < 3 && !sucesso_seg; ++tentativa) {
                    OpcoesHttp seg_op;
                    seg_op.headers = headers_stream;
                    seg_op.timeout_sec = 45.0;
                    RespostaHttp r = http_requisicao(seg.url, seg_op);
                    if (r.status >= 200 && r.status < 300 && !r.corpo.empty()) {
                        std::vector<uint8_t> dados_seg(r.corpo.begin(), r.corpo.end());

                        // Descriptografia AES-128 se aplicável
                        if (!seg.key_uri.empty() && cache_chaves.count(seg.key_uri)) {
                            const auto& key = cache_chaves[seg.key_uri];
                            uint8_t iv[16] = {0};
                            if (seg.iv.size() == 16) {
                                std::memcpy(iv, seg.iv.data(), 16);
                            } else {
                                // IV derivado da sequência se ausente
                                for (int b = 0; b < 4; ++b) {
                                    iv[15 - b] = (seg.seq >> (b * 8)) & 0xFF;
                                }
                            }
                            std::vector<uint8_t> dec;
                            if (descriptografar_aes128(dados_seg, key.data(), iv, dec)) {
                                dados_seg = std::move(dec);
                            }
                        }

                        // Remove eventuais máscaras PNG / JPEG de CDNs
                        size_t offset_midia = encontrar_inicio_midia(dados_seg.data(), dados_seg.size());
                        std::ofstream out(arq_part, std::ios::binary);
                        if (out) {
                            out.write(reinterpret_cast<const char*>(dados_seg.data() + offset_midia),
                                      dados_seg.size() - offset_midia);
                            out.close();
                            total_bytes_baixados.fetch_add(dados_seg.size() - offset_midia);
                            sucesso_seg = true;
                        }
                    }
                    if (!sucesso_seg && tentativa < 2) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250 * (tentativa + 1)));
                    }
                }

                if (sucesso_seg) {
                    size_t cnt = baixados.fetch_add(1) + 1;
                    if (cnt % 5 == 0 || cnt == total_segmentos) {
                        auto agora = std::chrono::steady_clock::now();
                        double segs = std::chrono::duration<double>(agora - t_inicio).count();
                        double pct = (double)cnt * 100.0 / (double)total_segmentos;
                        double vel_mbs = (segs > 0) ? ((double)total_bytes_baixados.load() / (1024.0 * 1024.0 * segs)) : 0.0;
                        std::lock_guard<std::mutex> lock(mtx_log);
                        std::cout << "\r[HLS] " << cnt << "/" << total_segmentos
                                  << " (" << std::fixed << std::setprecision(1) << pct << "%) "
                                  << std::setprecision(2) << vel_mbs << " MB/s   " << std::flush;
                    }
                } else {
                    falhas.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : workers) t.join();
    std::cout << "\n";

    if (falhas.load() > (total_segmentos / 20)) { // mais de 5% de falhas
        std::cerr << "[ERRO] Muitos segmentos falharam no download (" << falhas.load() << "). Abortando união.\n";
        return false;
    }

    std::cout << "[STITCH] Unindo " << baixados.load() << " segmentos no arquivo final...\n";
    fs::path ts_saida = pasta_saida / (nome_base + ".ts");
    std::ofstream out_ts(ts_saida, std::ios::binary);
    if (!out_ts) {
        std::cerr << "[ERRO] Não foi possível criar arquivo .ts final: " << ts_saida.string() << "\n";
        return false;
    }

    std::vector<char> buffer_leitura(256 * 1024);
    for (size_t i = 0; i < total_segmentos; ++i) {
        char nome_part[64];
        std::snprintf(nome_part, sizeof(nome_part), "part_%06zu.ts", i);
        fs::path arq_part = tmp_dir / nome_part;

        std::ifstream in_part(arq_part, std::ios::binary);
        if (in_part) {
            while (in_part.read(buffer_leitura.data(), buffer_leitura.size()) || in_part.gcount() > 0) {
                out_ts.write(buffer_leitura.data(), in_part.gcount());
            }
            in_part.close();
            fs::remove(arq_part, ec);
        }
    }
    out_ts.close();
    fs::remove_all(tmp_dir, ec);

    long long ts_size = fs::file_size(ts_saida, ec);

    // Tenta conversão/remux para .mp4 via ffmpeg se instalado
    std::string ffmpeg_exe = localizar_ffmpeg();
    if (!ffmpeg_exe.empty()) {
        fs::path mp4_saida = pasta_saida / (nome_base + ".mp4");
        std::cout << "[FFMPEG] Remuxando para MP4 sem perda de qualidade...\n";
        std::string cmd_ffmpeg = "\"\"" + ffmpeg_exe + "\" -y -hide_banner -loglevel warning -i \"" +
                                 ts_saida.string() + "\" -c copy \"" + mp4_saida.string() + "\"\"";
        int r_ff = std::system(cmd_ffmpeg.c_str());
        if (r_ff == 0 && fs::exists(mp4_saida, ec) && fs::file_size(mp4_saida, ec) > 1024) {
            fs::remove(ts_saida, ec);
            long long mp4_size = fs::file_size(mp4_saida, ec);
            std::cout << "\n============================================================\n";
            std::cout << "[SUCESSO] Vídeo concluído: " << mp4_saida.string() << "\n";
            std::cout << "Tamanho final: " << formatar_bytes(mp4_size) << "\n";
            std::cout << "============================================================\n";
            return true;
        }
    }

    std::cout << "\n============================================================\n";
    std::cout << "[SUCESSO] Vídeo MPEG-TS concluído: " << ts_saida.string() << "\n";
    std::cout << "Tamanho final: " << formatar_bytes(ts_size) << "\n";
    std::cout << "============================================================\n";
    return true;
}

bool baixar_video(const std::string& url_ou_pagina, const std::string& pasta_saida, const std::string& nome_personalizado) {
    OpcoesVideo op;
    op.url = url_ou_pagina;
    op.pasta_saida = pasta_saida;
    op.nome_arquivo = nome_personalizado;
    return baixar_video(op);
}

void executar_dataset_video(const DatasetCfg& ds) {
    std::string pasta = resolver_pasta_saida(ds.pasta_saida, "videos", "PASTA_SAIDA");
    std::vector<std::string> urls = ds.lista_urls;
    if (!ds.url.empty()) urls.push_back(ds.url);

    if (urls.empty()) {
        std::cerr << "[ERRO] Dataset de vídeo " << ds.id << " não possui URL configurada.\n";
        return;
    }

    std::cout << "\n============================================================\n";
    std::cout << "[DATASET VIDEO] " << ds.id << ": Baixando " << urls.size() << " episódios/vídeos...\n";
    std::cout << "Pasta de saída: " << pasta << "\n";
    std::cout << "============================================================\n";

    size_t idx = 0;
    for (const auto& u : urls) {
        ++idx;
        std::cout << "\n>>> [" << idx << "/" << urls.size() << "] " << u << "\n";
        OpcoesVideo op;
        op.url = u;
        op.pasta_saida = pasta;
        std::string nome_arquivo = ds.padrao_arquivo;
        if (!nome_arquivo.empty() && (nome_arquivo.find("{ep}") != std::string::npos || nome_arquivo.find("{episodio}") != std::string::npos)) {
            static const std::regex reg_ep(R"([?&]ep=(\d+))");
            std::smatch m;
            std::string ep_str = std::to_string(idx);
            if (std::regex_search(u, m, reg_ep)) {
                ep_str = m[1].str();
            }
            nome_arquivo = aplicar_ctx(nome_arquivo, {{"ep", ep_str}, {"episodio", ep_str}});
        } else if (urls.size() > 1 && !nome_arquivo.empty()) {
            nome_arquivo += "_" + std::to_string(idx);
        }
        op.nome_arquivo = nome_arquivo;
        op.headers = ds.headers;
        op.max_workers = CFG.download_workers;
        baixar_video(op);
    }
}
