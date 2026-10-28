#include "search/query.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unicode/uchar.h>
#include <unicode/utf8.h>

namespace search {
namespace {
// Лексер выделяет синтаксические единицы, но ещё не решает порядок операций.
// position хранится в байтах исходного UTF-8 для точного указания ошибки.
enum class TokenKind { Term, And, Or, AndNot, Left, Right, End };
struct Token {
    TokenKind kind;
    std::string text;
    size_t position;
    uint8_t field = Any;
};

class QueryLexer {
  public:
    explicit QueryLexer(std::string_view input) : input_(input) {}
    Token next() {
        // Пробелы отделяют элементы запроса; скобки имеют собственные границы,
        // поэтому a AND(b OR c) допустимо без пробела перед открывающей скобкой.
        while (position_ < input_.size()) {
            const auto [space, width] = character(position_);
            if (!space)
                break;
            position_ += width;
        }
        const auto start = position_;
        if (start == input_.size())
            return {TokenKind::End, {}, start};
        if (input_[position_] == '(') {
            ++position_;
            return {TokenKind::Left, {}, start};
        }
        if (input_[position_] == ')') {
            ++position_;
            return {TokenKind::Right, {}, start};
        }
        while (position_ < input_.size() && input_[position_] != '(' && input_[position_] != ')') {
            const auto [space, width] = character(position_);
            if (space)
                break;
            position_ += width;
        }
        std::string raw(input_.substr(start, position_ - start));
        // Операторы распознаются ДО нормализации и только в верхнем регистре.
        // Благодаря этому слово and можно искать как обычное слово документа.
        if (raw == "AND")
            return {TokenKind::And, {}, start};
        if (raw == "OR")
            return {TokenKind::Or, {}, start};
        if (raw == "ANDNOT")
            return {TokenKind::AndNot, {}, start};
        if (raw == "NOT")
            throw std::runtime_error("Унарный NOT не поддерживается, байт " + std::to_string(start));
        uint8_t field = Any;
        // Префикс относится к одному терму. После его удаления остаток проходит
        // ровно тот же TextAnalyzer, который использовался при построении индекса.
        if (const auto colon = raw.find(':'); colon != std::string::npos) {
            const auto name = raw.substr(0, colon);
            if (name == "title")
                field = Title;
            else if (name == "headings")
                field = Headings;
            else if (name == "body")
                field = Body;
            else
                throw std::runtime_error("Неизвестное поле: " + name);
            raw.erase(0, colon + 1);
        }
        auto words = TextAnalyzer::tokenize(raw);
        // Лексема должна означать один терм. Неявное AND между словами и фразовый
        // поиск не определены: например, foo-bar требует явного foo AND bar.
        if (words.size() != 1)
            throw std::runtime_error("Ожидается один терм, байт " + std::to_string(start));
        return {TokenKind::Term, std::move(words.front()), start, field};
    }

  private:
    std::pair<bool, size_t> character(size_t offset) const {
        // ASCII обрабатывается дёшево, многобайтовые символы целиком декодирует ICU.
        // Возвращаем ширину символа, иначе шаг на один char попал бы в середину UTF-8.
        // Это также позволяет считать NBSP и другие Unicode-пробелы разделителями.
        const auto byte = static_cast<unsigned char>(input_[offset]);
        if (byte < 128)
            return {std::isspace(byte) != 0, 1};
        int32_t end = static_cast<int32_t>(offset);
        UChar32 codepoint;
        U8_NEXT(reinterpret_cast<const uint8_t*>(input_.data()), end, static_cast<int32_t>(input_.size()),
                codepoint);
        if (codepoint < 0)
            throw std::runtime_error("Некорректный UTF-8, байт " + std::to_string(offset));
        return {u_isUWhiteSpace(codepoint) != 0, static_cast<size_t>(end) - offset};
    }
    std::string_view input_;
    size_t position_ = 0;
};

int precedence(TokenKind kind) {
    // Скобка получает ноль: она служит границей стека и не сворачивается как
    // операция. ANDNOT имеет приоритет AND, поэтому a ANDNOT b AND c — это (a\b) AND c.
    if (kind == TokenKind::Or)
        return 1;
    if (kind == TokenKind::And || kind == TokenKind::AndNot)
        return 2;
    return 0;
}
QueryKind operation(TokenKind kind) {
    if (kind == TokenKind::And)
        return QueryKind::And;
    if (kind == TokenKind::Or)
        return QueryKind::Or;
    return QueryKind::AndNot;
}

// Каждое подвыражение тоже выглядит как отсортированный поток. Храним только
// два дочерних курсора и текущий ответ, а не все совпадения поддерева.
// align устанавливает инвариант: present_ означает, что current_ — ближайший
// ещё не выданный ответ операции. После next/advance инвариант восстанавливается.
class BinaryIterator final : public PostingIterator {
  public:
    BinaryIterator(QueryKind kind, std::unique_ptr<PostingIterator> left,
                   std::unique_ptr<PostingIterator> right, IteratorMetrics* metrics)
        : kind_(kind), left_(std::move(left)), right_(std::move(right)), metrics_(metrics) {
        align();
    }
    bool valid() const override { return present_; }
    uint32_t doc_id() const override {
        if (!present_)
            throw std::out_of_range("Итератор исчерпан");
        return current_;
    }
    void next() override {
        if (metrics_)
            ++metrics_->next_calls;
        if (!present_)
            return;
        if (kind_ == QueryKind::Or) {
            // Если один doc_id присутствует в обоих потоках, продвигаем оба.
            // Иначе второй ребёнок вернул бы тот же документ ещё раз.
            if (left_->valid() && left_->doc_id() == current_)
                left_->next();
            if (right_->valid() && right_->doc_id() == current_)
                right_->next();
            // AND нашёл равенство, ANDNOT — допустимый левый документ. В обоих случаях
            // достаточно сдвинуть левую сторону; align подведёт правую при необходимости.
        } else
            left_->next();
        align();
    }
    void advance(uint32_t target) override {
        if (metrics_)
            ++metrics_->advance_calls;
        if (!present_ || current_ >= target)
            return;
        left_->advance(target);
        // Для ANDNOT правая сторона тоже монотонна: исключения меньше target
        // больше не понадобятся. Никакого просмотра всего универсального множества.
        right_->advance(target);
        align();
    }

