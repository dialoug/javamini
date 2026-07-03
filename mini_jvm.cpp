// mini_jvm.cpp —— 一个"简易易读"的 Java 虚拟机架构演示
//
// 分层:
//   1) 字节码指令集         —— 平台无关的中间指令
//   2) 类加载子系统          —— 解析文本"class 文件" + 常量池 + 链接(符号解析)
//   3) 方法区 / 运行时数据区  —— 类/方法元数据 + 堆 + 虚拟机栈(栈帧) + PC
//   4) 执行引擎              —— 基于操作数栈的解释器循环
//   5) 垃圾回收              —— 标记-清除(精确 GC)
//
// 本步骤(方向 2)重点:常量池 + 从文本文件加载 + "符号引用 -> 直接引用"的解析。
//   * .class 里不写"方法下标 0",而写符号"factorial";这些符号进【常量池】。
//   * 加载后有一个【链接】阶段:把常量池里的符号解析成直接引用(方法/类/字段下标)。
//     这正是真实 JVM 类加载"验证->准备->解析"里的"解析"。
//
// 编译:见 CMakeLists.txt。运行:mini_jvm [class文件路径]  (默认 program.mjvm)

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

// ============================================================
// 0. 带类型标签的值(精确 GC 的前提:区分 INT 与 REF)
// ============================================================
struct Value {
    enum Tag { INT, REF } tag = INT;
    int i = 0;                          // INT:数值;REF:堆下标
    static Value Int(int v)   { return Value{INT, v}; }
    static Value Ref(int idx) { return Value{REF, idx}; }
};

// ============================================================
// 1. 字节码指令集
// ============================================================
enum class Op {
    ICONST, ILOAD, ISTORE,
    IADD, ISUB, IMUL, IDIV,
    // 条件跳转一族(对应真实 JVM 的 if_icmp{eq,ne,lt,ge,gt,le}):
    // 弹出 b、a(a 先入栈),按比较关系成立则跳转到 arg 指定地址
    IF_ICMPEQ, IF_ICMPNE, IF_ICMPLT, IF_ICMPGE, IF_ICMPGT, IF_ICMPLE,
    GOTO,
    INVOKESTATIC,
    IRETURN, RETURN,
    NEW, GETFIELD, PUTFIELD,
    PRINT, POP, DUP, GC,
};

// 一条指令。
//   arg      : 立即数(iconst/iload/istore)或跳转目标(链接后填入)。
//   cpIndex  : 符号引用指令(invokestatic/new/get/putfield)指向常量池的下标。
//   resolved : 链接阶段把 cpIndex 解析出来的【直接引用】(方法/类/字段下标)。
struct Instr {
    Op  op;
    int arg      = 0;
    int cpIndex  = -1;   // -1 表示该指令没有符号引用
    int resolved = -1;
};

// ============================================================
// 2. 常量池(Constant Pool)
//    真实 .class 每个类有一个常量池,存符号:类名、方法名、字段的"类.字段"等。
//    这里用一个全局常量池,类型化地存三类符号引用。
// ============================================================
struct Const {
    enum Kind { CLASS_REF, METHOD_REF, FIELD_REF } kind;
    std::string a;          // 类名 / 方法名 / 字段的类名
    std::string b;          // FIELD_REF 时:字段名
};

// ============================================================
// 方法区元数据
// ============================================================
struct Method {
    std::string        name;
    int                argCount  = 0;
    int                maxLocals = 0;
    std::vector<Instr> code;
};
struct Field { std::string name; };
struct Klass {
    std::string        name;
    std::vector<Field> fields;
    int fieldIndex(const std::string& n) const {
        for (int i = 0; i < (int)fields.size(); ++i) if (fields[i].name == n) return i;
        return -1;
    }
};

// ============================================================
// 堆对象 / 栈帧
// ============================================================
struct Object {
    Klass*             klass  = nullptr;
    std::vector<Value> fields;
    bool               alive  = false;
    bool               marked = false;
};
struct Frame {
    Method*            method;
    std::vector<Value> locals;
    std::vector<Value> operandStack;
    int                pc = 0;
    void  push(Value v) { operandStack.push_back(v); }
    Value pop()         { Value v = operandStack.back(); operandStack.pop_back(); return v; }
    Value top()         { return operandStack.back(); }
};

