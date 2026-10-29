#pragma once
#include <string>

void iniciar_rate_limit();
void dormir(double segundos);
void esperar_envio(const std::string& grupo, double custo);
void tratar_429(double espera, const std::string& motivo, const std::string& endpoint,
                const std::string& chave = "");
void restaurar_taxa();
double backoff_exponencial(int tentativa);

// Cooldown individual: um 429 em um endpoint/chave nao bloqueia os demais.
// Retorna true se pode usar agora; caso contrario, *espera_restante recebe
// quantos segundos faltam (se o ponteiro nao for nulo).
bool endpoint_disponivel(const std::string& endpoint, double* espera_restante);
bool chave_disponivel(const std::string& chave, double* espera_restante);