  private:
    void align() {
        present_ = false;
        if (kind_ == QueryKind::Or) {
            // Минимальный из текущих ID — следующий элемент объединения. Если
            // один поток закончился, оставшийся уже отсортирован и может выдаваться целиком.
            if (!left_->valid() && !right_->valid())
                return;
            current_ = !right_->valid()  ? left_->doc_id()
                       : !left_->valid() ? right_->doc_id()
                                         : std::min(left_->doc_id(), right_->doc_id());
            present_ = true;
            return;
        }
        while (left_->valid()) {
            if (kind_ == QueryKind::And) {
                if (!right_->valid())
                    return;
                // Если меньший doc_id не встречается на текущей позиции второго
                // списка, то не встретится и позже: оба списка строго возрастают.
                // advance пропускает сразу весь заведомо бесполезный диапазон.
                // Например, [2, 7, 100] и [50, 100]: у первого сразу ищем >=50,
                // получаем 100, затем подводим второй к 100 и возвращаем совпадение.
                if (left_->doc_id() < right_->doc_id())
                    left_->advance(right_->doc_id());
                else if (right_->doc_id() < left_->doc_id())
                    right_->advance(left_->doc_id());
                else {
                    current_ = left_->doc_id();
                    present_ = true;
                    return;
                }
            } else {
                // Разность A\B проверяет только кандидатов A. Подводим B к
                // текущему A: меньшее значение B уже не может исключить этот ID.
                if (right_->valid() && right_->doc_id() < left_->doc_id())
                    right_->advance(left_->doc_id());
                if (!right_->valid() || left_->doc_id() != right_->doc_id()) {
                    // B закончился или уже перескочил ID слева: в отсортированном
                    // B этот документ больше не встретится, поэтому его можно выдать.
                    current_ = left_->doc_id();
                    present_ = true;
                    return;
                }
                // При равенстве левый кандидат запрещён. Ищем следующий без
                // построения дополнения B относительно всех документов корпуса.
                left_->next();
            }
        }
    }
    QueryKind kind_;
    std::unique_ptr<PostingIterator> left_, right_;
    IteratorMetrics* metrics_;
    bool present_ = false;
    uint32_t current_ = 0;
};
}

std::unique_ptr<QueryNode> QueryParser::parse(std::string_view query) {
    // Итеративный shunting-yard не расходует стек C++ на каждую пару скобок.
    // Операторы AND и ANDNOT равноприоритетны и левоассоциативны; OR ниже.
    // Ограничен размер AST, а не глубина одних скобок. Это защищает последующее
    // рекурсивное исполнение/уничтожение дерева от запросов на миллионы узлов.
    if (query.size() > 1024 * 1024)
        throw std::runtime_error("Запрос превышает 1 МиБ");
    QueryLexer lexer(query);
    std::vector<TokenKind> operators;
    std::vector<std::unique_ptr<QueryNode>> operands;
    // После '(' или операции нужен операнд, после терма или ')' — операция/конец.
    // Этот флаг отсекает a b, () и AND a раньше возможного обращения к пустому стеку.
    bool expected_operand = true;
    size_t nodes = 0;
    auto reduce = [&] {
        // Свёртка заменяет два готовых поддерева одним узлом. Правое снимается
        // первым, потому что было положено последним; для ANDNOT порядок критичен.
        if (operands.size() < 2 || operators.empty())
            throw std::runtime_error("Недостаточно операндов");
        auto node = std::make_unique<QueryNode>();
        node->kind = operation(operators.back());
        operators.pop_back();
        node->right = std::move(operands.back());
        operands.pop_back();
        node->left = std::move(operands.back());
        operands.pop_back();
        operands.push_back(std::move(node));
    };
    while (true) {
        auto token = lexer.next();
        if (token.kind == TokenKind::End)
            break;
        if (token.kind != TokenKind::Left && token.kind != TokenKind::Right && ++nodes > 2048)
            throw std::runtime_error("Запрос содержит слишком много узлов");
        auto error = [&] {
            throw std::runtime_error("Синтаксическая ошибка, байт " + std::to_string(token.position));
        };
        if (token.kind == TokenKind::Term) {
            if (!expected_operand)
                error();
            auto node = std::make_unique<QueryNode>();
            node->term = std::move(token.text);
            node->field = token.field;
            operands.push_back(std::move(node));
            expected_operand = false;
        } else if (token.kind == TokenKind::Left) {
            if (!expected_operand)
                error();
            operators.push_back(TokenKind::Left);
        } else if (token.kind == TokenKind::Right) {
            // Закрывающая скобка заставляет завершить только операции своей группы.
            // Саму '(' удаляем, но в AST она не нужна: группировку уже задаёт дерево.
            if (expected_operand)
                error();
            while (!operators.empty() && operators.back() != TokenKind::Left)
                reduce();
            if (operators.empty())
                error();
            operators.pop_back();
            expected_operand = false;
        } else {
            if (expected_operand)
                error();
            // >=, а не >, реализует левую ассоциативность при равном приоритете:
            // a ANDNOT b ANDNOT c сворачивается как (a\b)\c.
            while (!operators.empty() && precedence(operators.back()) >= precedence(token.kind))
                reduce();
            operators.push_back(token.kind);
            expected_operand = true;
        }
    }
    if (expected_operand)
        throw std::runtime_error("Пустой запрос или отсутствует правый операнд");
    // Остаток стека — операции верхнего уровня. Оставшаяся '(' означает,
    // что соответствующая ')' так и не встретилась; такое дерево не возвращаем.
    while (!operators.empty()) {
        if (operators.back() == TokenKind::Left)
            throw std::runtime_error("Незакрытая скобка");
        reduce();
    }
    if (operands.size() != 1)
        throw std::runtime_error("Некорректный запрос");
    return std::move(operands.back());
}
std::unique_ptr<PostingIterator> combine(QueryKind kind, std::unique_ptr<PostingIterator> left,
                                         std::unique_ptr<PostingIterator> right, IteratorMetrics* metrics) {
    if (kind == QueryKind::Term || !left || !right)
        throw std::invalid_argument("Некорректная булева операция");
    return std::make_unique<BinaryIterator>(kind, std::move(left), std::move(right), metrics);
}
uint64_t QueryExecutor::estimate(const QueryNode& query) const {
    // Для листа df точен по объединению полей; фильтр поля может лишь уменьшить
    // ответ. Для пересечения размер не превосходит меньшего входа, для разности —
    // левого, для объединения — суммы и общего числа документов. Корреляции слов
    // неизвестны, поэтому это оценки сверху, а не точная кардинальность запроса.
    if (query.kind == QueryKind::Term) {
        auto info = index_.dictionary().find(query.term);
        return info ? info->df : 0;
    }
    const auto left = estimate(*query.left);
    if (query.kind == QueryKind::AndNot)
        return left;
    const auto right = estimate(*query.right);
    return query.kind == QueryKind::And ? std::min(left, right)
                                        : std::min(index_.documents().size(), left + right);
}
std::unique_ptr<PostingIterator> QueryExecutor::execute(const QueryNode& query, AdvanceMode mode,
                                                        IteratorMetrics* metrics) const {
    // Рекурсивно повторяем форму AST, заменяя каждый его узел исполняемым курсором.
    // Дети независимы: повтор одного терма в запросе создаёт два отдельных положения.
    if (query.kind == QueryKind::Term)
        return index_.postings(query.term, query.field, mode, metrics);
    auto* left = query.left.get();
    auto* right = query.right.get();
    // Перестановка безопасна только у коммутативных операций. Оценка df не требует
    // чтения postings; для AND меньший поток становится ведущим. ANDNOT сохраняет
    // исходный порядок, иначе изменился бы смысл запроса.
    if ((query.kind == QueryKind::And || query.kind == QueryKind::Or) && estimate(*right) < estimate(*left))
        std::swap(left, right);
    return combine(query.kind, execute(*left, mode, metrics), execute(*right, mode, metrics), metrics);
}
std::vector<uint32_t> SearchEngine::search(std::string_view query, uint64_t top_k, AdvanceMode mode,
                                           IteratorMetrics* metrics) const {
    auto ast = QueryParser::parse(query);
    if (!top_k)
        return {};
    auto iterator = QueryExecutor(index_).execute(*ast, mode, metrics);
    std::vector<uint32_t> results;
    // Каждый запрос следующего результата тянет только необходимые сдвиги вниз
    // по дереву. Как только набрано top_k, оставшиеся совпадения не вычисляются.
    while (iterator->valid() && results.size() < top_k) {
        results.push_back(iterator->doc_id());
        // Не вычисляем даже один лишний результат после последнего требуемого.
        if (results.size() < top_k)
            iterator->next();
    }
    return results;
}
}
