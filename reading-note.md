# reading notes

LUA是一个非常精简的脚本语言，采用ANSI C语言编写，具有大部分脚本语言该有的特性：
* 虚拟机
* 垃圾回收
* 协程
* 模块化
* 提供C API与C语言高效交互，同时也可以通过C语言进行扩展
* 执行引擎可嵌入到宿主语言，使用户程序具有脚本化的功能，进一步扩展程序功能

LUA脚本的基本类型：
1. nil
2. boolean
3. string
4. number
5. table
6. function
7. thread

LUA内嵌一些以C函数提供扩展的module/package：
1. os
2. sys
3. io
4. math

C-API，它是如何与用户C语言程序进行交互的
* 利用一个虚拟栈，LUA内部引擎和外界C代码交互
* C-API对虚拟栈的操作，划分为不同的操作类别

对于LUA，我们需要重点研究学习它的虚拟机VM的实现以及垃圾回收的实现
* 虚拟机VM的实现，涉及到LUA脚本解析，中间字节代码(opcode)的生成以及执行
* 垃圾回收的实现，涉及到如何管理不同类型的对象，如何标记可收回垃圾对象等


| file name | comment |
| --- | --- |
| lvm.{h/c} | LUA虚拟机的实现 |
| lobject.{h/c} | LUA数据类型的定义 |
| ltm.{h/c} | LUA tagmethod的定义 |
| lopcodes.{h/c} | LUA虚拟机字节码的定义 |
| lcode.{h/c} | LUA opcode的生成 |
| lgc.{h/c} | LUA的垃圾回收的实现 |
| lparser.{h/c} | parser相关的代码 |
| llex.{h/c} | lexer相关的代码 |
| lstate.{h/c} | lua_State的定义 |
