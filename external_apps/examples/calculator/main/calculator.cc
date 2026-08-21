#include "metalio_app_api.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum class CalculatorMode : uint8_t { Standard, Scientific };
enum class KeyAction : uint8_t {
    Digit, Decimal, Clear, Backspace, Sign, Percent, Operator, Equals,
    Function, Constant, OpenParenthesis, CloseParenthesis,
};
enum class KeyStyle : uint8_t { Number, Utility, Operator, Equals, Scientific };

struct KeyDefinition {
    const char* label;
    KeyAction action;
    const char* token;
    uint8_t column;
    uint8_t row;
    uint8_t column_span;
    KeyStyle style;
};

constexpr KeyDefinition kStandardKeys[] = {
    {"AC", KeyAction::Clear, "", 0, 0, 1, KeyStyle::Utility},
    {"退格", KeyAction::Backspace, "", 1, 0, 1, KeyStyle::Utility},
    {"±", KeyAction::Sign, "", 2, 0, 1, KeyStyle::Utility},
    {"%", KeyAction::Percent, "", 3, 0, 1, KeyStyle::Utility},
    {"7", KeyAction::Digit, "7", 0, 1, 1, KeyStyle::Number},
    {"8", KeyAction::Digit, "8", 1, 1, 1, KeyStyle::Number},
    {"9", KeyAction::Digit, "9", 2, 1, 1, KeyStyle::Number},
    {"÷", KeyAction::Operator, "/", 3, 1, 1, KeyStyle::Operator},
    {"4", KeyAction::Digit, "4", 0, 2, 1, KeyStyle::Number},
    {"5", KeyAction::Digit, "5", 1, 2, 1, KeyStyle::Number},
    {"6", KeyAction::Digit, "6", 2, 2, 1, KeyStyle::Number},
    {"×", KeyAction::Operator, "*", 3, 2, 1, KeyStyle::Operator},
    {"1", KeyAction::Digit, "1", 0, 3, 1, KeyStyle::Number},
    {"2", KeyAction::Digit, "2", 1, 3, 1, KeyStyle::Number},
    {"3", KeyAction::Digit, "3", 2, 3, 1, KeyStyle::Number},
    {"−", KeyAction::Operator, "-", 3, 3, 1, KeyStyle::Operator},
    {"0", KeyAction::Digit, "0", 0, 4, 1, KeyStyle::Number},
    {".", KeyAction::Decimal, ".", 1, 4, 1, KeyStyle::Number},
    {"=", KeyAction::Equals, "", 2, 4, 1, KeyStyle::Equals},
    {"+", KeyAction::Operator, "+", 3, 4, 1, KeyStyle::Operator},
};

