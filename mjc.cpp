// mjc.cpp —— mini-JVM 编译器(相当于 javac)
//
// 把一门"类 Java 的小语言"(.mj)编译成 mini-JVM 的文本字节码(.mjvm)。
// 至此整条链路凑齐:
//     源码(.mj) --[mjc]--> 字节码(.mjvm) --[类加载/链接]--> 解释执行 + GC
//
// 编译器分三步(和真实编译器一致):
//     1) 词法分析(Lexer)   源码字符流 -> 记号(token)流
//     2) 语法分析(Parser)  记号流 -> 抽象语法树(AST)
//     3) 代码生成(CodeGen) 遍历 AST -> 目标字节码
//
// 支持的语言特性:
//     class Name { field; ... }        类与字段
//     func name(a, b) { ... }          函数(编译成静态方法)
//     var x = expr;  /  x = expr;       局部变量
//     if (cond) {..} else {..}          分支
//     while (cond) {..}                 循环
//     return expr;  return;             返回
//     print(expr);   gc();              内建
//     new Name()      obj.field         对象与字段访问
//     + - * /   < > <= >= == !=         运算符
//
// 用法:  mjc <源文件.mj> [输出.mjvm]     (默认输出 out.mjvm)

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <stdexcept>
#include <cctype>

// ============================================================
// 1. 词法分析(Lexer):字符流 -> 记号流
// ============================================================
struct Token {
    enum Kind { INT, IDENT, PUNCT, END } kind;
    std::string text;   // 原文(标识符/符号)
    int         value;  // INT 时的数值
    int         line;
};

class Lexer {
    const std::string& src;
    size_t pos = 0;
    int    line = 1;
public:
    explicit Lexer(const std::string& s) : src(s) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        while (true) {
            skipTrivia();
            if (pos >= src.size()) { out.push_back({Token::END, "<eof>", 0, line}); break; }
            char c = src[pos];
            if (std::isdigit((unsigned char)c)) {
                int start = (int)pos;
                while (pos < src.size() && std::isdigit((unsigned char)src[pos])) pos++;
                std::string num = src.substr(start, pos - start);
                out.push_back({Token::INT, num, std::stoi(num), line});
            } else if (std::isalpha((unsigned char)c) || c == '_') {
                int start = (int)pos;
                while (pos < src.size() && (std::isalnum((unsigned char)src[pos]) || src[pos] == '_')) pos++;
                out.push_back({Token::IDENT, src.substr(start, pos - start), 0, line});
            } else {
                // 双字符运算符:<= >= == !=
                std::string two = src.substr(pos, 2);
                if (two == "<=" || two == ">=" || two == "==" || two == "!=") {
                    out.push_back({Token::PUNCT, two, 0, line}); pos += 2;
                } else {
                    out.push_back({Token::PUNCT, std::string(1, c), 0, line}); pos += 1;
                }
            }
        }
        return out;
    }
private:
    void skipTrivia() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == '\n') { line++; pos++; }
            else if (std::isspace((unsigned char)c)) pos++;
            else if (c == '/' && pos + 1 < src.size() && src[pos+1] == '/') {  // // 行注释
                while (pos < src.size() && src[pos] != '\n') pos++;
            } else break;
        }
    }
};

// ============================================================
// 2. 抽象语法树(AST)节点定义
// ============================================================
struct Expr {
    enum Kind { INT_LIT, VAR, BINARY, CALL, NEW, FIELD } kind;
    int         intVal = 0;                 // INT_LIT
    std::string name;                        // VAR / CALL(函数名) / NEW(类名) / FIELD(对象变量名)
    std::string op;                          // BINARY 运算符 或 FIELD 的字段名
    std::unique_ptr<Expr>              lhs, rhs;   // BINARY
    std::vector<std::unique_ptr<Expr>> args;       // CALL 实参
};

struct Stmt {
    enum Kind { VAR_DECL, ASSIGN, FIELD_ASSIGN, IF, WHILE, RETURN, PRINT, EXPR, GC } kind;
    std::string           name;              // VAR_DECL/ASSIGN:变量名;FIELD_ASSIGN:对象变量名
    std::string           field;             // FIELD_ASSIGN:字段名
    std::unique_ptr<Expr> expr;              // 初值 / 右值 / 条件 / 返回值 / 打印值
    std::vector<std::unique_ptr<Stmt>> body;     // IF 的 then / WHILE 的循环体
    std::vector<std::unique_ptr<Stmt>> elseBody;  // IF 的 else
    bool hasExpr = false;                    // RETURN 是否带返回值
};

