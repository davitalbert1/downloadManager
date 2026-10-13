# Compilar

Os dois executáveis (`clima_viewer.exe` e `download.exe`) são gerados pelo mesmo `CMakeLists.txt`, na raiz do projeto.

## Pré-requisitos (MSYS2)

No shell MSYS2 MinGW64 (use `abrir_mingw.bat`), instale o que falta:

```sh
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-base mingw-w64-x86_64-curl mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-nlohmann-json
```

## Configurar

No shell MinGW64 do MSYS2, na raiz do projeto:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Alternativa sem Ninja:

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
```

## Compilar

```sh
cmake --build build
```

## Compilar auto

```sh
./compilar.bat
```

Isso gera:

- `build/clima_viewer.exe` — visualizador Qt (lê `clima.db` do diretório atual)
- `build/download.exe` — downloader (usa `download.ini` do diretório atual)

## Executar

Clima viewer:

```sh
./build/clima_viewer.exe
```

db manager:
```sh
./build/db_manager.exe
```

### Formas de execução do Downloader (`download.exe`):

O `download.exe` é totalmente universal e controlado pelo `download.ini` ou arquivo `.env`. Ele suporta baixar datasets salvando tanto em banco de dados SQLite (`tipo_saida = db`) quanto diretamente em arquivos organizados por pasta (`tipo_saida = arquivo`, como no caso de dados **ERA5** da Copernicus CDS API).

#### 1. Executar todos os datasets do `download.ini`
```cmd
./build/download.exe
```
ou especificando o arquivo `.ini`:
```cmd
./build/download.exe download.ini
```

#### 2. Executar apenas uma seção/dataset específico
Você pode passar diretamente o nome da seção do dataset (com ou sem o prefixo `dataset:`):
```cmd
./build/download.exe era5
```
```cmd
./build/download.exe dataset:era5
```
Com o caminho do arquivo `.ini` explícito:
```cmd
./build/download.exe download.ini era5
```

#### 3. Usar flags de seleção de dataset (`-d`, `--dataset`, `-s`, `--secao`)
```cmd
./build/download.exe -d era5
```
```cmd
./build/download.exe download.ini --dataset era5
```

#### 4. Baixar múltiplos datasets específicos
múltiplos datasets separados por vírgula ou por argumentos distintos:
```cmd
./build/download.exe era5,clima_diario
```
```cmd
./build/download.exe download.ini era5 clima_diario
```
```cmd
./build/download.exe -d era5 -d clima_diario
```

#### 5. Modos de Saída (Definidos no `download.ini`)

| `tipo_saida` | Destino | Exemplo |
|---|---|---|
| `db` (padrão) | Tabela SQLite (`db` + `tabela`) | APIs JSON (Open-Meteo, PokeAPI, etc.) |
| `arquivo` | Arquivos em `pasta_saida` | ERA5 / scraping HTML (`html_imgs`) |
| `wikipedia` ou `wiki` | Dumps `.xml.bz2` em `pasta_saida` + importação para SQLite | Ver seção 6 |
| `drive` ou `google_drive` | Google Drive (`folder_id`) -> `pasta_saida` | Ver `[dataset:drive_backup]` no `download.ini` |

Auditor:

```sh
./build/clima_auditor.exe
```

No PowerShell/VS Code (raiz do projeto):

```powershell
./build/download.exe era5
./build/clima_viewer.exe   # execute a partir de app\ para achar clima.db
```

---

### 6. Importar dumps Wikipedia / Wikimedia (`.xml.bz2` -> SQLite)

O `download.exe` espelha o pipeline Python de `F:\wikipedia` (`download.py` + `importDumps.py` + `limpeza.py`) em C++ (`download/wikipedia_importer.cpp`):

1. **Lista** os `.xml.bz2` do diretório remoto (`url`).
2. **Baixa** os que ainda não existem em `pasta_saida` (pula os já baixados com tamanho > 0).
3. **Importa** cada `.xml.bz2` para o SQLite (`db`): parse `<page>` a `<page>`, limpeza de wikitexto, `INSERT` em lote (`wiki_batch` por `COMMIT`) com `resume` pela tabela `progresso` (reexecutar continua de onde parou).

Esquema criado no banco destino (igual ao `importDumps.py`):

- `paginas(page_id PK, titulo, namespace, projeto, data, origem, texto)`
- `progresso(arquivo PK, completo, ultimo_page_id)`
- `categorias(page_id, categoria)` — `PK(page_id, categoria)`
- `links(page_id, destino)` — `PK(page_id, destino)`

#### 6.1 Configurar (`download.ini`)

Exemplo comentado já existe no `download.ini`. Descomente/adapte:

```ini
[dataset:wikipedia_enwiki]
tipo_saida = wikipedia
url = https://dumps.wikimedia.org/other/mediawiki_content_current/enwiki/2026-07-01/xml/bzip2/
pasta_saida = F:\wikipedia\dump\enwiki
db = F:\wikipedia\wikipedia.db
projeto = enwiki
data_dump = 2026-07-01
apenas_ns0 = 1
ignorar_redirect = 1
tamanho_minimo = 20
wiki_batch = 2000
listar_remoto = 1
```

Parâmetros:

| Chave | Obrigatório | Padrão | O que faz |
|---|---|---|---|
| `tipo_saida` | sim | — | Use `wikipedia` (aceita `wiki`). Sem isso o dataset roda como API genérica. |
| `url` | não* | `""` | Diretório remoto com os `.xml.bz2` (ex: `.../enwiki/2026-07-01/xml/bzip2/`). *Se vazio, importa **somente o local**. |
| `pasta_saida` | sim | `wikipedia_dump` | Onde os `.xml.bz2` são salvos/lidos. Aceita absoluto ou relativo ao CWD. É criada se não existir. |
| `db` | não | `db_path` do `[geral]` | Banco SQLite destino (ex: `F:\wikipedia\wikipedia.db`). |
| `projeto` | não | deduzido do nome do arquivo (`enwiki-...` -> `enwiki`) | Gravado na coluna `paginas.projeto`. |
| `data_dump` | não | deduzido do nome/URL (`-YYYY-MM-DD-`) | Gravado na coluna `paginas.data`. |
| `apenas_ns0` | não | `1` | `1` = importa só namespace `0` (artigos); `0` = importa todos. |
| `ignorar_redirect` | não | `1` | `1` = pula páginas `#REDIRECT` / `#REDIRECIONAMENTO`. |
| `tamanho_minimo` | não | `20` | Descarta páginas com texto limpo menor que isso (caracteres). |
| `wiki_batch` | não | `2000` | Páginas por `COMMIT` (lote de `INSERT`). |
| `listar_remoto` | não | `1` | `1` = lista o diretório remoto via HTTP; `0` = importa **só o que já está** em `pasta_saida` (modo offline). |
| `tamanho_min_bytes` | não | `0` | Tamanho mínimo aceito no download (protege contra `.bz2` truncado). |

