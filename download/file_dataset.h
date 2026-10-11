#pragma once
#include "config.h"

// Orquestra e processa downloads salvos diretamente em arquivos (arquivos estáticos, listas, jobs assíncronos)
void executar_dataset_arquivo(const DatasetCfg& ds);

// Download de um arquivo individual com arquivo temporário, verificação de tamanho e headers
bool baixar_arquivo_seguro(const std::string& url,
                           const std::string& caminho_destino,
                           const std::string& api_key = "",
                           int tamanho_min = 0,
                           const std::map<std::string, std::string>& headers = {},
                           double timeout_sec = 1800.0);
