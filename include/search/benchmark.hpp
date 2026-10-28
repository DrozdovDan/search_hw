#pragma once
#include "search/query.hpp"
#include <ostream>
namespace search {
// Один индекс открывается на весь прогон. Замер включает разбор и получение ID;
// прогрев, открытие файлов, чтение метаданных и JSON-отчёт в latency не входят.
void benchmark_queries(const fs::path& index, const fs::path& queries, uint32_t repeats, uint64_t top_k,
                       AdvanceMode mode, std::ostream& out);
// Изолирует влияние отношения длин списков от токенизации и дискового ввода.
void benchmark_intersections(std::ostream& out);
// Фиксированный seed позволяет воспроизвести тот же набор терминов и документов.
void generate_corpus(const fs::path& path, uint64_t documents, uint32_t seed);
}