struct Func {
    std::string              name;
    std::vector<std::string> params;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct ClassDecl {
    std::string              name;
    std::vector<std::string> fields;
};

// ============================================================
// 2b. 语法分析(Parser):记号流 -> AST(递归下降)
// ============================================================
class Parser {
    std::vector<Token> toks;
    size_t p = 0;

    const Token& peek() const { return toks[p]; }
    const Token& next()       { return toks[p++]; }
    bool isPunct(const std::string& s) const { return peek().kind == Token::PUNCT && peek().text == s; }
    bool isKw(const std::string& s) const { return peek().kind == Token::IDENT && peek().text == s; }

    void expectPunct(const std::string& s) {
        if (!isPunct(s)) err("期望 '" + s + "'");
        next();
    }
    std::string expectIdent() {
        if (peek().kind != Token::IDENT) err("期望标识符");
        return next().text;
    }
    [[noreturn]] void err(const std::string& msg) {
        throw std::runtime_error("语法错误(行 " + std::to_string(peek().line) + "):" + msg
                                 + ",实际读到 '" + peek().text + "'");
    }
public:
    explicit Parser(std::vector<Token> t) : toks(std::move(t)) {}

    void parse(std::vector<ClassDecl>& classes, std::vector<Func>& funcs) {
        while (peek().kind != Token::END) {
            if (isKw("class"))     classes.push_back(parseClass());
            else if (isKw("func")) funcs.push_back(parseFunc());
            else err("顶层只能是 class 或 func");
        }
    }
private:
    ClassDecl parseClass() {
        next();                                  // 'class'
        ClassDecl c; c.name = expectIdent();
        expectPunct("{");
        while (!isPunct("}")) { c.fields.push_back(expectIdent()); expectPunct(";"); }
        expectPunct("}");
        return c;
    }

    Func parseFunc() {
        next();                                  // 'func'
        Func f; f.name = expectIdent();
        expectPunct("(");
        if (!isPunct(")")) {
            f.params.push_back(expectIdent());
            while (isPunct(",")) { next(); f.params.push_back(expectIdent()); }
        }
        expectPunct(")");
        f.body = parseBlock();
        return f;
    }

    std::vector<std::unique_ptr<Stmt>> parseBlock() {
        expectPunct("{");
        std::vector<std::unique_ptr<Stmt>> stmts;
        while (!isPunct("}")) stmts.push_back(parseStmt());
        expectPunct("}");
        return stmts;
    }

    std::unique_ptr<Stmt> parseStmt() {
        if (isKw("var"))    return parseVarDecl();
        if (isKw("if"))     return parseIf();
        if (isKw("while"))  return parseWhile();
        if (isKw("return")) return parseReturn();
        if (isKw("print"))  return parsePrint();
        if (isKw("gc"))     return parseGc();
        return parseAssignOrExpr();
    }

    std::unique_ptr<Stmt> parseVarDecl() {
        next();                                  // 'var'
        auto s = std::make_unique<Stmt>(); s->kind = Stmt::VAR_DECL;
        s->name = expectIdent();
        expectPunct("=");
        s->expr = parseExpr();
        expectPunct(";");
        return s;
    }

    std::unique_ptr<Stmt> parseIf() {
        next(); expectPunct("(");
        auto s = std::make_unique<Stmt>(); s->kind = Stmt::IF;
        s->expr = parseExpr();
        expectPunct(")");
        s->body = parseBlock();
        if (isKw("else")) { next(); s->elseBody = parseBlock(); }
        return s;
    }

    std::unique_ptr<Stmt> parseWhile() {
        next(); expectPunct("(");
        auto s = std::make_unique<Stmt>(); s->kind = Stmt::WHILE;
        s->expr = parseExpr();
        expectPunct(")");
        s->body = parseBlock();
        return s;
    }

    std::unique_ptr<Stmt> parseReturn() {
        next();
        auto s = std::make_unique<Stmt>(); s->kind = Stmt::RETURN;
        if (!isPunct(";")) { s->expr = parseExpr(); s->hasExpr = true; }
        expectPunct(";");
        return s;
    }

