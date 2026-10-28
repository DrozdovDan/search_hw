#include "search/benchmark.hpp"
#include "search/web.hpp"
#include <charconv>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include "json.hpp"

namespace {
using namespace search;
// CLI только проверяет аргументы и вызывает библиотеку. Парсер запросов и
// операции над postings не зависят от argc/argv, что позволяет тестировать их отдельно.
class Arguments {
  public:
    Arguments(int argc, char** argv) {
        // argv[1] — команда. Далее всегда пары --имя значение; отрицательное
        // числовое значение не пытаемся принять за новую опцию, его отклонит number.
        for (int i = 2; i < argc; i += 2) {
            std::string key(argv[i]);
            if (!key.starts_with("--") || i + 1 >= argc)
                throw std::runtime_error("Ожидается --параметр значение");
            if (!values_.emplace(key, argv[i + 1]).second)
                throw std::runtime_error("Повтор параметра " + key);
        }
    }
    void allow(std::initializer_list<const char*> keys) const {
        // Набор разрешённых ключей свой у каждой команды. Опечатка не должна
        // молча превращаться в использование значения по умолчанию.
        std::set<std::string> allowed;
        for (auto key : keys)
            allowed.insert(key);
        for (const auto& [key, value] : values_)
            if (!allowed.contains(key))
                throw std::runtime_error("Неизвестный параметр " + key);
    }
    std::string get(const std::string& key, const std::string& fallback = "") const {
        auto it = values_.find(key);
        return it == values_.end() ? fallback : it->second;
    }
    std::string required(const std::string& key) const {
        auto value = get(key);
        if (value.empty())
            throw std::runtime_error("Требуется " + key);
        return value;
    }
    uint64_t number(const std::string& key, uint64_t fallback, uint64_t maximum = UINT64_MAX) const {
        auto value = get(key);
        if (value.empty())
            return fallback;
        uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        // Проверяем и код ошибки, и конец разбора: строка 10abc не равна числу 10.
        // Верхний предел проверяется до последующих приведений к uint32_t.
        if (error != std::errc{} || end != value.data() + value.size() || parsed > maximum)
            throw std::runtime_error("Некорректное число для " + key);
        return parsed;
    }
    AdvanceMode mode() const {
        auto value = get("--advance", "binary");
        if (value == "binary")
            return AdvanceMode::Binary;
        if (value == "sequential")
            return AdvanceMode::Sequential;
        throw std::runtime_error("--advance: binary или sequential");
    }

  private:
    std::map<std::string, std::string> values_;
};
void print_document(uint32_t id, const Metadata& document) {
    std::cout << nlohmann::json({{"internal_id", id},
                                 {"docid", document.docid},
                                 {"title", document.title},
                                 {"url", document.url},
                                 {"snippet", document.snippet}})
                     .dump()
              << '\n';
}
void print_results(const SearchEngine& engine, const std::string& query, uint64_t top_k, AdvanceMode mode) {
    IteratorMetrics metrics;
    const auto results = engine.search(query, top_k, mode, &metrics);
    // JSONL сохраняет табы, переводы строк и управляющие символы в метаданных.
    for (auto id : results) {
        // Поиск отдаёт только компактные ID. Тексты title/URL извлекаем для
        // реально показанных документов, а не для всех кандидатов запроса.
        auto document = engine.index().documents().get(id);
        print_document(id, document);
    }
    // stderr отделяет диагностику от JSONL: stdout можно передавать следующей программе.
    std::cerr << "Результатов: " << results.size() << "; next=" << metrics.next_calls
              << ", advance=" << metrics.advance_calls << '\n';
}
}