constexpr KeyDefinition kScientificKeys[] = {
    {"sin", KeyAction::Function, "sin", 0, 0, 1, KeyStyle::Scientific},
    {"cos", KeyAction::Function, "cos", 1, 0, 1, KeyStyle::Scientific},
    {"tan", KeyAction::Function, "tan", 2, 0, 1, KeyStyle::Scientific},
    {"ln", KeyAction::Function, "ln", 3, 0, 1, KeyStyle::Scientific},
    {"log", KeyAction::Function, "log", 4, 0, 1, KeyStyle::Scientific},
    {"√", KeyAction::Function, "sqrt", 5, 0, 1, KeyStyle::Scientific},
    {"x²", KeyAction::Function, "square", 6, 0, 1, KeyStyle::Scientific},
    {"1/x", KeyAction::Function, "reciprocal", 0, 1, 1, KeyStyle::Scientific},
    {"π", KeyAction::Constant, "pi", 1, 1, 1, KeyStyle::Scientific},
    {"e", KeyAction::Constant, "e", 2, 1, 1, KeyStyle::Scientific},
    {"(", KeyAction::OpenParenthesis, "(", 3, 1, 1, KeyStyle::Scientific},
    {")", KeyAction::CloseParenthesis, ")", 4, 1, 1, KeyStyle::Scientific},
    {"xʸ", KeyAction::Operator, "^", 5, 1, 1, KeyStyle::Scientific},
    {"mod", KeyAction::Operator, "mod", 6, 1, 1, KeyStyle::Scientific},
    {"AC", KeyAction::Clear, "", 0, 2, 1, KeyStyle::Utility},
    {"±", KeyAction::Sign, "", 1, 2, 1, KeyStyle::Utility},
    {"%", KeyAction::Percent, "", 2, 2, 1, KeyStyle::Utility},
    {"7", KeyAction::Digit, "7", 3, 2, 1, KeyStyle::Number},
    {"8", KeyAction::Digit, "8", 4, 2, 1, KeyStyle::Number},
    {"9", KeyAction::Digit, "9", 5, 2, 1, KeyStyle::Number},
    {"÷", KeyAction::Operator, "/", 6, 2, 1, KeyStyle::Operator},
    {"退格", KeyAction::Backspace, "", 0, 3, 1, KeyStyle::Utility},
    {"4", KeyAction::Digit, "4", 1, 3, 1, KeyStyle::Number},
    {"5", KeyAction::Digit, "5", 2, 3, 1, KeyStyle::Number},
    {"6", KeyAction::Digit, "6", 3, 3, 1, KeyStyle::Number},
    {"×", KeyAction::Operator, "*", 4, 3, 1, KeyStyle::Operator},
    {"−", KeyAction::Operator, "-", 5, 3, 1, KeyStyle::Operator},
    {"+", KeyAction::Operator, "+", 6, 3, 1, KeyStyle::Operator},
    {"1", KeyAction::Digit, "1", 0, 4, 1, KeyStyle::Number},
    {"2", KeyAction::Digit, "2", 1, 4, 1, KeyStyle::Number},
    {"3", KeyAction::Digit, "3", 2, 4, 1, KeyStyle::Number},
    {"0", KeyAction::Digit, "0", 3, 4, 1, KeyStyle::Number},
    {".", KeyAction::Decimal, ".", 4, 4, 1, KeyStyle::Number},
    {"=", KeyAction::Equals, "", 5, 4, 2, KeyStyle::Equals},
};

struct AppState {
    const metalio_app_host_api_t* api = nullptr;
    const metalio_app_launch_context_t* launch = nullptr;
    metalio_app_capabilities_t capabilities = 0;
    metalio_app_theme_t theme{};
    metalio_app_widget_t display = 0;
    metalio_app_widget_t history_label = 0;
    metalio_app_widget_t result_label = 0;
    metalio_app_widget_t standard_grid = 0;
    metalio_app_widget_t scientific_grid = 0;
    metalio_app_widget_t mode_segment = 0;
    metalio_app_widget_t standard_buttons[sizeof(kStandardKeys) / sizeof(kStandardKeys[0])]{};
    metalio_app_widget_t scientific_buttons[sizeof(kScientificKeys) / sizeof(kScientificKeys[0])]{};
    char expression[128]{};
    char history[160]{};
    char display_text[96] = "0";
    CalculatorMode mode = CalculatorMode::Standard;
    bool after_equals = false;
    bool error = false;
};

AppState s_app{};

bool HasControlApi(const metalio_app_host_api_t* api) {
    return api != nullptr && api->struct_size >=
        offsetof(metalio_app_host_api_t, set_action_picker_selected) +
            sizeof(api->set_action_picker_selected) &&
        api->set_label_alignment != nullptr && api->set_label_font != nullptr &&
        api->set_widget_visible != nullptr && api->set_rect_border != nullptr &&
        api->set_button_border != nullptr && api->add_action_segment != nullptr;
}

bool EndsWith(const char* text, const char* suffix) {
    if (text == nullptr || suffix == nullptr) return false;
    const size_t text_length = std::strlen(text);
    const size_t suffix_length = std::strlen(suffix);
    return suffix_length <= text_length &&
        std::memcmp(text + text_length - suffix_length, suffix, suffix_length) == 0;
}

bool Append(char* destination, size_t capacity, const char* text) {
    const size_t used = std::strlen(destination);
    const size_t added = text != nullptr ? std::strlen(text) : 0;
    if (used + added >= capacity) return false;
    if (added != 0) std::memcpy(destination + used, text, added + 1);
    return true;
}

void Copy(char* destination, size_t capacity, const char* text) {
    if (capacity != 0) std::snprintf(destination, capacity, "%s", text != nullptr ? text : "");
}

bool ExpressionEndsValue(const char* expression) {
    if (expression == nullptr || expression[0] == '\0') return false;
    const size_t length = std::strlen(expression);
    const char last = expression[length - 1];
    return (last >= '0' && last <= '9') || last == ')' || last == '%' ||
           EndsWith(expression, "pi") || last == 'e';
}