    std::unique_ptr<Stmt> parsePrint() {
        next(); expectPunct("(");
        auto s = std::make_unique<Stmt>(); s->kind = Stmt::PRINT;
        s->expr = parseExpr();
        expectPunct(")"); expectPunct(";");
        return s;
    }

    std::unique_ptr<Stmt> parseGc() {
        next(); expectPunct("("); expectPunct(")"); expectPunct(";");
        auto s = std::make_unique<Stmt>(); s->kind = Stmt::GC;
        return s;
    }

    // 赋值 或 表达式语句。先看 IDENT,再看后面是 '=' / '.f =' / 其它
    std::unique_ptr<Stmt> parseAssignOrExpr() {
        if (peek().kind == Token::IDENT) {
            // 前瞻:IDENT = ...   或   IDENT . field = ...
            std::string id = peek().text;
            if (toks[p+1].kind == Token::PUNCT && toks[p+1].text == "=") {
                next(); next();                  // IDENT '='
                auto s = std::make_unique<Stmt>(); s->kind = Stmt::ASSIGN;
                s->name = id; s->expr = parseExpr(); expectPunct(";");
                return s;
            }
            if (toks[p+1].kind == Token::PUNCT && toks[p+1].text == "." &&
                toks[p+3].kind == Token::PUNCT && toks[p+3].text == "=") {
                next();                          // IDENT
                expectPunct(".");
                std::string fld = expectIdent();
                expectPunct("=");
                auto s = std::make_unique<Stmt>(); s->kind = Stmt::FIELD_ASSIGN;
                s->name = id; s->field = fld; s->expr = parseExpr(); expectPunct(";");
                return s;
            }
        }
        // 否则当作表达式语句(如函数调用)
        auto s = std::make_unique<Stmt>(); s->kind = Stmt::EXPR;
        s->expr = parseExpr(); expectPunct(";");
        return s;
    }

    // ---- 表达式:比较 < 加减 < 乘除 < 基本单元 ----
    std::unique_ptr<Expr> parseExpr() { return parseComparison(); }

    std::unique_ptr<Expr> parseComparison() {
        auto l = parseAdditive();
        if (isPunct("<") || isPunct(">") || isPunct("<=") ||
            isPunct(">=") || isPunct("==") || isPunct("!=")) {
            std::string op = next().text;
            auto e = std::make_unique<Expr>(); e->kind = Expr::BINARY;
            e->op = op; e->lhs = std::move(l); e->rhs = parseAdditive();
            return e;
        }
        return l;
    }

    std::unique_ptr<Expr> parseAdditive() {
        auto l = parseTerm();
        while (isPunct("+") || isPunct("-")) {
            std::string op = next().text;
            auto e = std::make_unique<Expr>(); e->kind = Expr::BINARY;
            e->op = op; e->lhs = std::move(l); e->rhs = parseTerm();
            l = std::move(e);
        }
        return l;
    }

    std::unique_ptr<Expr> parseTerm() {
        auto l = parseFactor();
        while (isPunct("*") || isPunct("/")) {
            std::string op = next().text;
            auto e = std::make_unique<Expr>(); e->kind = Expr::BINARY;
            e->op = op; e->lhs = std::move(l); e->rhs = parseFactor();
            l = std::move(e);
        }
        return l;
    }

    std::unique_ptr<Expr> parseFactor() {
        if (peek().kind == Token::INT) {
            auto e = std::make_unique<Expr>(); e->kind = Expr::INT_LIT; e->intVal = next().value;
            return e;
        }
        if (isPunct("(")) { next(); auto e = parseExpr(); expectPunct(")"); return e; }
        if (isKw("new")) {
            next();
            auto e = std::make_unique<Expr>(); e->kind = Expr::NEW; e->name = expectIdent();
            expectPunct("("); expectPunct(")");
            return e;
        }
        if (peek().kind == Token::IDENT) {
            std::string id = next().text;
            if (isPunct("(")) {                  // 函数调用
                next();
                auto e = std::make_unique<Expr>(); e->kind = Expr::CALL; e->name = id;
                if (!isPunct(")")) {
                    e->args.push_back(parseExpr());
                    while (isPunct(",")) { next(); e->args.push_back(parseExpr()); }
                }
                expectPunct(")");
                return e;
            }
            if (isPunct(".")) {                  // 字段读取 obj.field
                next();
                auto e = std::make_unique<Expr>(); e->kind = Expr::FIELD;
                e->name = id; e->op = expectIdent();
                return e;
            }
            auto e = std::make_unique<Expr>(); e->kind = Expr::VAR; e->name = id;  // 变量
            return e;
        }
        err("无法解析的表达式");
    }
};

// ============================================================
// 3. 代码生成(CodeGen):AST -> .mjvm 文本字节码
// ============================================================
class CodeGen {
    std::ostringstream out;
    std::unordered_map<std::string, std::string> fieldOwner;  // 字段名 -> 所属类名
    // 当前函数的局部变量表:变量名 -> 槽位
    std::unordered_map<std::string, int> locals;
    int nextSlot = 0;
    int labelId  = 0;

