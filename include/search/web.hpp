#pragma once
#include "search/storage.hpp"
#include <string>

namespace search {
struct WebOptions {
    fs::path index, assets, cache;
    // Дополнительные индексы обнаруживаются только в этом каталоге. HTTP-клиент
    // выбирает готовый идентификатор, а не передаёт произвольный путь на сервере.
    fs::path indexes_root;
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    uint32_t workers = 4;
};

// Индексы остаются открытыми между запросами, как в интерактивном CLI.
// Полные поля читаются из исходного корпуса, отдельно от поискового индекса.
void serve_web(const WebOptions& options);
}
