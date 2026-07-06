#include "mainwindow.h"
#include "./ui_mainwindow.h"



// main.cpp
/*
 * 用法:  decrypt_tool <起始文件夹> <保存文件夹>
 * 示例:  decrypt_tool D:\加密文件 D:\解密输出
 *
 * 遍历起始文件夹下所有文件，通过记事本解密后，
 * 按原始目录结构保存到指定的保存文件夹。
 *
 * 构建:
 *   mkdir build && cd build
 *   cmake .. -G "Visual Studio 17 2022"
 *   cmake --build . --config Release
 *
 * 或用 Qt Creator 直接打开 CMakeLists.txt 构建。
 *
 * 注意: 控制台如显示乱码，先执行 chcp 65001
 */

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QElapsedTimer>
#include <QThread>
#include <windows.h>

#pragma execution_character_set("utf-8")

// ============================================================
//  等待时间配置（毫秒），电脑快可调小，慢则调大
// ============================================================
static constexpr int WAIT_OPEN_MS  = 300;   // 记事本窗口打开
static constexpr int WAIT_KEY_MS   = 50;    // 键盘操作间隔
static constexpr int WAIT_CLOSE_MS = 100;   // 关闭记事本
static constexpr int WAIT_FOCUS_MS = 50;    // 窗口聚焦后等待

// ============================================================
//  Windows API — 键盘模拟
// ============================================================

static void pressKey(WORD vk)
{
    INPUT inputs[2] {};
    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = vk;
    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, inputs, sizeof(INPUT));
}

static void hotkey(WORD mod, WORD key)
{
    INPUT inputs[4] {};
    inputs[0].type   = INPUT_KEYBOARD;
    inputs[0].ki.wVk = mod;
    inputs[1].type   = INPUT_KEYBOARD;
    inputs[1].ki.wVk = key;
    inputs[2].type   = INPUT_KEYBOARD;
    inputs[2].ki.wVk = key;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type   = INPUT_KEYBOARD;
    inputs[3].ki.wVk = mod;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(4, inputs, sizeof(INPUT));
}

// ============================================================
//  Windows API — 剪贴板
// ============================================================

static void clearClipboard()
{
    if(::OpenClipboard(NULL))
    {
        ::EmptyClipboard();
        ::CloseClipboard();
    }
}

static QString readClipboard()
{
    QString result;
    if(!::OpenClipboard(NULL))
    {
        return result;
    }

    HANDLE hData = ::GetClipboardData(CF_UNICODETEXT);
    if(hData)
    {
        const auto *p = static_cast<const wchar_t *>(::GlobalLock(hData));
        if(p)
        {
            result = QString::fromWCharArray(p);
            ::GlobalUnlock(hData);
        }
    }
    ::CloseClipboard();
    return result;
}

// ============================================================
//  记事本自动化
// ============================================================

static QString findNotepad()
{
    const QStringList paths =
    {
        QStringLiteral(R"(C:\Windows\notepad.exe)"),
        QStringLiteral(R"(C:\Windows\System32\notepad.exe)")
    };
    for(const auto &p : paths)
        if(QFile::exists(p))
        {
            return p;
        }
    return QStringLiteral("notepad.exe");
}

static bool focusNotepadWindow()
{
    HWND hwnd = ::FindWindowW(L"Notepad", nullptr);
    if(hwnd)
    {
        ::SetForegroundWindow(hwnd);
        return true;
    }
    return false;
}

/**
 * @brief 用记事本打开文件，全选复制，返回剪贴板中的明文内容
 *
 * 流程：启动记事本(自动解密) → 等待窗口 → 聚焦 →
 *       Ctrl+A(全选) → Ctrl+C(复制) → 读剪贴板 → Alt+F4(N) 关闭
 */
static QString openAndCopy(const QString &filePath,
                           const QString &notepadPath)
{
    // 清空剪贴板，防止残留旧内容干扰判断
    clearClipboard();

    // 启动记事本
    QProcess proc;
    proc.start(notepadPath, QStringList() << filePath);
    if(!proc.waitForStarted(3000))
        return {};

    QThread::msleep(WAIT_OPEN_MS);

    // 确保记事本窗口在最前面
    focusNotepadWindow();
    QThread::msleep(WAIT_FOCUS_MS);

    // Ctrl+A 全选
    hotkey(VK_CONTROL, 'A');
    QThread::msleep(WAIT_KEY_MS);

    // Ctrl+C 复制
    hotkey(VK_CONTROL, 'C');
    QThread::msleep(WAIT_KEY_MS);

    // 读取剪贴板（此时内容已被记事本解密为明文）
    QString content = readClipboard();

    // Alt+F4 关闭记事本
    hotkey(VK_MENU, VK_F4);
    QThread::msleep(WAIT_CLOSE_MS);

    // 按 N —— 不保存
    pressKey('N');
    QThread::msleep(WAIT_KEY_MS);

    // 等待记事本进程退出
    proc.waitForFinished(2000);

    return content;
}