    std::string newLabel(const std::string& hint) { return "L" + hint + std::to_string(labelId++); }

    int slotOf(const std::string& name) {
        auto it = locals.find(name);
        if (it != locals.end()) return it->second;
        int s = nextSlot++;                       // 首次出现即分配槽位
        locals[name] = s;
        return s;
    }
    void emit(const std::string& s)  { out << "    " << s << "\n"; }
    void label(const std::string& l) { out << l << ":\n"; }

    // 比较运算符 -> 取反后的条件跳转助记符(javac 风格:条件为假就跳过)
    static std::string negatedBranch(const std::string& op) {
        if (op == "<")  return "if_icmpge";
        if (op == ">")  return "if_icmple";
        if (op == "<=") return "if_icmpgt";
        if (op == ">=") return "if_icmplt";
        if (op == "==") return "if_icmpne";
        if (op == "!=") return "if_icmpeq";
        throw std::runtime_error("非比较运算符:" + op);
    }
    static bool isComparison(const std::string& op) {
        return op=="<"||op==">"||op=="<="||op==">="||op=="=="||op=="!=";
    }

public:
    std::string generate(const std::vector<ClassDecl>& classes, const std::vector<Func>& funcs) {
        // 记录字段归属(编译 obj.field 时用来确定 putfield/getfield 的类名)
        for (const auto& c : classes)
            for (const auto& f : c.fields) {
                if (fieldOwner.count(f))
                    throw std::runtime_error("字段名 '" + f + "' 在多个类中重复,本迷你语言要求字段名全局唯一");
                fieldOwner[f] = c.name;
            }

        out << "; 由 mjc 自动生成 —— 请勿手改\n\n";
        for (const auto& c : classes) {
            out << ".class " << c.name << "\n";
            for (const auto& f : c.fields) out << "  .field " << f << "\n";
            out << ".end\n\n";
        }
        for (const auto& f : funcs) genFunc(f);
        return out.str();
    }

private:
    void genFunc(const Func& f) {
        locals.clear(); nextSlot = 0;
        for (const auto& p : f.params) slotOf(p);   // 参数占据前几个槽
        int argCount = (int)f.params.size();

        // 方法头暂用占位,函数体生成完才知道 maxLocals —— 先把体写到临时流
        std::ostringstream saved; saved.swap(out);
        for (const auto& s : f.body) genStmt(*s);
        emit("iconst 0");                            // 兜底返回(无显式 return 时)
        emit("ireturn");
        std::string bodyText = out.str();
        saved.swap(out);

        out << ".method " << f.name << " args=" << argCount
            << " locals=" << nextSlot << "\n";
        out << bodyText;
        out << ".end\n\n";
    }

    void genStmt(const Stmt& s) {
        switch (s.kind) {
        case Stmt::VAR_DECL:
        case Stmt::ASSIGN: {
            genExpr(*s.expr);
            emit("istore " + std::to_string(slotOf(s.name)));
        } break;
        case Stmt::FIELD_ASSIGN: {
            emit("iload " + std::to_string(slotOf(s.name)));   // 压对象引用
            genExpr(*s.expr);                                  // 压值
            emit("putfield " + ownerOf(s.field) + "." + s.field);
        } break;
        case Stmt::IF: {
            std::string elseL = newLabel("else");
            std::string endL  = newLabel("endif");
            genCondition(*s.expr, elseL);          // 条件为假 -> 跳 else
            for (const auto& st : s.body) genStmt(*st);
            if (!s.elseBody.empty()) {
                emit("goto " + endL);
                label(elseL);
                for (const auto& st : s.elseBody) genStmt(*st);
                label(endL);
            } else {
                label(elseL);                      // 无 else:假分支直接落到 if 之后
            }
        } break;
        case Stmt::WHILE: {
            std::string startL = newLabel("while");
            std::string endL   = newLabel("endwhile");
            label(startL);
            genCondition(*s.expr, endL);           // 条件为假 -> 跳出
            for (const auto& st : s.body) genStmt(*st);
            emit("goto " + startL);
            label(endL);
        } break;
        case Stmt::RETURN: {
            if (s.hasExpr) { genExpr(*s.expr); emit("ireturn"); }
            else           { emit("iconst 0"); emit("ireturn"); }
        } break;
        case Stmt::PRINT: {
            genExpr(*s.expr);
            emit("print");                         // print 只看栈顶,不弹
            emit("pop");                           // 手动弹掉
        } break;
        case Stmt::EXPR: {
            genExpr(*s.expr);
            emit("pop");                           // 表达式语句丢弃结果
        } break;
        case Stmt::GC: emit("gc"); break;
        }
    }