// ============================================================
// 虚拟机
// ============================================================
class VM {
public:
    std::vector<Const>  constPool;   // 常量池:符号引用
    std::vector<Method> methods;     // 方法区
    std::vector<Klass>  klasses;     // 方法区
    std::vector<Object> heap;        // 堆
    std::vector<Frame>  stack;       // 虚拟机栈
    std::vector<int>    freeList;    // 回收后可复用的堆槽

    // 查表辅助(链接阶段用)
    int methodIndex(const std::string& n) const {
        for (int i = 0; i < (int)methods.size(); ++i) if (methods[i].name == n) return i;
        return -1;
    }
    int classIndex(const std::string& n) const {
        for (int i = 0; i < (int)klasses.size(); ++i) if (klasses[i].name == n) return i;
        return -1;
    }

    // ==========================================================
    // 2b. 链接 —— 解析(Resolution):常量池符号引用 -> 直接引用
    //     遍历所有指令,把带 cpIndex 的符号解析成方法/类/字段下标,写入 resolved。
    // ==========================================================
    void link() {
        for (Method& m : methods) {
            for (Instr& in : m.code) {
                if (in.cpIndex < 0) continue;
                const Const& c = constPool[in.cpIndex];
                switch (c.kind) {
                case Const::METHOD_REF: {
                    int idx = methodIndex(c.a);
                    if (idx < 0) throw std::runtime_error("链接失败:未知方法 " + c.a);
                    in.resolved = idx;
                } break;
                case Const::CLASS_REF: {
                    int idx = classIndex(c.a);
                    if (idx < 0) throw std::runtime_error("链接失败:未知类 " + c.a);
                    in.resolved = idx;
                } break;
                case Const::FIELD_REF: {
                    int ci = classIndex(c.a);
                    if (ci < 0) throw std::runtime_error("链接失败:未知类 " + c.a);
                    int fi = klasses[ci].fieldIndex(c.b);
                    if (fi < 0) throw std::runtime_error("链接失败:未知字段 " + c.a + "." + c.b);
                    in.resolved = fi;   // 直接引用 = 字段在对象内的槽位
                } break;
                }
            }
        }
    }

    void dumpConstPool() const {
        std::cout << "[常量池] 共 " << constPool.size() << " 项:\n";
        for (int i = 0; i < (int)constPool.size(); ++i) {
            const Const& c = constPool[i];
            std::cout << "  #" << i << " ";
            if (c.kind == Const::CLASS_REF)  std::cout << "Class  " << c.a;
            if (c.kind == Const::METHOD_REF) std::cout << "Method " << c.a;
            if (c.kind == Const::FIELD_REF)  std::cout << "Field  " << c.a << "." << c.b;
            std::cout << "\n";
        }
    }

    // -------- 堆分配(优先复用空闲槽) --------
    int allocate(int klassIndex) {
        Klass& k = klasses[klassIndex];
        Object obj;
        obj.klass = &k;
        obj.fields.assign(k.fields.size(), Value::Int(0));
        obj.alive = true; obj.marked = false;
        int idx;
        if (!freeList.empty()) { idx = freeList.back(); freeList.pop_back(); heap[idx] = std::move(obj); }
        else { heap.push_back(std::move(obj)); idx = (int)heap.size() - 1; }
        return idx;
    }

    // -------- GC:标记-清除 --------
    void markFrom(int idx) {
        if (idx < 0 || idx >= (int)heap.size()) return;
        Object& o = heap[idx];
        if (!o.alive || o.marked) return;
        o.marked = true;
        for (const Value& fv : o.fields) if (fv.tag == Value::REF) markFrom(fv.i);
    }
    int countLive() const { int n = 0; for (const Object& o : heap) if (o.alive) ++n; return n; }
    void gc() {
        int before = countLive();
        for (Object& o : heap) o.marked = false;
        for (Frame& f : stack) {
            for (const Value& v : f.locals)       if (v.tag == Value::REF) markFrom(v.i);
            for (const Value& v : f.operandStack) if (v.tag == Value::REF) markFrom(v.i);
        }
        int reclaimed = 0;
        for (int i = 0; i < (int)heap.size(); ++i)
            if (heap[i].alive && !heap[i].marked) { heap[i].alive = false; freeList.push_back(i); ++reclaimed; }
        std::cout << "[GC] 回收前存活 " << before << ",回收 " << reclaimed
                  << ",回收后存活 " << countLive() << "(空闲槽 " << freeList.size() << ")\n";
    }