// ============================================================
//  类型筛选解析
// ============================================================

static QStringList parseExtensions(const QString &filterStr)
{
    QStringList extensions;
    if(filterStr.trimmed().isEmpty())
    {
        return extensions;
    }

// 按 | 分割，转小写，去掉开头的点
#ifdef QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    const QStringList parts = filterStr.split('|', Qt::SkipEmptyParts);
#else
    const QStringList parts = filterStr.split('|', QString::SkipEmptyParts);
#endif

    for(auto ext : parts)
    {
        ext = ext.trimmed().toLower();
        if(ext.isEmpty())
        {
            continue;
        }
        if(ext.startsWith('.'))
        {
            ext = ext.mid(1);
        }
        extensions.append(ext);
    }
    return extensions;
}

static bool matchesExtension(const QString &filePath, const QStringList &allowed)
{
    if(allowed.isEmpty())
    {
        return true;    // 无筛选，全部通过
    }
    const QString ext = QFileInfo(filePath).suffix().toLower();
    return allowed.contains(ext);
}

// ============================================================
//  文件收集（带筛选）
// ============================================================

static QStringList collectFiles(const QString &srcDir, const QStringList &allowedExtensions)
{
    QStringList files;
    const QDir src(srcDir);
    QDirIterator it(srcDir, QDir::Files, QDirIterator::Subdirectories);

    while(it.hasNext())
    {
        it.next();
        if(matchesExtension(it.filePath(), allowedExtensions))
        {
            files.append(src.relativeFilePath(it.filePath()));
        }
    }
    return files;
}


// // ============================================================
// //  主函数
// // ============================================================

// int main(int argc, char *argv[])
// {
//     QCoreApplication app(argc, argv);
//     QTextStream out(stdout);
//     QTextStream err(stderr);

// #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
//     out.setEncoding(QStringConverter::Utf8);
//     err.setEncoding(QStringConverter::Utf8);
// #else
//     out.setCodec("UTF-8");
//     err.setCodec("UTF-8");
// #endif

//     const QStringList args = QCoreApplication::arguments();

//     if(args.size() != 3)
//     {
//         out << "用法: decrypt_tool <起始文件夹> <保存文件夹>\n"
//             << "示例: decrypt_tool D:\\加密文件 D:\\解密输出\n";
//         return 1;
//     }

//     const QString srcDir = args.at(1);
//     const QString dstDir = args.at(2);

//     if(!QDir(srcDir).exists())
//     {
//         err << "起始文件夹不存在: " << srcDir << "\n";
//         return 1;
//     }

//     const QString notepadPath = findNotepad();

//     out << "==================================================\n"
//         << "  加密文件批量解密工具\n"
//         << "  运行中请勿移动鼠标或切换窗口\n"
//         << "==================================================\n\n"
//         << "起始文件夹: " << QDir(srcDir).absolutePath() << "\n"
//         << "保存文件夹: " << QDir(dstDir).absolutePath() << "\n";

//     const QStringList files = collectFiles(srcDir);
//     const int total = files.size();

//     if(total == 0)
//     {
//         out << "起始文件夹下没有找到任何文件\n";
//         return 0;
//     }

//     out << "共 " << total << " 个文件待处理\n\n";
//     out.flush();

//     QElapsedTimer timer;
//     timer.start();

//     int success = 0;
//     int fail    = 0;
//     QStringList failedList;

//     for(int i = 0; i < total; ++i)
//     {
//         const QString &rel    = files.at(i);
//         const QString absPath = QDir(srcDir).absoluteFilePath(rel);
//         const QString outPath = QDir(dstDir).absoluteFilePath(rel);

//         out << QString("[%1/%2] %3").arg(i + 1).arg(total).arg(rel);
//         out.flush();

//         // 确保输出子目录存在
//         QDir().mkpath(QFileInfo(outPath).absolutePath());

//         // 通过记事本解密并复制内容
//         const QString content = openAndCopy(absPath, notepadPath);