    // 生成"条件为假就跳到 falseLabel"的代码(javac 风格取反跳转)
    void genCondition(const Expr& e, const std::string& falseLabel) {
        if (e.kind == Expr::BINARY && isComparison(e.op)) {
            genExpr(*e.lhs);
            genExpr(*e.rhs);
            emit(negatedBranch(e.op) + " " + falseLabel);
        } else {
            genExpr(e);                            // 非比较:值为 0 视为假
            emit("iconst 0");
            emit("if_icmpeq " + falseLabel);
        }
    }

    void genExpr(const Expr& e) {
        switch (e.kind) {
        case Expr::INT_LIT: emit("iconst " + std::to_string(e.intVal)); break;
        case Expr::VAR:     emit("iload " + std::to_string(slotOf(e.name))); break;
        case Expr::NEW:     emit("new " + e.name); break;
        case Expr::FIELD:
            emit("iload " + std::to_string(slotOf(e.name)));
            emit("getfield " + ownerOf(e.op) + "." + e.op);
            break;
        case Expr::CALL: {
            for (const auto& a : e.args) genExpr(*a);      // 实参依次压栈
            emit("invokestatic " + e.name);
        } break;
        case Expr::BINARY: {
            if (isComparison(e.op)) {                      // 比较作为值:物化成 0/1
                std::string t = newLabel("true"), end = newLabel("cend");
                genExpr(*e.lhs); genExpr(*e.rhs);
                // 条件成立跳 true(用正向分支)
                std::string br =
                    e.op=="<"?"if_icmplt": e.op==">"?"if_icmpgt": e.op=="<="?"if_icmple":
                    e.op==">="?"if_icmpge": e.op=="=="?"if_icmpeq":"if_icmpne";
                emit(br + " " + t);
                emit("iconst 0"); emit("goto " + end);
                label(t); emit("iconst 1");
                label(end);
            } else {
                genExpr(*e.lhs); genExpr(*e.rhs);
                if (e.op == "+") emit("iadd");
                else if (e.op == "-") emit("isub");
                else if (e.op == "*") emit("imul");
                else if (e.op == "/") emit("idiv");
                else throw std::runtime_error("未知运算符:" + e.op);
            }
        } break;
        }
    }

    std::string ownerOf(const std::string& field) {
        auto it = fieldOwner.find(field);
        if (it == fieldOwner.end())
            throw std::runtime_error("未知字段 '" + field + "'(没有类声明它)");
        return it->second;
    }
};

// ============================================================
int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "用法: mjc <源文件.mj> [输出.mjvm]\n"; return 1; }
    std::string inPath  = argv[1];
    std::string outPath = (argc > 2) ? argv[2] : "out.mjvm";

    try {
        std::ifstream fin(inPath);
        if (!fin) throw std::runtime_error("打不开源文件:" + inPath);
        std::stringstream buf; buf << fin.rdbuf();
        std::string src = buf.str();

        Lexer lexer(src);
        Parser parser(lexer.tokenize());
        std::vector<ClassDecl> classes;
        std::vector<Func>      funcs;
        parser.parse(classes, funcs);

        CodeGen cg;
        std::string bytecode = cg.generate(classes, funcs);

        std::ofstream fout(outPath);
        fout << bytecode;
        std::cout << "编译成功:" << inPath << " -> " << outPath
                  << "(" << classes.size() << " 个类," << funcs.size() << " 个函数)\n";
    } catch (const std::exception& e) {
        std::cerr << "[编译错误] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