int main(int argc, char** argv) {
    using namespace search;
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
            std::cout
                << "search_engine index --corpus PATH --output DIR [--max-docs N|--preset 10k|100k|1m|full]\n"
                   "  [--threads N --memory-limit-mb N --merge-fan-in N --snippet-bytes N --segment-docs N]\n"
                   "search_engine search --index DIR --query EXPR [--top-k N --advance binary|sequential]\n"
                   "search_engine interactive --index DIR [--top-k N --advance binary|sequential]\n"
                   "search_engine serve --index DIR [--host 127.0.0.1 --port 8080 --workers 4]\n"
                   "  [--web-root DIR --cache-dir DIR --indexes-root DIR]\n"
                   "search_engine stats|validate --index DIR\n"
                   "search_engine benchmark --index DIR --queries FILE [--repeats N --top-k N --advance binary|sequential]\n"
                   "search_engine benchmark-intersection\n"
                   "search_engine generate --output FILE --documents N [--seed N]\n";
            return 0;
        }
        const std::string command(argv[1]);
        Arguments args(argc, argv);
        if (command == "index") {
            args.allow({"--corpus", "--output", "--threads", "--memory-limit-mb", "--merge-fan-in",
                        "--max-docs", "--preset", "--snippet-bytes", "--segment-docs"});
            BuildOptions options;
            options.corpus = args.required("--corpus");
            options.output = args.required("--output");
            options.threads = static_cast<uint32_t>(args.number("--threads", 1, 256));
            options.memory_limit_mb = args.number("--memory-limit-mb", 512);
            options.merge_fan_in = static_cast<uint32_t>(args.number("--merge-fan-in", 16, 64));
            options.snippet_bytes = static_cast<uint32_t>(args.number("--snippet-bytes", 0, 65536));
            options.segment_docs = static_cast<uint32_t>(args.number("--segment-docs", 0, UINT32_MAX));
            options.max_docs = args.number("--max-docs", 0, UINT32_MAX);
            const auto preset = args.get("--preset");
            if (!preset.empty()) {
                // preset — удобное имя того же max_docs. Одновременные параметры
                // отклоняем, чтобы не возникало скрытого порядка переопределения.
                if (!args.get("--max-docs").empty())
                    throw std::runtime_error("Задайте только один из --preset и --max-docs");
                if (preset == "10k")
                    options.max_docs = 10000;
                else if (preset == "100k")
                    options.max_docs = 100000;
                else if (preset == "1m")
                    options.max_docs = 1000000;
                else if (preset == "full")
                    options.max_docs = 0;
                else
                    throw std::runtime_error("Неизвестный preset");
            }
            const auto stats = IndexBuilder::build(options);
            std::cout << nlohmann::json(
                             {{"documents", stats.documents},
                              {"terms", stats.terms},
                              {"postings", stats.postings},
                              {"segments", stats.segments},
                              {"index_bytes", stats.index_bytes},
                              {"seconds", stats.seconds},
                              {"documents_per_second",
                               stats.seconds > 0 ? static_cast<double>(stats.documents) / stats.seconds : 0},
                              {"peak_rss_kb", stats.peak_rss_kb},
                              {"max_tracked_bytes", stats.max_tracked_bytes}})
                             .dump(2)
                      << '\n';
        } else if (command == "search" || command == "interactive") {
            if (command == "search")
                args.allow({"--index", "--query", "--top-k", "--advance"});
            else
                args.allow({"--index", "--top-k", "--advance"});
            SearchEngine engine(args.required("--index"));
            const auto top_k = args.number("--top-k", 10);
            const auto mode = args.mode();
            if (command == "search")
                print_results(engine, args.required("--query"), top_k, mode);
            else {
                std::cerr << "Введите запрос; :quit завершает работу.\n";
                for (std::string query; std::getline(std::cin, query);) {
                    if (query == ":quit")
                        break;
                    // Ошибка одной строки оставляет сессию и открытый индекс живыми.
                    // Ошибки первоначального открытия обрабатываются внешним catch.
                    try {
                        print_results(engine, query, top_k, mode);
                    } catch (const std::exception& error) {
                        std::cerr << "Ошибка: " << error.what() << '\n';
                    }
                }
            }
        } else if (command == "serve") {
            args.allow(
                {"--index", "--host", "--port", "--workers", "--web-root", "--cache-dir", "--indexes-root"});
            WebOptions options;
            options.index = fs::absolute(args.required("--index"));
            options.host = args.get("--host", "127.0.0.1");
            options.port = static_cast<uint16_t>(args.number("--port", 8080, 65535));
            options.workers = static_cast<uint32_t>(args.number("--workers", 4, 32));
            options.assets = args.get("--web-root", SEARCH_WEB_ROOT);
            options.cache = args.get("--cache-dir", (options.index.parent_path() / "web-cache").string());
            options.indexes_root = args.get("--indexes-root", options.index.parent_path().string());
            serve_web(options);
        } else if (command == "stats") {
            // DiskIndex сначала сверяет манифест с файлами. Только после этого
            // показываем статистику, чтобы не представить обрезанный индекс готовым.
            args.allow({"--index"});
            const fs::path path = args.required("--index");
            DiskIndex index(path);
            std::ifstream in(path / "manifest.json");
            auto manifest = nlohmann::json::parse(in);
            manifest["index_bytes"] = directory_size(path);
            std::cout << manifest.dump(2) << '\n';
        } else if (command == "validate") {
            args.allow({"--index"});
            validate_index(args.required("--index"));
            std::cout << "Индекс корректен\n";
        } else if (command == "benchmark") {
            args.allow({"--index", "--queries", "--repeats", "--top-k", "--advance"});
            benchmark_queries(args.required("--index"), args.required("--queries"),
                              static_cast<uint32_t>(args.number("--repeats", 100, UINT32_MAX)),
                              args.number("--top-k", 10), args.mode(), std::cout);
        } else if (command == "benchmark-intersection") {
            args.allow({});
            benchmark_intersections(std::cout);
        } else if (command == "generate") {
            args.allow({"--output", "--documents", "--seed"});
            generate_corpus(args.required("--output"), args.number("--documents", 10000, UINT32_MAX),
                            static_cast<uint32_t>(args.number("--seed", 42, UINT32_MAX)));
        } else
            throw std::runtime_error("Неизвестная команда: " + command);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка: " << error.what() << '\n';
        return 1;
    }
}
