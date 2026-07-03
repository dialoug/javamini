# mini-JVM

一个用 C++ 实现的、**简易易读**的 Java 虚拟机架构演示。目标不是性能或完整,而是把真实 JVM 的核心分层用最少的代码讲清楚,并且真能编译运行。

整条链路完整可跑:

```
源码(.mj)  ──mjc──▶  字节码(.mjvm)  ──类加载──▶  链接  ──▶  解释执行  +  GC
 (你写的程序)  编译器      (中间表示)     加载子系统   符号解析   执行引擎    垃圾回收
```

## 组成

| 文件 | 角色 | 对应真实工具链 |
|------|------|------|
| [mjc.cpp](mjc.cpp) | 编译器:词法 → 语法 → 代码生成 | `javac` |
| [mini_jvm.cpp](mini_jvm.cpp) | 虚拟机:类加载 → 链接 → 解释执行 → GC | `java` / JVM |
| [program.mj](program.mj) | 示例源码(mini 语言) | `.java` |
| [program.mjvm](program.mjvm) | 手写字节码示例 | `.class`(文本化) |

## 构建与运行

需要 CMake ≥ 3.15 和一个 C++17 编译器(MSVC / g++ / clang++)。

```bash
# 配置 + 编译
cmake -S . -B build
cmake --build build --config Debug

# 一条命令跑全链路:编译 program.mj 并在 mini-JVM 上执行
cmake --build build --target demo
```

手动分步:

```bash
cd build/Debug
./mjc program.mj program.mjvm     # 源码 -> 字节码
./mini_jvm program.mjvm           # 加载、链接、执行
```

预期输出:

```
[输出] 120        factorial(5)
[输出] 55         sum(10)
[GC] 回收前存活 6,回收 5,回收后存活 1
[输出] 42         keeper.count(GC 后依然存活)
```

## 架构:对应真实 JVM 的哪些层

### 1. 编译器 mjc(相当于 javac)

三步式,和真实编译器一致:

- **词法分析(Lexer)**:字符流 → 记号(token)流
- **语法分析(Parser)**:递归下降,记号流 → 抽象语法树(AST)。表达式优先级用层级函数表达(比较 < 加减 < 乘除 < 基本单元)
- **代码生成(CodeGen)**:遍历 AST → 文本字节码

生成时能看到真实 `javac` 也用的手法:`if` 用**取反跳转**(条件为假才跳走)、标签由编译器生成、局部变量按出现顺序分配槽位。

### 2. 字节码指令集

基于**操作数栈**的指令(不是寄存器机),和 JVM 一致。含常量/局部变量存取、算术、比较跳转 `if_icmp{eq,ne,lt,ge,gt,le}`、方法调用/返回、对象与字段操作。

### 3. 类加载子系统

- 解析文本 `.mjvm` 填充**方法区**(类/方法元数据)
- **常量池**:字节码里存的是**符号引用**(方法名、类名、`类.字段`),不是下标
- **链接(解析)**:把常量池的符号引用解析成**直接引用**(方法/类/字段下标)。这正是真实 JVM 类加载"验证→准备→解析"里的"解析"

### 4. 运行时数据区

| 区域 | 代码 | 说明 |
|------|------|------|
| 堆(Heap) | `VM::heap` | 对象实例,引用简化为堆下标 |
| 虚拟机栈 | `VM::stack` | 一摞栈帧,每次方法调用压一帧 |
| 栈帧(Frame) | `Frame` | 局部变量表 + 操作数栈 + 程序计数器 |

### 5. 执行引擎

一个 `while` + `switch` 的解释器循环:取指(PC 指向的指令)→ PC 前进 → 分派执行。纯解释,无 JIT。

### 6. 垃圾回收(标记-清除,精确 GC)

- 每个值带类型标签(`Value{INT|REF}`),让 VM 能区分"数值 42"和"引用到堆下标 42"——这是**精确 GC 的前提**,真实 JVM 靠 OOP map / 栈映射实现
- **GC Roots** = 所有栈帧的局部变量表 + 操作数栈里的引用
- 从根出发标记可达对象,清除其余;回收的槽进 freeList 可复用。不移动对象,所以"引用=下标"始终有效

## mini 语言(.mj)速览

```
class Counter { count; }          // 类与字段

func factorial(n) {               // 函数(编译成静态方法)
    if (n > 1) { return n * factorial(n - 1); }
    else       { return 1; }
}

func main() {
    var keeper = new Counter();   // 对象
    keeper.count = 42;            // 字段赋值
    print(factorial(5));          // 内建 print
    while (i < 5) { new Counter(); }  // 循环 + 制造垃圾
    gc();                         // 内建 gc
    return 0;
}
```

支持:`var`/赋值、`if/else`、`while`、`return`、`new`、`obj.field`、运算符 `+ - * /` 和比较 `< > <= >= == !=`、函数调用与递归。

## 有意的简化(与真实 JVM 的差距)

- 所有值只有 `int` 和引用两种,无完整类型系统、无 `long/double/float`
- GC 是显式指令触发,非自动;标记-清除不分代、不移动、无并发标记、无写屏障;真实 GC 只在**安全点**回收
- 类加载在启动时一次性全解析,真实 JVM 是**惰性解析**(首次用到才解析)
- 无字节码验证、无异常处理表、无接口/继承/多态、mini 语言要求字段名全局唯一
- 只有解释器,无 JIT 分层编译

这些简化点正是可以继续深入的方向。
