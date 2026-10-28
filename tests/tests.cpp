#include "search/benchmark.hpp"
#include "search/corpus.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <unistd.h>
#include <zlib.h>

using namespace search;
namespace {
uint64_t checks = 0;
// Проверки не используют assert: Release с NDEBUG обязан выполнять тот же набор,
// что Debug/ASAN, и завершаться ненулевым кодом при первой ошибке.
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F function) {
    bool failed = false;
    try {
        function();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "Ожидалась ошибка");
}
std::vector<uint32_t> collect(std::unique_ptr<PostingIterator> iterator) {
    std::vector<uint32_t> result;
    while (iterator->valid()) {
        result.push_back(iterator->doc_id());
        iterator->next();
    }
    return result;
}
void write_documents(const fs::path& path, const std::vector<Document>& documents) {
    std::ofstream out(path);
    for (const auto& d : documents)
        out << nlohmann::json({{"docid", d.docid},
                               {"url", d.url},
                               {"title", d.title},
                               {"headings", d.headings},
                               {"body", d.body}})
                   .dump()
            << '\n';
}
void gzip_file(const fs::path& source, const fs::path& output) {
    // Маленький тестовый файл допустимо целиком держать в памяти. Это помощник
    // создания fixtures; производственный CorpusReader так корпус не читает.
    std::ifstream in(source, std::ios::binary);
    const std::string value((std::istreambuf_iterator<char>(in)), {});
    gzFile file = gzopen(output.c_str(), "wb");
    require(file != nullptr, "gzopen");
    require(gzwrite(file, value.data(), static_cast<unsigned>(value.size())) ==
                static_cast<int>(value.size()),
            "gzwrite");
    require(gzclose(file) == Z_OK, "gzclose");
}
void tar_file(const fs::path& source, const fs::path& output) {
    // Минимальный настоящий tar: заголовок 512 байт, полезные данные, выравнивание
    // и два нулевых блока. Благодаря этому reader проверяется на байтах архива,
    // а не через подменённый тестовый интерфейс чтения.
    std::ifstream in(source, std::ios::binary);
    std::string value((std::istreambuf_iterator<char>(in)), {});
    std::array<char, 512> header{};
    std::string name = source.filename();
    std::copy(name.begin(), name.end(), header.begin());
    snprintf(header.data() + 124, 12, "%011lo", static_cast<unsigned long>(value.size()));
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    header[156] = '0';
    unsigned checksum = 0;
    for (auto c : header)
        checksum += static_cast<unsigned char>(c);
    snprintf(header.data() + 148, 8, "%06o", checksum);
    header[155] = ' ';
    std::ofstream out(output, std::ios::binary);
    out.write(header.data(), header.size());
    out << value;
    std::string padding((512 - value.size() % 512) % 512 + 1024, '\0');
    out << padding;
}
void analyzer_tests() {
    require(TextAnalyzer::tokenize("Hello, HELLO! Ёжик ёЖИК café 中文 123") ==
                std::vector<std::string>({"hello", "hello", "ёжик", "ёжик", "café", "中文", "123"}),
            "Unicode lowercase");
    require(TextAnalyzer::tokenize("").empty(), "Пустая строка");
    require(TextAnalyzer::tokenize("... \t\n—!").empty(), "Пунктуация");
    require(TextAnalyzer::tokenize("FOO-bar_42") == std::vector<std::string>({"foo", "bar", "42"}),
            "Разделители");
    require(TextAnalyzer::tokenize("e\xcc\x81") == std::vector<std::string>({"e\xcc\x81"}),
            "Combining marks");
    rejects([] {
        TextAnalyzer::tokenize(std::string("\xff", 1));
    });
    Document d{"1", "", "Word word", "WORD", "word"};
    auto terms = TextAnalyzer::analyze(d);
    require(terms.size() == 1 && terms[0].first == "word" && terms[0].second == 7, "Дедупликация и поля");
}
void parser_tests() {
    auto node = QueryParser::parse("a OR b AND c ANDNOT d");
    require(node->kind == QueryKind::Or && node->right->kind == QueryKind::AndNot &&
                node->right->left->kind == QueryKind::And,
            "Приоритет и ассоциативность");
    require(QueryParser::parse("title:ПрИвЕт!")->term == "привет", "Нормализация запроса");
    require(QueryParser::parse("\u2003title:Привет\u00a0AND\tbody:Мир")->kind == QueryKind::And,
            "Пробелы Unicode в грамматике");
    for (const std::string query : {"", " ", "AND a", "a AND", "()", "(a", "a)", "a b", "a OR OR b", "NOT a",
                                    "a NOT b", "a(b)", "bad:a", "title:", "foo-bar", "a AND ()"})
        rejects([&] {
            QueryParser::parse(query);
        });
    std::string deep(10000, '(');
    deep += "a";
    deep += std::string(10000, ')');
    require(QueryParser::parse(deep)->term == "a", "Глубокие скобки без рекурсии");
    std::mt19937 random(9182);
    // Здесь не утверждаем, что любая случайная строка ошибочна: часть из них
    // законные термы. Проверяем завершение без повреждения памяти/падения;
    // точную грамматику проверяют именованные случаи выше.
    const std::string alphabet = "abc () ANDORNT:_!";
    for (unsigned i = 0; i < 3000; ++i) {
        std::string input;
        for (unsigned j = 0, n = random() % 100; j < n; ++j)
            input += alphabet[random() % alphabet.size()];
        try {
            QueryParser::parse(input);
        } catch (const std::exception&) {
        }
    }
}
void iterator_tests() {
    std::mt19937 random(42);
    for (unsigned trial = 0; trial < 1000; ++trial) {
        // Генерируем уже отсортированные уникальные ID; эталон STL вычисляет
        // операции независимо от кода наших курсоров. Разные плотности дают
        // короткие и длинные промежутки для проверки advance.
        std::vector<uint32_t> a, b;
        for (uint32_t doc = 0; doc < 400; ++doc) {
            if (random() % 3 == 0)
                a.push_back(doc);
            if (random() % 7 == 0)
                b.push_back(doc);
        }
        for (auto mode : {AdvanceMode::Sequential, AdvanceMode::Binary}) {
            auto iterator = vector_iterator(a, mode);
            size_t expected = 0;
            for (unsigned step = 0; step < 50; ++step) {
                const uint32_t target = random() % 500;
                // Цели могут уменьшаться. Эталонный курсор тоже не двигается
                // назад: advance — продолжение прохода, а не новый поиск с начала.
                while (expected < a.size() && a[expected] < target)
                    ++expected;
                iterator->advance(target);
                require(iterator->valid() == (expected < a.size()), "advance valid");
                if (iterator->valid()) {
                    require(iterator->doc_id() == a[expected], "advance lower_bound");
                    if (random() % 2) {
                        iterator->next();
                        ++expected;
                    }
                }
            }
            iterator->advance(UINT32_MAX);
            require(!iterator->valid(), "advance до конца");
            iterator->next();
            iterator->advance(0);
            require(!iterator->valid(), "Конец устойчив");
            rejects([&] {
                iterator->doc_id();
            });
            for (auto kind : {QueryKind::And, QueryKind::Or, QueryKind::AndNot}) {
                std::vector<uint32_t> reference;
                if (kind == QueryKind::And)
                    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                                          std::back_inserter(reference));
                if (kind == QueryKind::Or)
                    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(reference));
                if (kind == QueryKind::AndNot)
                    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                                        std::back_inserter(reference));
                require(collect(combine(kind, vector_iterator(a, mode), vector_iterator(b, mode))) ==
                            reference,
                        "Случайная булева операция");
                auto combined = combine(kind, vector_iterator(a, mode), vector_iterator(b, mode));
                size_t position = 0;
                for (uint32_t target = 0; target < 420; target += 13) {
                    while (position < reference.size() && reference[position] < target)
                        ++position;
                    combined->advance(target);
                    require(combined->valid() == (position < reference.size()), "Булев advance valid");
                    if (combined->valid())
                        require(combined->doc_id() == reference[position], "Булев advance lower_bound");
                }
            }
        }
    }
    auto edge = vector_iterator({0, UINT32_MAX});
    edge->advance(UINT32_MAX);
    require(edge->valid() && edge->doc_id() == UINT32_MAX, "Нет sentinel-конфликта");
    rejects([] {
        vector_iterator({3, 3});
    });
}
bool reference_match(const QueryNode& node, const std::map<std::string, uint8_t>& terms) {
    // Эталон отвечает на вопрос для ОДНОГО документа обычной булевой логикой.
    // Он ничего не знает о сортированных списках, mmap и advance, поэтому может
    // выявить общую ошибку в соединении итераторов. analyze проверяется отдельно.
    if (node.kind == QueryKind::Term) {
        auto it = terms.find(node.term);
        return it != terms.end() && (!node.field || (it->second & node.field));
    }
    const bool left = reference_match(*node.left, terms), right = reference_match(*node.right, terms);
    if (node.kind == QueryKind::And)
        return left && right;
    if (node.kind == QueryKind::Or)
        return left || right;
    return left && !right;
}
std::string random_query(std::mt19937& random, unsigned depth) {
    static const std::vector<std::string> words{"alpha",   "beta",        "gamma",     "delta",
                                                "missing", "title:alpha", "body:beta", "headings:gamma"};
    if (!depth || random() % 3 == 0)
        return words[random() % words.size()];
    static const std::vector<std::string> operators{" AND ", " OR ", " ANDNOT "};
    auto left = random_query(random, depth - 1), right = random_query(random, depth - 1);
    return "(" + left + operators[random() % operators.size()] + right + ")";
}
void disk_tests(const fs::path& root) {
    std::vector<Document> documents{{"empty", "", "", "", ""},
                                    {"one", "url", "Alpha alpha", "gamma", "beta βήτα"},
                                    {"two", "", "beta", "", "alpha gamma"},
                                    {"three", "", "delta", "alpha", "delta"}};
    std::mt19937 random(73);
    const std::array<std::string, 4> words{"alpha", "beta", "gamma", "delta"};
    for (unsigned i = 0; i < 90; ++i) {
        Document document;
        document.docid = "doc" + std::to_string(i);
        for (auto* field : {&document.title, &document.headings, &document.body})
            for (const auto& word : words)
                if (random() % 3 == 0)
                    *field += word + ' ';
        documents.push_back(std::move(document));
    }
    auto corpus = root / "corpus.jsonl";
    write_documents(corpus, documents);
    gzip_file(corpus, root / "corpus.jsonl.gz");
    tar_file(root / "corpus.jsonl.gz", root / "corpus.tar");
    fs::create_directory(root / "directory");
    write_documents(root / "directory" / "z.jsonl", {documents[1]});
    write_documents(root / "directory-part.jsonl", {documents[2]});
    gzip_file(root / "directory-part.jsonl", root / "directory" / "a.gz");
    {
        std::ofstream ignored(root / "directory" / "README.txt");
        ignored << "not JSON";
    }
    uint64_t directory_count = 0;
    CorpusReader::read(root / "directory", [&](Document&& d) {
        require(d.docid == documents[directory_count == 0 ? 2 : 1].docid,
                "Лексикографический порядок файлов каталога");
        ++directory_count;
        return true;
    });
    require(directory_count == 2, "Смешанные JSONL/gzip в каталоге, посторонние файлы пропущены");
    {
        std::ofstream edge(root / "lines.jsonl");
        const std::string record =
            R"({"docid":"nullfields","title":null,"url":null,"headings":null,"body":null})";
        edge << "\r\n" << record << "\r\n\n" << record;
        edge.close();
        uint64_t count = 0;
        CorpusReader::read(root / "lines.jsonl", [&](Document&& d) {
            require(d.title.empty() && d.headings.empty() && d.body.empty() && d.url.empty(), "Поля null");
            ++count;
            return true;
        });
        require(count == 2, "CRLF, пустые строки и последняя строка без newline");
    }
    for (const auto& source : {corpus, root / "corpus.jsonl.gz", root / "corpus.tar"}) {
        uint64_t count = 0;
        CorpusReader::read(source, [&](Document&& d) {
            require(d.docid == documents[count].docid && d.body == documents[count].body, "Потоковое чтение");
            ++count;
            return true;
        });
        require(count == documents.size(), "Количество прочитанных документов");
        count = 0;
        CorpusReader::read(source, [&](Document&&) {
            return ++count < 2;
        });
        require(count == 2, "Остановка по префиксу");
    }
    {
        std::ifstream in(root / "corpus.jsonl.gz", std::ios::binary);
        const std::string compressed((std::istreambuf_iterator<char>(in)), {});
        std::ofstream out(root / "concatenated.gz", std::ios::binary);
        out << compressed << compressed;
        out.close();
        uint64_t count = 0;
        CorpusReader::read(root / "concatenated.gz", [&](Document&&) {
            ++count;
            return true;
        });
        require(count == documents.size() * 2, "Несколько members в gzip");
        auto large = documents.front();
        large.body.assign(1024 * 1024, 'x');
        write_documents(root / "large.jsonl", {large});
        gzip_file(root / "large.jsonl", root / "large.gz");
        CorpusReader::read(root / "large.gz", [&](Document&& d) {
            require(d.body == large.body, "Строка пересекает буферы inflate");
            return true;
        });
        std::fstream corrupt(root / "large.gz", std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(-8, std::ios::end);
        corrupt.put('\0');
        corrupt.close();
        rejects([&] {
            CorpusReader::read(root / "large.gz", [](Document&&) {
                return true;
            });
        });
    }
    BuildOptions options;
    options.corpus = root / "corpus.tar";
    options.output = root / "index";
    options.segment_docs = 3;
    options.merge_fan_in = 2;
    options.memory_limit_mb = 8;
    options.threads = 3;
    options.snippet_bytes = 7;
    // Малые сегменты и fan-in=2 принуждают несколько уровней слияния даже на
    // десятках документов: иначе тест проверил бы только один лёгкий проход.
    auto stats = IndexBuilder::build(options);
    require(stats.documents == documents.size() && stats.segments > 20, "Многопроходное слияние");
    validate_index(options.output);
    std::vector<std::map<std::string, uint8_t>> reference;
    for (const auto& d : documents) {
        std::map<std::string, uint8_t> terms;
        for (const auto& [word, field] : TextAnalyzer::analyze(d))
            terms[word] = field;
        reference.push_back(std::move(terms));
    }
    {
        SearchEngine engine(options.output);
        require(engine.index().documents().get(1).docid == "one", "Метаданные");
        require(engine.index().documents().get(1).snippet == "beta β", "UTF-8 сниппет");
        for (unsigned i = 0; i < 1000; ++i) {
            const auto query = random_query(random, 5);
            const auto ast = QueryParser::parse(query);
            std::vector<uint32_t> expected;
            // Полный перебор допустим для маленького эталона. Основной движок
            // должен получить ту же упорядоченную выдачу через дисковые списки.
            for (uint32_t doc = 0; doc < reference.size(); ++doc)
                if (reference_match(*ast, reference[doc]))
                    expected.push_back(doc);
            for (auto mode : {AdvanceMode::Sequential, AdvanceMode::Binary})
                require(engine.search(query, 1000, mode) == expected,
                        "Сравнение с полным перебором: " + query);
            if (!expected.empty())
                require(engine.search(query, 1) == std::vector<uint32_t>{expected[0]}, "top-k");
        }
        require(engine.search("alpha", 0).empty(), "top-k 0");
        for (uint64_t i = 0; i < engine.index().dictionary().size(); ++i) {
            const auto [term, info] = engine.index().dictionary().at(i);
            auto sequential = engine.index().postings(term, Any, AdvanceMode::Sequential);
            auto binary = engine.index().postings(term);
            auto expected = collect(std::move(sequential));
            require(expected.size() == info.df, "df");
            size_t pos = 0;
            for (uint32_t target = 0; target < documents.size() + 10; target += 7) {
                while (pos < expected.size() && expected[pos] < target)
                    ++pos;
                binary->advance(target);
                require(binary->valid() == (pos < expected.size()), "Дисковый advance valid");
                if (binary->valid())
                    require(binary->doc_id() == expected[pos], "Дисковый advance");
            }
        }
    }
    SearchEngine reopened(options.output);
    auto expected = reopened.search("(alpha OR beta) ANDNOT delta", 1000);
    options.output = root / "single";
    options.segment_docs = 0;
    options.threads = 1;
    IndexBuilder::build(options);
    SearchEngine single(options.output);
    require(single.search("(alpha OR beta) ANDNOT delta", 1000) == expected,
            "Однопоточный и сегментный индексы совпадают");
    for (const auto* name :
         {"terms.data", "terms.offsets", "postings.data", "documents.offsets", "documents.data"}) {
        // Сравниваем данные побайтово, а не только один запрос. Манифест исключён:
        // время, RSS, число потоков и сегментов в разных построениях различаются.
        std::ifstream a(root / "index" / name, std::ios::binary), b(root / "single" / name, std::ios::binary);
        require(std::string((std::istreambuf_iterator<char>(a)), {}) ==
                    std::string((std::istreambuf_iterator<char>(b)), {}),
                "Детерминированная сериализация");
    }
    rejects([&] {
        IndexBuilder::build(options);
    });
    options.output = root / "prefix";
    options.max_docs = 2;
    require(IndexBuilder::build(options).documents == 2, "max-docs");
    write_documents(root / "empty.jsonl", {});
    options.corpus = root / "empty.jsonl";
    options.output = root / "empty";
    options.max_docs = 0;
    require(IndexBuilder::build(options).documents == 0, "Пустой корпус");
    validate_index(options.output);
    require(SearchEngine(options.output).search("alpha").empty(), "Пустой индекс");
    {
        std::ofstream out(root / "bad.jsonl");
        out << "{bad json}\n";
    }
    options.corpus = root / "bad.jsonl";
    options.output = root / "failed";
    rejects([&] {
        IndexBuilder::build(options);
    });
    require(!fs::exists(options.output), "Ошибка не публикует индекс");
    fs::copy_file(root / "corpus.jsonl.gz", root / "broken.gz");
    fs::resize_file(root / "broken.gz", 20);
    rejects([&] {
        CorpusReader::read(root / "broken.gz", [](Document&&) {
            return true;
        });
    });
    {
        std::ofstream out(root / "passage.jsonl");
        out << "{\"docid\":\"d\",\"pid\":\"p\",\"passage\":\"abc\"}\n";
    }
    rejects([&] {
        CorpusReader::read(root / "passage.jsonl", [](Document&&) {
            return true;
        });
    });
    fs::resize_file(root / "prefix" / "postings.data", 1);
    rejects([&] {
        DiskIndex broken(root / "prefix");
    });
    generate_corpus(root / "spill.jsonl", 20000, 42);
    options.corpus = root / "spill.jsonl";
    options.output = root / "spill";
    options.threads = 2;
    options.memory_limit_mb = 8;
    options.snippet_bytes = 0;
    auto spilled = IndexBuilder::build(options);
    require(spilled.segments > 1 && spilled.max_tracked_bytes <= 6 * 1024 * 1024,
            "Автоматический сброс по бюджету памяти");
    validate_index(options.output);
    require(SearchEngine(options.output).search("common", 30000).size() == 20000,
            "Postings после автоматического сброса");
    Document oversized{"oversized", "", "", "", ""};
    // Первый документ не проходит оценку бюджета, пока reader уже может ждать
    // места в очереди. Этот случай проверяет и ошибку лимита, и корректную отмену
    // конвейера без зависания на join и без публикации частичного индекса.
    for (unsigned i = 0; i < 30000; ++i)
        oversized.body += "distinct" + std::to_string(i) + ' ';
    std::vector<Document> cancellation(100, documents[1]);
    cancellation.insert(cancellation.begin(), std::move(oversized));
    write_documents(root / "oversized.jsonl", cancellation);
    options.corpus = root / "oversized.jsonl";
    options.output = root / "oversized-index";
    rejects([&] {
        IndexBuilder::build(options);
    });
    require(!fs::exists(options.output), "Отмена конвейера при превышении бюджета одного документа");
}
}
int main() {
    const auto root = fs::temp_directory_path() / ("boolean-search-tests-" + std::to_string(getpid()));
    try {
        fs::create_directory(root);
        analyzer_tests();
        parser_tests();
        iterator_tests();
        disk_tests(root);
        fs::remove_all(root);
        std::cout << "Пройдено проверок: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка теста: " << error.what() << "; файлы: " << root << '\n';
        return 1;
    }
}