Outros projetos/idiomas: basta trocar `url`/`pasta_saida`/`projeto`/`data_dump`. Ex: `ptwiki`, `eswiki`, `commonswiki`, `wikidatawiki`.

#### 6.2 Executar

Só o dataset Wikipedia (recomendado — o dump completo tem dezenas de GB):

```cmd
./build/download.exe wikipedia_enwiki
```

Variações (mesmo padrão das outras seções):

```cmd
./build/download.exe dataset:wikipedia_enwiki
./build/download.exe download.ini wikipedia_enwiki
./build/download.exe -d wikipedia_enwiki
./build/download.exe download.ini --dataset wikipedia_enwiki
```

Somente local, sem listar/baixar do remoto (útil para reimportar/recontinuar):

```ini
listar_remoto = 0
```

```cmd
./build/download.exe wikipedia_enwiki
```

Executar tudo do `download.ini` (inclui Wikipedia + demais datasets):

```cmd
./build/download.exe
```

O `resume` é automático: arquivos com `progresso.completo = 1` são pulados; interromper com `Ctrl+C` e rodar de novo continua do último `COMMIT`.

#### 6.3 Verificar o resultado

```sh
sqlite3 "F:\wikipedia\wikipedia.db" "SELECT COUNT(*), projeto, data FROM paginas GROUP BY projeto, data;"
sqlite3 "F:\wikipedia\wikipedia.db" "SELECT arquivo, completo, ultimo_page_id FROM progresso;"
sqlite3 "F:\wikipedia\wikipedia.db" "SELECT * FROM paginas LIMIT 2;"
```

Ou abra o arquivo no `dbManager.exe` / `clima_auditor.exe`.

#### 6.4 Dicas / solução de problemas

- `Nenhum .xml.bz2 (pasta vazia / lista remota falhou)` = `pasta_saida` vazia e a listagem HTTP falhou (sem rede ou `url` errada). Confira a `url` no navegador e `http_timeout` no `[geral]`.
- Download recomeça do zero se o `.bz2` local estiver com tamanho `0`. Arquivo parcial > 0 **não** é retomado — apague o `.bz2` incompleto para baixar de novo.
- Disco: o `enwiki` completo precisa de ~20 GB só para os `.bz2` + espaço extra para o `.db`. Para teste, baixe 1 arquivo manualmente para `pasta_saida`, ponha `listar_remoto = 0` e rode o importador.
- Performance: aumente `wiki_batch` (ex: `5000`) para `COMMIT`s maiores; `apenas_ns0 = 1` + `ignorar_redirect = 1` reduzem muito o volume importado.