    // -------- 调用 / 执行 --------
    void pushFrame(int methodIndex, const std::vector<Value>& args) {
        Method& m = methods[methodIndex];
        Frame fr; fr.method = &m;
        fr.locals.assign(m.maxLocals, Value::Int(0));
        for (int i = 0; i < m.argCount; ++i) fr.locals[i] = args[i];
        stack.push_back(std::move(fr));
    }

    int execute(int entryMethod, const std::vector<Value>& args) {
        pushFrame(entryMethod, args);
        int returnValue = 0;
        while (!stack.empty()) {
            Frame& f = stack.back();
            Instr in = f.method->code[f.pc];
            f.pc++;
            switch (in.op) {
            case Op::ICONST: f.push(Value::Int(in.arg)); break;
            case Op::ILOAD:  f.push(f.locals[in.arg]); break;
            case Op::ISTORE: f.locals[in.arg] = f.pop(); break;
            case Op::IADD: { int b = f.pop().i, a = f.pop().i; f.push(Value::Int(a + b)); } break;
            case Op::ISUB: { int b = f.pop().i, a = f.pop().i; f.push(Value::Int(a - b)); } break;
            case Op::IMUL: { int b = f.pop().i, a = f.pop().i; f.push(Value::Int(a * b)); } break;
            case Op::IDIV: { int b = f.pop().i, a = f.pop().i;
                if (b == 0) throw std::runtime_error("除零异常(idiv)");
                f.push(Value::Int(a / b)); } break;
            case Op::IF_ICMPEQ: { int b = f.pop().i, a = f.pop().i; if (a == b) f.pc = in.arg; } break;
            case Op::IF_ICMPNE: { int b = f.pop().i, a = f.pop().i; if (a != b) f.pc = in.arg; } break;
            case Op::IF_ICMPLT: { int b = f.pop().i, a = f.pop().i; if (a <  b) f.pc = in.arg; } break;
            case Op::IF_ICMPGE: { int b = f.pop().i, a = f.pop().i; if (a >= b) f.pc = in.arg; } break;
            case Op::IF_ICMPGT: { int b = f.pop().i, a = f.pop().i; if (a >  b) f.pc = in.arg; } break;
            case Op::IF_ICMPLE: { int b = f.pop().i, a = f.pop().i; if (a <= b) f.pc = in.arg; } break;
            case Op::GOTO: f.pc = in.arg; break;
            case Op::INVOKESTATIC: {
                Method& m = methods[in.resolved];       // 用【解析后的直接引用】
                std::vector<Value> callArgs(m.argCount);
                for (int i = m.argCount - 1; i >= 0; --i) callArgs[i] = f.pop();
                pushFrame(in.resolved, callArgs);
            } break;
            case Op::IRETURN: {
                Value rv = f.pop(); stack.pop_back();
                if (stack.empty()) returnValue = rv.i; else stack.back().push(rv);
            } break;
            case Op::RETURN: stack.pop_back(); break;
            case Op::NEW:      f.push(Value::Ref(allocate(in.resolved))); break;
            case Op::GETFIELD: { Value ref = f.pop(); f.push(heap[ref.i].fields[in.resolved]); } break;
            case Op::PUTFIELD: { Value val = f.pop(); Value ref = f.pop(); heap[ref.i].fields[in.resolved] = val; } break;
            case Op::PRINT: std::cout << "[输出] " << f.top().i << "\n"; break;
            case Op::POP:   f.pop(); break;
            case Op::DUP:   f.push(f.top()); break;
            case Op::GC:    gc(); break;
            }
        }
        return returnValue;
    }
};

