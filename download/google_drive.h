#pragma once
#include "config.h"
#include <string>

// Executa download de arquivos do Google Drive configurados via DatasetCfg ou download.ini
bool executar_dataset_google_drive(const DatasetCfg& ds);