int ParenthesisBalance(const char* expression) {
    int balance = 0;
    for (const char* cursor = expression; cursor != nullptr && *cursor != '\0'; ++cursor) {
        if (*cursor == '(') ++balance;
        if (*cursor == ')') --balance;
    }
    return balance;
}

void FormatNumber(double value, char* output, size_t capacity) {
    if (!std::isfinite(value)) {
        Copy(output, capacity, "错误");
        return;
    }
    const double nearest = std::round(value);
    if (std::fabs(value - nearest) < 1e-9) value = nearest;
    if (std::fabs(value) < 1e-12) value = 0.0;
    char compact[48];
    std::snprintf(compact, sizeof(compact), "%.10g", value);
    if (std::strlen(compact) <= 12) Copy(output, capacity, compact);
    else std::snprintf(output, capacity, "%.6e", value);
}

void FormatExpression(const char* source, char* output, size_t capacity) {
    output[0] = '\0';
    const char* cursor = source != nullptr ? source : "";
    while (*cursor != '\0') {
        if (std::strncmp(cursor, "reciprocal", 10) == 0) {
            Append(output, capacity, "倒数"); cursor += 10;
        } else if (std::strncmp(cursor, "square", 6) == 0) {
            Append(output, capacity, "平方"); cursor += 6;
        } else if (std::strncmp(cursor, "sqrt", 4) == 0) {
            Append(output, capacity, "√"); cursor += 4;
        } else if (std::strncmp(cursor, "mod", 3) == 0) {
            Append(output, capacity, " mod "); cursor += 3;
        } else if (std::strncmp(cursor, "pi", 2) == 0) {
            Append(output, capacity, "π"); cursor += 2;
        } else {
            char token[2] = {*cursor++, '\0'};
            if (token[0] == '*') Append(output, capacity, "×");
            else if (token[0] == '/') Append(output, capacity, "÷");
            else if (token[0] == '-') Append(output, capacity, "−");
            else Append(output, capacity, token);
        }
    }
}

class ScientificParser {
public:
    explicit ScientificParser(const char* source) : cursor_(source != nullptr ? source : "") {}
    bool Parse(double* result) {
        if (result == nullptr || *cursor_ == '\0') return false;
        okay_ = true;
        const double value = ParseExpression();
        if (!okay_ || *cursor_ != '\0' || !std::isfinite(value)) return false;
        *result = value;
        return true;
    }

private:
    bool Take(const char* token) {
        const size_t length = std::strlen(token);
        if (std::strncmp(cursor_, token, length) != 0) return false;
        cursor_ += length;
        return true;
    }
    double Fail() { okay_ = false; return 0.0; }
    double ParseExpression() {
        double value = ParseTerm();
        while (okay_) {
            if (Take("+")) value += ParseTerm();
            else if (Take("-")) value -= ParseTerm();
            else break;
        }
        return value;
    }
    double ParseTerm() {
        double value = ParsePower();
        while (okay_) {
            if (Take("*")) value *= ParsePower();
            else if (Take("/")) value /= ParsePower();
            else if (Take("mod")) value = std::fmod(value, ParsePower());
            else break;
        }
        return value;
    }
    double ParsePower() {
        const double base = ParseUnary();
        return Take("^") ? std::pow(base, ParsePower()) : base;
    }
    double ApplyFunction(const char* name, double value) {
        constexpr double kPi = 3.14159265358979323846;
        if (std::strcmp(name, "sin") == 0) return std::sin(value * kPi / 180.0);
        if (std::strcmp(name, "cos") == 0) return std::cos(value * kPi / 180.0);
        if (std::strcmp(name, "tan") == 0) {
            const double radians = value * kPi / 180.0;
            return std::fabs(std::cos(radians)) < 1e-12 ? std::nan("") : std::tan(radians);
        }
        if (std::strcmp(name, "ln") == 0) return value > 0.0 ? std::log(value) : std::nan("");
        if (std::strcmp(name, "log") == 0) return value > 0.0 ? std::log10(value) : std::nan("");
        if (std::strcmp(name, "sqrt") == 0) return value >= 0.0 ? std::sqrt(value) : std::nan("");
        if (std::strcmp(name, "square") == 0) return value * value;
        if (std::strcmp(name, "reciprocal") == 0) return value != 0.0 ? 1.0 / value : std::nan("");
        return std::nan("");
    }
    double ParseUnary() {
        if (Take("-")) return -ParseUnary();
        constexpr const char* kFunctions[] = {"reciprocal", "square", "sqrt", "sin", "cos", "tan", "log", "ln"};
        for (const char* function : kFunctions) {
            if (!Take(function)) continue;
            if (!Take("(")) return Fail();
            const double value = ParseExpression();
            if (!Take(")")) return Fail();
            return ApplyFunction(function, value);
        }
        double value = ParsePrimary();
        while (Take("%")) value /= 100.0;
        return value;
    }
    double ParsePrimary() {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kE = 2.71828182845904523536;
        if (Take("(")) {
            const double value = ParseExpression();
            return Take(")") ? value : Fail();
        }
        if (Take("pi")) return kPi;
        if (Take("e")) return kE;
        char* end = nullptr;
        const double value = std::strtod(cursor_, &end);
        if (end == cursor_) return Fail();
        cursor_ = end;
        return value;
    }
    const char* cursor_;
    bool okay_ = true;
};