// ============================================================
// 3. 类加载子系统:解析文本"class 文件" -> 方法区 + 常量池
//    这是把 .mjvm 文本"读进来变成可执行元数据"的过程,对应真实 JVM
//    读取 .class 字节流并填充方法区。符号引用在这里进常量池,link() 再解析。
// ============================================================
class ClassLoader {
    VM& vm;
    // 每个方法:标签名 -> 指令地址(用于把 goto/if 的标签解析成 pc)
    std::unordered_map<std::string, std::unordered_map<std::string,int>> labels;

    // 常量池"驻留":相同符号只存一份,返回其下标
    int internMethod(const std::string& name) {
        for (int i = 0; i < (int)vm.constPool.size(); ++i)
            if (vm.constPool[i].kind == Const::METHOD_REF && vm.constPool[i].a == name) return i;
        vm.constPool.push_back(Const{Const::METHOD_REF, name, ""}); return (int)vm.constPool.size() - 1;
    }
    int internClass(const std::string& name) {
        for (int i = 0; i < (int)vm.constPool.size(); ++i)
            if (vm.constPool[i].kind == Const::CLASS_REF && vm.constPool[i].a == name) return i;
        vm.constPool.push_back(Const{Const::CLASS_REF, name, ""}); return (int)vm.constPool.size() - 1;
    }
    int internField(const std::string& cls, const std::string& fld) {
        for (int i = 0; i < (int)vm.constPool.size(); ++i)
            if (vm.constPool[i].kind == Const::FIELD_REF && vm.constPool[i].a == cls && vm.constPool[i].b == fld) return i;
        vm.constPool.push_back(Const{Const::FIELD_REF, cls, fld}); return (int)vm.constPool.size() - 1;
    }

    static Op opFromMnemonic(const std::string& s) {
        if (s == "iconst")       return Op::ICONST;
        if (s == "iload")        return Op::ILOAD;
        if (s == "istore")       return Op::ISTORE;
        if (s == "iadd")         return Op::IADD;
        if (s == "isub")         return Op::ISUB;
        if (s == "imul")         return Op::IMUL;
        if (s == "idiv")         return Op::IDIV;
        if (s == "if_icmpeq")    return Op::IF_ICMPEQ;
        if (s == "if_icmpne")    return Op::IF_ICMPNE;
        if (s == "if_icmplt")    return Op::IF_ICMPLT;
        if (s == "if_icmpge")    return Op::IF_ICMPGE;
        if (s == "if_icmpgt")    return Op::IF_ICMPGT;
        if (s == "if_icmple")    return Op::IF_ICMPLE;
        if (s == "goto")         return Op::GOTO;
        if (s == "invokestatic") return Op::INVOKESTATIC;
        if (s == "ireturn")      return Op::IRETURN;
        if (s == "return")       return Op::RETURN;
        if (s == "new")          return Op::NEW;
        if (s == "getfield")     return Op::GETFIELD;
        if (s == "putfield")     return Op::PUTFIELD;
        if (s == "print")        return Op::PRINT;
        if (s == "pop")          return Op::POP;
        if (s == "dup")          return Op::DUP;
        if (s == "gc")           return Op::GC;
        throw std::runtime_error("未知指令助记符:" + s);
    }

public:
    explicit ClassLoader(VM& v) : vm(v) {}

