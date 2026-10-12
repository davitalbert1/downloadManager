#pragma once
#include <windows.h>
#include <commdlg.h>
#include <string>

inline std::string OpenFileDialog(HWND ownerHwnd) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ownerHwnd;
    ofn.lpstrFilter = "Bancos de Dados SQLite (*.db;*.sqlite;*.sqlite3)\0*.db;*.sqlite;*.sqlite3;*.db3\0Todos os Arquivos (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Selecione o arquivo de banco de dados (.db)";

    if (GetOpenFileNameA(&ofn)) return std::string(filename);
    return "";
}