void Haptic(metalio_app_haptic_effect_t effect) {
    if ((s_app.capabilities & METALIO_APP_CAP_HAPTICS) != 0 && s_app.api->play_haptic != nullptr) {
        s_app.api->play_haptic(s_app.launch->host_context, effect);
    }
}

void RenderEntry() {
    if (s_app.after_equals || s_app.error) Copy(s_app.display_text, sizeof(s_app.display_text), s_app.error ? "错误" : s_app.expression);
    else {
        FormatExpression(s_app.expression, s_app.display_text, sizeof(s_app.display_text));
        if (s_app.display_text[0] == '\0') Copy(s_app.display_text, sizeof(s_app.display_text), "0");
    }
    s_app.api->set_label_text(s_app.launch->host_context, s_app.history_label, s_app.history);
    s_app.api->set_label_text(s_app.launch->host_context, s_app.result_label, s_app.display_text);
    s_app.api->set_label_color(s_app.launch->host_context, s_app.result_label, s_app.error ? s_app.theme.danger : s_app.theme.text);
    s_app.api->set_label_font(s_app.launch->host_context, s_app.result_label,
        std::strlen(s_app.display_text) > 18 ? METALIO_APP_FONT_MEDIUM : METALIO_APP_FONT_LARGE);
}

void ResetEntry(bool clear_history) {
    s_app.expression[0] = '\0';
    Copy(s_app.display_text, sizeof(s_app.display_text), "0");
    if (clear_history) s_app.history[0] = '\0';
    s_app.after_equals = false;
    s_app.error = false;
    RenderEntry();
}

void BeginFreshEntryIfNeeded() {
    if (!s_app.after_equals) return;
    s_app.expression[0] = '\0';
    s_app.after_equals = false;
    s_app.error = false;
}

void InputDigit(const char* digit) {
    BeginFreshEntryIfNeeded();
    if (ExpressionEndsValue(s_app.expression) && (EndsWith(s_app.expression, "pi") ||
        EndsWith(s_app.expression, "e") || EndsWith(s_app.expression, ")") || EndsWith(s_app.expression, "%"))) {
        Append(s_app.expression, sizeof(s_app.expression), "*");
    }
    size_t digits = 0;
    const char* cursor = s_app.expression + std::strlen(s_app.expression);
    while (cursor > s_app.expression) {
        const char value = cursor[-1];
        if ((value >= '0' && value <= '9') || value == '.') {
            if (value != '.') ++digits;
            --cursor;
        } else break;
    }
    if (digits < 10) Append(s_app.expression, sizeof(s_app.expression), digit);
    RenderEntry();
}

void InputDecimal() {
    BeginFreshEntryIfNeeded();
    const char* end = s_app.expression + std::strlen(s_app.expression);
    const char* start = end;
    while (start > s_app.expression && ((start[-1] >= '0' && start[-1] <= '9') || start[-1] == '.')) --start;
    if (std::memchr(start, '.', static_cast<size_t>(end - start)) != nullptr) return;
    if (start == end) Append(s_app.expression, sizeof(s_app.expression), "0");
    Append(s_app.expression, sizeof(s_app.expression), ".");
    RenderEntry();
}

bool RemoveSuffix(char* text, const char* suffix) {
    if (!EndsWith(text, suffix)) return false;
    text[std::strlen(text) - std::strlen(suffix)] = '\0';
    return true;
}

