#pragma once
#include "config.h"

// Orquestra e processa downloads salvos diretamente em arquivos (arquivos estáticos, listas, jobs assíncronos)
void executar_dataset_arquivo(const DatasetCfg& ds);

// Download de um arquivo individual com arquivo temporário, verificação de tamanho e headers
// tamanho_max <= 0 = sem limite. Se pulado_por_tamanho != nullptr, recebe true quando o
// arquivo foi PULADO por exceder o maximo (nao conta como falha de rede).
bool baixar_arquivo_seguro(const std::string& url,
                           const std::string& caminho_destino,
                           const std::string& api_key = "",
                           long long tamanho_min = 0,
                           const std::map<std::string, std::string>& headers = {},
                           double timeout_sec = 1800.0,
                           long long tamanho_max = 0,
                           bool* pulado_por_tamanho = nullptr);
