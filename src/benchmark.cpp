#include "search/benchmark.hpp"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>

namespace search {
namespace {
using Clock = std::chrono::steady_clock;
// Метод nearest rank: после сортировки берём ceil(q*N)-й элемент, считая с 1.
// Для N=100 P95 — 95-й замер, а не среднее соседних. Функция вызывается только
// для непустых выборок: повторы и список запросов проверяются до измерений.
double percentile(const std::vector<double>& sorted, double quantile) {
    const auto rank = static_cast<size_t>(std::ceil(quantile * static_cast<double>(sorted.size())));
    return sorted[rank ? rank - 1 : 0];
}
}
void benchmark_queries(const fs::path& path, const fs::path& queries_path, uint32_t repeats, uint64_t top_k,
                       AdvanceMode mode, std::ostream& out) {
    if (!repeats)
        throw std::runtime_error("repeats должен быть положительным");
    SearchEngine engine(path);
    std::ifstream file(queries_path);
    check_stream(file, "Открытие запросов");
    std::vector<std::string> queries;
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // Проверка грамматики заранее не позволяет включить быстрый отказ на
        // ошибочной строке в статистику обычных поисковых запросов.
        if (!line.empty() && line.front() != '#') {
            QueryParser::parse(line);
            queries.push_back(std::move(line));
        }
    }
    if (queries.empty())
        throw std::runtime_error("Файл запросов пуст");
    // Один прогрев каждого запроса исключён из статистики. Время открытия индекса
    // и чтения метаданных не входит: измеряются parse + plan + top-k doc_id.
    for (const auto& query : queries)
        engine.search(query, top_k, mode);
    std::vector<double> samples;
    std::vector<std::vector<double>> per_query(queries.size());
    IteratorMetrics metrics;
    uint64_t checksum = 0, result_count = 0;
    const auto wall_begin = Clock::now();
    for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
        for (size_t i = 0; i < queries.size(); ++i) {
            const auto begin = Clock::now();
            // Внутренний таймер окружает именно вызов поиска. Подсчёт checksum,
            // накопление выборки и JSON-форматирование не увеличивают latency.
            auto docs = engine.search(queries[i], top_k, mode, &metrics);
            const auto microseconds = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
            samples.push_back(microseconds);
            per_query[i].push_back(microseconds);
            result_count += docs.size();
            for (auto id : docs)
                checksum += id;
        }
    }
    // Внешнее время включает накладные расходы всего цикла. QPS считаем по нему,
    // а не по сумме коротких latency: иначе throughput получился бы завышенным.
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_begin).count();
    const double seconds = std::accumulate(samples.begin(), samples.end(), 0.0) / 1e6;
    std::sort(samples.begin(), samples.end());
    nlohmann::json details = nlohmann::json::array();
    for (size_t i = 0; i < queries.size(); ++i) {
        std::sort(per_query[i].begin(), per_query[i].end());
        details.push_back({{"query", queries[i]},
                           {"p50_us", percentile(per_query[i], .5)},
                           {"p95_us", percentile(per_query[i], .95)},
                           {"p99_us", percentile(per_query[i], .99)},
                           {"returned_per_run", engine.search(queries[i], top_k, mode).size()}});
        // Для одиночного терма сохраняем фактический df, чтобы категория
        // «редкий/частый/отсутствующий» подтверждалась словарём данного индекса.
        // Для составного запроса df отдельных слов не выдаём за число совпадений.
        const auto ast = QueryParser::parse(queries[i]);
        if (ast->kind == QueryKind::Term) {
            const auto info = engine.index().dictionary().find(ast->term);
            details.back()["term_union_df"] = info ? info->df : 0;
        }
    }
    nlohmann::json report = {{"queries", queries.size()},
                             {"repeats", repeats},
                             {"samples", samples.size()},
                             {"top_k", top_k},
                             {"advance", mode == AdvanceMode::Binary ? "binary" : "sequential"},
                             {"p50_us", percentile(samples, .5)},
                             {"p95_us", percentile(samples, .95)},
                             {"p99_us", percentile(samples, .99)},
                             {"throughput_qps", static_cast<double>(samples.size()) / wall_seconds},
                             {"measured_seconds", wall_seconds},
                             {"query_seconds", seconds},
                             {"next_calls", metrics.next_calls},
                             {"advance_calls", metrics.advance_calls},
                             {"comparisons", metrics.comparisons},
                             {"results", result_count},
                             {"checksum", checksum},
                             {"peak_rss_kb", peak_rss_kb()},
                             {"cache", "warm; one unmeasured pass"},
                             {"scope", "parse + plan + top-k internal ids; no metadata"},
                             {"per_query", details}};
    out << report.dump(2) << '\n';
}
void benchmark_intersections(std::ostream& out) {
    nlohmann::json reports = nlohmann::json::array();
    // Длинный список неизменен, короткий выбирает равномерные позиции в нём.
    // Копирование входа исключено из таймера. Повторное создание итераторов нужно,
    // поскольку advance меняет их состояние и не допускает перемещения назад.
    for (uint32_t ratio : {1u, 10u, 100u, 1000u}) {
        std::vector<uint32_t> large(1000000), small;
        // Каждый элемент короткого списка присутствует в длинном. Число ответов
        // известно независимо от исполняемого алгоритма и проверяется после прогона.
        for (uint32_t i = 0; i < large.size(); ++i)
            large[i] = i * 2;
        for (uint32_t i = 0; i < large.size(); i += ratio)
            small.push_back(i * 2);
        for (auto mode : {AdvanceMode::Sequential, AdvanceMode::Binary}) {
            IteratorMetrics metrics;
            double microseconds = 0;
            uint64_t count = 0;
            constexpr uint32_t repeats = 10;
            for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
                auto left = vector_iterator(small, mode, &metrics),
                     right = vector_iterator(large, mode, &metrics);
                const auto begin = Clock::now();
                auto result = combine(QueryKind::And, std::move(left), std::move(right), &metrics);
                while (result->valid()) {
                    ++count;
                    result->next();
                }
                microseconds += std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
            }
            if (count != small.size() * repeats)
                throw std::runtime_error("Ошибка измерения пересечений");
            reports.push_back({{"ratio", ratio},
                               {"short_size", small.size()},
                               {"long_size", large.size()},
                               {"algorithm", mode == AdvanceMode::Binary ? "galloping_binary" : "sequential"},
                               {"mean_us", microseconds / repeats},
                               {"next_calls", metrics.next_calls},
                               {"advance_calls", metrics.advance_calls},
                               {"comparisons", metrics.comparisons},
                               {"checksum", count}});
        }
    }
    out << reports.dump(2) << '\n';
}
void generate_corpus(const fs::path& path, uint64_t documents, uint32_t seed) {
    if (fs::exists(path))
        throw std::runtime_error("Файл уже существует: " + path.string());
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    std::ofstream out(path);
    check_stream(out, "Создание синтетического корпуса");
    std::mt19937 random(seed);
    for (uint64_t i = 0; i < documents; ++i) {
        std::string body = "common search document ";
        if (i % 2 == 0)
            body += "machine learning ";
        if (i % 3 == 0)
            body += "deep learning ";
        if (i % 5 == 0)
            body += "python ";
        if (i % 997 == 0)
            body += "rareterm ";
        // Детерминированные делители дают пересекающиеся множества для булевых
        // операций; случайные wordN создают большой фон. Одинаковый seed задаёт
        // одинаковый корпус, а уникальный номер в title заставляет расти словарь.
        for (unsigned word = 0; word < 100; ++word)
            body += "word" + std::to_string(random() % 10000) + ' ';
        nlohmann::json document = {{"docid", "synthetic_" + std::to_string(i)},
                                   {"url", "https://example.org/" + std::to_string(i)},
                                   {"title", "Document " + std::to_string(i)},
                                   {"headings", "Search Пример"},
                                   {"body", body}};
        out << document.dump() << '\n';
    }
    out.close();
    check_stream(out, "Сохранение синтетического корпуса");
}
}