void Backspace() {
    if (s_app.after_equals) s_app.after_equals = false;
    s_app.error = false;
    constexpr const char* kTokens[] = {"reciprocal(", "square(", "sqrt(", "sin(", "cos(", "tan(", "log(", "ln(", "mod", "pi"};
    for (const char* token : kTokens) {
        if (RemoveSuffix(s_app.expression, token)) { RenderEntry(); return; }
    }
    const size_t length = std::strlen(s_app.expression);
    if (length != 0) s_app.expression[length - 1] = '\0';
    RenderEntry();
}

void InputOperator(const char* token) {
    if (s_app.error) ResetEntry(false);
    if (s_app.after_equals) s_app.after_equals = false;
    if (s_app.expression[0] == '\0') {
        if (std::strcmp(token, "-") == 0) Append(s_app.expression, sizeof(s_app.expression), "-");
        RenderEntry(); return;
    }
    if (!ExpressionEndsValue(s_app.expression)) {
        constexpr const char* kOperators[] = {"mod", "+", "-", "*", "/", "^"};
        for (const char* value : kOperators) if (RemoveSuffix(s_app.expression, value)) break;
    }
    Append(s_app.expression, sizeof(s_app.expression), token);
    RenderEntry();
}

void InputFunction(const char* token) {
    if (s_app.error) ResetEntry(false);
    if (s_app.after_equals) s_app.after_equals = false;
    char next[sizeof(s_app.expression)]{};
    if (ExpressionEndsValue(s_app.expression)) {
        if (std::strlen(token) + std::strlen(s_app.expression) + 2 >= sizeof(next)) return;
        Copy(next, sizeof(next), token);
        Append(next, sizeof(next), "(");
        Append(next, sizeof(next), s_app.expression);
        Append(next, sizeof(next), ")");
        Copy(s_app.expression, sizeof(s_app.expression), next);
    } else {
        Append(s_app.expression, sizeof(s_app.expression), token);
        Append(s_app.expression, sizeof(s_app.expression), "(");
    }
    RenderEntry();
}

void InputConstant(const char* token) {
    BeginFreshEntryIfNeeded();
    if (ExpressionEndsValue(s_app.expression)) Append(s_app.expression, sizeof(s_app.expression), "*");
    Append(s_app.expression, sizeof(s_app.expression), token);
    RenderEntry();
}

void InputParenthesis(bool open) {
    BeginFreshEntryIfNeeded();
    if (open) {
        if (ExpressionEndsValue(s_app.expression)) Append(s_app.expression, sizeof(s_app.expression), "*");
        Append(s_app.expression, sizeof(s_app.expression), "(");
    } else if (ParenthesisBalance(s_app.expression) > 0 && ExpressionEndsValue(s_app.expression)) {
        Append(s_app.expression, sizeof(s_app.expression), ")");
    }
    RenderEntry();
}

void ToggleSign() {
    if (s_app.error) return;
    if (s_app.after_equals) s_app.after_equals = false;
    if (s_app.expression[0] == '\0') Append(s_app.expression, sizeof(s_app.expression), "-");
    else {
        char next[sizeof(s_app.expression)]{};
        if (std::strlen(s_app.expression) + 3 >= sizeof(next)) return;
        Copy(next, sizeof(next), "-(");
        Append(next, sizeof(next), s_app.expression);
        Append(next, sizeof(next), ")");
        Copy(s_app.expression, sizeof(s_app.expression), next);
    }
    RenderEntry();
}

void ApplyPercent() {
    if (s_app.error) return;
    if (s_app.after_equals) s_app.after_equals = false;
    if (ExpressionEndsValue(s_app.expression)) {
        Append(s_app.expression, sizeof(s_app.expression), "%");
        RenderEntry();
    }
}

void Evaluate() {
    if (s_app.expression[0] == '\0') return;
    char source[sizeof(s_app.expression)];
    Copy(source, sizeof(source), s_app.expression);
    double result = 0.0;
    ScientificParser parser(source);
    if (!parser.Parse(&result)) {
        Copy(s_app.history, sizeof(s_app.history), ParenthesisBalance(source) != 0 ? "表达式不完整" : "无法计算");
        s_app.expression[0] = '\0';
        s_app.error = true;
        s_app.after_equals = true;
        RenderEntry(); return;
    }
    char formatted_source[128];
    FormatExpression(source, formatted_source, sizeof(formatted_source));
    std::snprintf(s_app.history, sizeof(s_app.history), "%s =", formatted_source);
    FormatNumber(result, s_app.expression, sizeof(s_app.expression));
    s_app.error = false;
    s_app.after_equals = true;
    RenderEntry();
}