//         if(content.isEmpty())
//         {
//             out << " -> 空，跳过\n";
//             ++fail;
//             failedList.append(rel);
//             continue;
//         }

//         // 二进制模式写入，不做换行符转换
//         QFile outFile(outPath);
//         if(outFile.open(QIODevice::WriteOnly))
//         {
//             outFile.write(content.toUtf8());
//             outFile.close();
//             out << " -> " << content.size() << " 字符 ok\n";
//             ++success;
//         }
//         else
//         {
//             out << " -> 写入失败: " << outFile.errorString() << "\n";
//             ++fail;
//             failedList.append(rel);
//         }

//         out.flush();
//     }

//     const qint64 elapsed = timer.elapsed();

//     out << "\n==================================================\n"
//         << "  完成! 成功 " << success << " 个, 失败 " << fail
//         << " 个, 共 " << total << " 个\n"
//         << "  耗时 " << QString::number(elapsed / 1000.0, 'f', 1)
//         << " 秒\n";

//     if(!failedList.isEmpty())
//     {
//         out << "  失败文件:\n";
//         for(const auto &name : failedList)
//         {
//             out << "    - " << name << "\n";
//         }
//     }

//     out << "==================================================\n";
//     out.flush();

//     return 0;
// }


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_pushButton_clicked()
{
    QTextStream out(stdout);
    QTextStream err(stderr);

    const QString srcDir = ui->lineEdit->text();
    const QString dstDir = ui->lineEdit_2->text();
    const QString filterStr = ui->lineEdit_3->text();

    if(!QDir(srcDir).exists())
    {
        err << "起始文件夹不存在: " << srcDir << "\n";
        return;
    }

    // 解析扩展名筛选列表
    const QStringList allowedExt = parseExtensions(filterStr);

    const QString notepadPath = findNotepad();

    out << "==================================================\n"
        << "  加密文件批量解密工具\n"
        << "  运行中请勿移动鼠标或切换窗口\n"
        << "==================================================\n\n"
        << "起始文件夹: " << QDir(srcDir).absolutePath() << "\n"
        << "保存文件夹: " << QDir(dstDir).absolutePath() << "\n";


    if(!allowedExt.isEmpty())
    {
        out << "文件筛选:   " << filterStr << " (仅处理这些类型)\n";
    }
    else
    {
        out << "文件筛选:   无 (处理所有文件)\n";
    }

    out << "\n";
    out.flush();


    const QStringList files = collectFiles(srcDir, allowedExt);
    const int total = files.size();

    if(total == 0)
    {
        out << "起始文件夹下没有找到任何文件\n";
        return;
    }

    out << "共 " << total << " 个文件待处理\n\n";
    out.flush();

    QElapsedTimer timer;
    timer.start();

    int success = 0;
    int fail    = 0;
    QStringList failedList;

    for(int i = 0; i < total; ++i)
    {
        const QString &rel    = files.at(i);
        const QString absPath = QDir(srcDir).absoluteFilePath(rel);
        const QString outPath = QDir(dstDir).absoluteFilePath(rel);

        out << QString("[%1/%2] %3").arg(i + 1).arg(total).arg(rel);
        out.flush();

        // 确保输出子目录存在
        QDir().mkpath(QFileInfo(outPath).absolutePath());

        // 通过记事本解密并复制内容
        const QString content = openAndCopy(absPath, notepadPath);

        if(content.isEmpty())
        {
            out << " -> 空，跳过\n";
            ++fail;
            failedList.append(rel);
            continue;
        }

        // 二进制模式写入，不做换行符转换
        QFile outFile(outPath);
        if(outFile.open(QIODevice::WriteOnly))
        {
            outFile.write(content.toUtf8());
            outFile.close();
            out << " -> " << content.size() << " 字符 ok\n";
            ++success;
        }
        else
        {
            out << " -> 写入失败: " << outFile.errorString() << "\n";
            ++fail;
            failedList.append(rel);
        }

        out.flush();
    }

    const qint64 elapsed = timer.elapsed();

    out << "\n==================================================\n"
        << "  完成! 成功 " << success << " 个, 失败 " << fail
        << " 个, 共 " << total << " 个\n"
        << "  耗时 " << QString::number(elapsed / 1000.0, 'f', 1)
        << " 秒\n";

    if(!failedList.isEmpty())
    {
        out << "  失败文件:\n";
        for(const auto &name : failedList)
        {
            out << "    - " << name << "\n";
        }
    }

    out << "==================================================\n";
    out.flush();

    return;

}

