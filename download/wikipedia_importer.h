#pragma once
#include <string>

// Importador de dumps da Wikipedia/Wikimedia (.xml.bz2) para SQLite.
// Replica o pipeline Python de F:\wikipedia:
//   download.py      -> lista + baixa os .xml.bz2 do diretorio remoto
//   importDumps.py   -> parse <page> por <page> + INSERT em lote com resume
//   limpeza.py       -> limpeza de wikitexto (versao C++ simplificada)
//
// Esquema criado no banco destino (igual ao importDumps.py):
//   paginas(page_id PK, titulo, namespace, projeto, data, origem, texto)
//   progresso(arquivo PK, completo, ultimo_page_id)
//   categorias(page_id, categoria) PK(page_id, categoria)
//   links(page_id, destino) PK(page_id, destino)
//
// Config via [dataset:*] (download.ini):
//   tipo_saida = wikipedia
//   url = https://dumps.wikimedia.org/other/mediawiki_content_current/enwiki/2026-07-01/xml/bzip2/
//   pasta_saida = F:\wikipedia\dump\enwiki   (ou relativo)
//   db = F:\wikipedia\wikipedia.db
//   projeto = enwiki          (opcional: deduzido do nome do arquivo)
//   data_dump = 2026-07-01    (opcional: deduzido do nome/URL)
//   apenas_ns0 = 1            (default 1: so namespace 0)
//   ignorar_redirect = 1      (default 1)
//   tamanho_minimo = 20       (default 20)
//   wiki_batch = 2000         (paginas por COMMIT)
//   listar_remoto = 1         (default 1; 0 = importa so o que ja esta em pasta_saida)

struct DatasetCfg;

void executar_dataset_wikipedia(const DatasetCfg& ds);
bool importar_arquivo_wikipedia_bz2(const std::string& caminho_bz2,
                                    const std::string& db_path,
                                    const std::string& projeto,
                                    const std::string& data_dump,
                                    const std::string& origem_nome,
                                    bool apenas_ns0,
                                    bool ignorar_redirect,
                                    int tamanho_minimo,
                                    int batch_size);