uint32_t KeyBackground(const KeyDefinition& key) {
    if (key.style == KeyStyle::Equals) return s_app.theme.accent;
    if (key.style == KeyStyle::Utility || key.style == KeyStyle::Scientific) return s_app.theme.raised;
    return s_app.theme.surface;
}
uint32_t KeyForeground(const KeyDefinition& key) {
    if (key.style == KeyStyle::Equals) return s_app.theme.accent_ink;
    if (key.style == KeyStyle::Operator) return s_app.theme.accent;
    if (key.style == KeyStyle::Utility) return s_app.theme.muted;
    return s_app.theme.text;
}
uint32_t KeyBorder(const KeyDefinition& key) {
    return key.style == KeyStyle::Equals || key.style == KeyStyle::Operator ? s_app.theme.accent : s_app.theme.border;
}

template <size_t Count>
void ApplyKeyTheme(const KeyDefinition (&keys)[Count], metalio_app_widget_t (&buttons)[Count]) {
    for (size_t index = 0; index < Count; ++index) {
        s_app.api->set_button_colors(s_app.launch->host_context, buttons[index],
            KeyBackground(keys[index]), s_app.theme.accent_pressed, KeyForeground(keys[index]));
        s_app.api->set_button_border(s_app.launch->host_context, buttons[index], KeyBorder(keys[index]), 1, 15);
    }
}

void ApplyTheme(const metalio_app_theme_t& theme) {
    s_app.theme = theme;
    s_app.api->set_background(s_app.launch->host_context, theme.background);
    s_app.api->set_rect_color(s_app.launch->host_context, s_app.display, theme.surface);
    s_app.api->set_rect_border(s_app.launch->host_context, s_app.display, theme.border, 1);
    s_app.api->set_label_color(s_app.launch->host_context, s_app.history_label, theme.muted);
    s_app.api->set_label_color(s_app.launch->host_context, s_app.result_label, s_app.error ? theme.danger : theme.text);
    ApplyKeyTheme(kStandardKeys, s_app.standard_buttons);
    ApplyKeyTheme(kScientificKeys, s_app.scientific_buttons);
}
void OnThemeChanged(void*, const metalio_app_theme_t* theme) { if (theme != nullptr) ApplyTheme(*theme); }

void SetMode(CalculatorMode mode) {
    s_app.mode = mode;
    const bool standard = mode == CalculatorMode::Standard;
    s_app.api->set_widget_visible(s_app.launch->host_context, s_app.standard_grid, standard);
    s_app.api->set_widget_visible(s_app.launch->host_context, s_app.scientific_grid, !standard);
    s_app.api->set_action_segment_selected(s_app.launch->host_context, s_app.mode_segment, standard ? 0 : 1);
}
void OnModeSelected(void*, uint32_t index) {
    Haptic(METALIO_APP_HAPTIC_CLICK);
    SetMode(index == 0 ? CalculatorMode::Standard : CalculatorMode::Scientific);
}

void OnKey(void* context) {
    const auto* key = static_cast<const KeyDefinition*>(context);
    if (key == nullptr) return;
    Haptic(key->action == KeyAction::Equals ? METALIO_APP_HAPTIC_CLICK : METALIO_APP_HAPTIC_TICK);
    switch (key->action) {
        case KeyAction::Digit: InputDigit(key->token); break;
        case KeyAction::Decimal: InputDecimal(); break;
        case KeyAction::Clear: ResetEntry(true); break;
        case KeyAction::Backspace: Backspace(); break;
        case KeyAction::Sign: ToggleSign(); break;
        case KeyAction::Percent: ApplyPercent(); break;
        case KeyAction::Operator: InputOperator(key->token); break;
        case KeyAction::Equals: Evaluate(); break;
        case KeyAction::Function: InputFunction(key->token); break;
        case KeyAction::Constant: InputConstant(key->token); break;
        case KeyAction::OpenParenthesis: InputParenthesis(true); break;
        case KeyAction::CloseParenthesis: InputParenthesis(false); break;
    }
}

