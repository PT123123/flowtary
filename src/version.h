// Flowtary 版本号 —— 全项目唯一来源
//   · release.bat 解析本文件的 FT_VER_DOT，把产物命名为 flowtary-<版本>.exe
//   · exe 的版本信息资源 src\flowtary.rc 引用 FT_VER_COMMA 与 FT_VER_DOT
//
// 升级版本号时：FT_VER_COMMA（数值四元组）与 FT_VER_DOT（点号分隔）要一起改。
//
// 注意：下面各宏的「取值」必须保持纯 ASCII，中文只能出现在注释里。
//   原因：rc.exe 按系统 ANSI 代码页读取源文件（中文 Windows 是 GBK），
//   而本文件是 UTF-8。若字符串字面量里含中文，UTF-8 字节会被当作 GBK 错位解析，
//   引号识别随之错乱，报 "RC2001: newline in constant"（定位到的行号往往不准，
//   实际元凶是含中文的那一行取值）。注释不受影响，所以中文说明可以放心写在注释里。
#pragma once

#define FT_VER_MAJOR 1
#define FT_VER_MINOR 0
#define FT_VER_PATCH 0
#define FT_VER_BUILD 0

#define FT_VER_COMMA 1,0,0,0    /* rc: FILEVERSION / PRODUCTVERSION */
#define FT_VER_DOT   "1.0.0.0"  /* rc string fields + release.bat  */

#define FT_PRODUCT_NAME "Flowtary"
#define FT_FILE_DESC    "Flowtary Launcher"
#define FT_COMPANY      "Flowtary"
#define FT_COPYRIGHT    "Copyright (C) 2026"
