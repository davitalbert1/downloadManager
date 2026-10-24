#pragma once
#include "config.h"
#include <map>
#include <string>
#include <vector>

// Opções para download de vídeo ou stream
struct OpcoesVideo {
    std::string url;
    std::string pasta_saida = "videos";
    std::string nome_arquivo; // se informado, usa esse nome de arquivo final
    std::string qualidade; // ex: "1080p", "720p", "melhor" (padrão: "melhor")
    std::map<std::string, std::string> headers;
    int max_workers = 6;
};

// Baixa um vídeo ou stream a partir de URL direta ou página web (suporta enma.lol, HLS .m3u8, MP4, etc.)
bool baixar_video(const OpcoesVideo& op);
bool baixar_video(const std::string& url_ou_pagina, const std::string& pasta_saida = "videos", const std::string& nome_personalizado = "");

// Executa um dataset com tipo_saida = "video" ou "stream"
void executar_dataset_video(const DatasetCfg& ds);

// Verifica se uma URL aparenta ser mídia direta (.mp4, .mkv, .m3u8, .webm, etc.)
bool eh_midia_direta(const std::string& url);