template <size_t Count>
bool BuildKeypad(metalio_app_widget_t grid, const KeyDefinition (&keys)[Count],
                 metalio_app_widget_t (&buttons)[Count], metalio_app_font_t font) {
    for (size_t index = 0; index < Count; ++index) {
        const KeyDefinition& key = keys[index];
        if (s_app.api->grid_add_button(s_app.launch->host_context, grid, key.column, key.row,
                key.column_span, 1, key.label, KeyBackground(key), KeyForeground(key), font,
                OnKey, const_cast<KeyDefinition*>(&key), &buttons[index]) != 0) return false;
        s_app.api->set_button_border(s_app.launch->host_context, buttons[index], KeyBorder(key), 1, 15);
    }
    return true;
}

}  // namespace

extern "C" int main(int argc, char* argv[]) {
    if (argc != 2 || argv == nullptr || argv[0] == nullptr || argv[1] == nullptr) return 1;
    s_app.api = reinterpret_cast<const metalio_app_host_api_t*>(argv[0]);
    s_app.launch = reinterpret_cast<const metalio_app_launch_context_t*>(argv[1]);
    if (s_app.api->abi_version != METALIO_APP_ABI_VERSION || !HasControlApi(s_app.api) ||
        s_app.launch->abi_version != METALIO_APP_ABI_VERSION ||
        s_app.launch->struct_size < sizeof(metalio_app_launch_context_t)) return 2;
    s_app.capabilities = s_app.api->get_capabilities(s_app.launch->host_context);
    const metalio_app_capabilities_t required = METALIO_APP_CAP_UI_BUTTONS |
        METALIO_APP_CAP_UI_GRID | METALIO_APP_CAP_UI_DRAW |
        METALIO_APP_CAP_UI_THEME | METALIO_APP_CAP_UI_CONTROLS;
    if ((s_app.capabilities & required) != required) return 3;
    if (s_app.api->get_theme(s_app.launch->host_context, &s_app.theme) != 0) return 3;

    s_app.api->set_background(s_app.launch->host_context, s_app.theme.background);
    if (s_app.api->add_rect(s_app.launch->host_context, 30, 14, 660, 94,
            s_app.theme.surface, 18, &s_app.display) != 0 ||
        s_app.api->set_rect_border(s_app.launch->host_context, s_app.display, s_app.theme.border, 1) != 0 ||
        s_app.api->add_label_ex(s_app.launch->host_context, "", 52, 24, 616, 24,
            s_app.theme.muted, METALIO_APP_FONT_SMALL_BOLD, &s_app.history_label) != 0 ||
        s_app.api->add_label_ex(s_app.launch->host_context, "0", 52, 47, 616, 54,
            s_app.theme.text, METALIO_APP_FONT_LARGE, &s_app.result_label) != 0 ||
        s_app.api->set_label_alignment(s_app.launch->host_context, s_app.history_label, METALIO_APP_TEXT_ALIGN_RIGHT) != 0 ||
        s_app.api->set_label_alignment(s_app.launch->host_context, s_app.result_label, METALIO_APP_TEXT_ALIGN_RIGHT) != 0 ||
        s_app.api->add_grid(s_app.launch->host_context, 30, 122, 660, 392, 4, 5, 8, 8, &s_app.standard_grid) != 0 ||
        s_app.api->add_grid(s_app.launch->host_context, 30, 122, 660, 392, 7, 5, 8, 8, &s_app.scientific_grid) != 0) return 4;
    if (!BuildKeypad(s_app.standard_grid, kStandardKeys, s_app.standard_buttons, METALIO_APP_FONT_MEDIUM_BOLD) ||
        !BuildKeypad(s_app.scientific_grid, kScientificKeys, s_app.scientific_buttons, METALIO_APP_FONT_SMALL_BOLD)) return 5;
    const char* modes[] = {"标准", "科学"};
    if (s_app.api->add_action_segment(s_app.launch->host_context, modes, 2, 0,
            OnModeSelected, nullptr, &s_app.mode_segment) != 0 ||
        s_app.api->set_widget_visible(s_app.launch->host_context, s_app.scientific_grid, 0) != 0 ||
        s_app.api->set_theme_callback(s_app.launch->host_context, OnThemeChanged, nullptr) != 0) return 6;
    ResetEntry(true);
    return 0;
}
