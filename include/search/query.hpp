#pragma once
#include "search/index.hpp"

namespace search {
enum class QueryKind { Term, And, Or, AndNot };
// Дерево смысла запроса: у листа заполнены term/field, у операции — left/right.
// unique_ptr выражает единственное владение: поддеревья не разделяются между
// узлами, поэтому изменение позиции одного итератора не затронет соседний.
struct QueryNode {
    QueryKind kind = QueryKind::Term;
    std::string term;
    uint8_t field = Any;
    std::unique_ptr<QueryNode> left, right;
};
class QueryParser {
  public:
    // Строит владеющее строками AST; исходную строку после возврата можно удалить.
    // Ошибка грамматики выражается исключением, частичное дерево наружу не выходит.
    static std::unique_ptr<QueryNode> parse(std::string_view query);
};
// Операция забирает оба курсора и сама становится курсором. Промежуточный vector
// совпадений не создаётся, поэтому таким способом можно соединить всё дерево.
std::unique_ptr<PostingIterator> combine(QueryKind kind, std::unique_ptr<PostingIterator> left,
                                         std::unique_ptr<PostingIterator> right,
                                         IteratorMetrics* metrics = nullptr);
class QueryExecutor {
  public:
    explicit QueryExecutor(const DiskIndex& index) : index_(index) {}
    // Лист AST превращается в дисковый курсор, операция — в курсор над детьми.
    // Индекс заимствован и должен существовать во время построения этой цепочки.
    std::unique_ptr<PostingIterator> execute(const QueryNode& query, AdvanceMode mode = AdvanceMode::Binary,
                                             IteratorMetrics* metrics = nullptr) const;

  private:
    // Верхняя оценка числа совпадений нужна только для порядка операндов.
    // Неточная оценка влияет на скорость, но не отбрасывает результаты.
    uint64_t estimate(const QueryNode& query) const;
    const DiskIndex& index_;
};
class SearchEngine {
  public:
    explicit SearchEngine(const fs::path& directory) : index_(directory) {}
    // Материализуется только конечная выдача до top_k, а не множества каждого
    // подвыражения. Возвращаются внутренние ID; метаданные CLI читает отдельно.
    std::vector<uint32_t> search(std::string_view query, uint64_t top_k = 10,
                                 AdvanceMode mode = AdvanceMode::Binary,
                                 IteratorMetrics* metrics = nullptr) const;
    const DiskIndex& index() const { return index_; }

  private:
    DiskIndex index_;
};
}