    void load(const std::string& path) {
        std::ifstream fin(path);
        if (!fin) throw std::runtime_error("打不开 class 文件:" + path);

        std::string line;
        Method* cur = nullptr;                       // 当前正在解析的方法
        int curIdx = -1;                             // 当前方法在 vm.methods 中的下标
        std::string curName;
        // 待回填的跳转:记录 (方法下标, 方法内指令位置, 标签名);
        // 注意用【下标】而非指针——后续 .method 会让 vm.methods 扩容,指针会失效。
        std::vector<std::tuple<int, int, std::string>> pendingJumps;

        while (std::getline(fin, line)) {
            // 去注释(; 之后)、去首尾空白
            auto semi = line.find(';');
            if (semi != std::string::npos) line = line.substr(0, semi);
            std::stringstream ss(line);
            std::string tok;
            if (!(ss >> tok)) continue;              // 空行

            if (tok == ".class") {
                std::string cname; ss >> cname;
                vm.klasses.push_back(Klass{cname, {}});
            } else if (tok == ".field") {
                std::string fname; ss >> fname;
                vm.klasses.back().fields.push_back(Field{fname});
            } else if (tok == ".method") {
                vm.methods.push_back(Method{});
                curIdx = (int)vm.methods.size() - 1;
                cur = &vm.methods.back();
                ss >> cur->name;
                std::string kv;                      // 解析 args=N locals=N
                while (ss >> kv) {
                    auto eq = kv.find('=');
                    std::string k = kv.substr(0, eq);
                    int v = std::stoi(kv.substr(eq + 1));
                    if (k == "args") cur->argCount = v;
                    if (k == "locals") cur->maxLocals = v;
                }
                curName = cur->name;
            } else if (tok == ".end") {
                cur = nullptr;
            } else if (!tok.empty() && tok.back() == ':') {
                // 标签定义 Lxx:  记录它指向下一条指令的地址
                std::string label = tok.substr(0, tok.size() - 1);
                labels[curName][label] = (int)cur->code.size();
            } else {
                // 普通指令
                Instr in;
                in.op = opFromMnemonic(tok);
                switch (in.op) {
                case Op::ICONST: case Op::ILOAD: case Op::ISTORE: {
                    int v; ss >> v; in.arg = v;
                } break;
                case Op::IF_ICMPEQ: case Op::IF_ICMPNE: case Op::IF_ICMPLT:
                case Op::IF_ICMPGE: case Op::IF_ICMPGT: case Op::IF_ICMPLE:
                case Op::GOTO: {
                    std::string label; ss >> label;   // 跳转目标是标签,稍后回填
                    pendingJumps.emplace_back(curIdx, (int)cur->code.size(), label);
                } break;
                case Op::INVOKESTATIC: {
                    std::string name; ss >> name;
                    in.cpIndex = internMethod(name);  // 进常量池(符号引用)
                } break;
                case Op::NEW: {
                    std::string cls; ss >> cls;
                    in.cpIndex = internClass(cls);
                } break;
                case Op::GETFIELD: case Op::PUTFIELD: {
                    std::string cf; ss >> cf;         // 形如 Counter.count
                    auto dot = cf.find('.');
                    in.cpIndex = internField(cf.substr(0, dot), cf.substr(dot + 1));
                } break;
                default: break;                       // 无操作数指令
                }
                cur->code.push_back(in);
            }
        }

        // 回填跳转:把标签名解析成方法内指令地址
        for (auto& [mi, pos, label] : pendingJumps) {
            Method& m = vm.methods[mi];               // 此时 vm.methods 不再扩容,下标稳定
            auto it = labels[m.name].find(label);
            if (it == labels[m.name].end())
                throw std::runtime_error("未定义标签:" + label + " (方法 " + m.name + ")");
            m.code[pos].arg = it->second;
        }
    }
};

// ============================================================
int main(int argc, char** argv) {
    std::string path = (argc > 1) ? argv[1] : "program.mjvm";
    VM vm;

    try {
        // ---- 加载:文本 class 文件 -> 方法区 + 常量池 ----
        ClassLoader loader(vm);
        loader.load(path);
        std::cout << "== 类加载完成:" << path << " ==\n";
        vm.dumpConstPool();

        // ---- 链接:符号引用 -> 直接引用 ----
        vm.link();
        std::cout << "== 链接完成(符号引用已解析为直接引用) ==\n";

        // ---- 执行:入口 = main ----
        int mainIdx = vm.methodIndex("main");
        if (mainIdx < 0) throw std::runtime_error("找不到入口方法 main");
        std::cout << "== mini-JVM 启动,执行 main ==\n";
        vm.execute(mainIdx, {});
        std::cout << "== 执行结束,堆槽总数 " << vm.heap.size()
                  << ",存活对象 " << vm.countLive() << " ==\n";
    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
