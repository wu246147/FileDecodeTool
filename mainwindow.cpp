#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QTextStream>
#include <windows.h>

#pragma execution_character_set("utf-8")

// ============================================================
//  配置（毫秒）
// ============================================================
static constexpr int WAIT_OPEN_MS   = 400;
static constexpr int WAIT_POLL_MS   = 100;
static constexpr int POLL_MAX_RETRY = 50;
static constexpr int WAIT_CLOSE_MS  = 200;

// ============================================================
//  通过 PID 查找窗口（不检查可见性，因为 SW_HIDE 后窗口不可见）
// ============================================================

static HWND findWindowByPid(DWORD targetPid)
{
    struct Data
    {
        DWORD pid;
        HWND  hwnd;
    } d{ targetPid, nullptr };

    ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
    {
        auto *p = reinterpret_cast<Data *>(lp);
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);

        if(pid != p->pid)
            return TRUE;

        // 优先找带标题栏的主窗口（排除工具/弹窗子窗口）
        LONG style = ::GetWindowLongW(hwnd, GWL_STYLE);
        if(style & WS_CAPTION)
        {
            p->hwnd = hwnd;
            return FALSE;
        }

        // 没找到有标题栏的，先记下第一个
        if(!p->hwnd)
            p->hwnd = hwnd;

        return TRUE;
    }, reinterpret_cast<LPARAM>(&d));

    return d.hwnd;
}

// ============================================================
//  递归查找子窗口
// ============================================================

static HWND findChildRecursive(HWND parent, const wchar_t *className)
{
    HWND child = ::FindWindowExW(parent, nullptr, className, nullptr);
    if(child)
    {
        return child;
    }

    child = ::FindWindowExW(parent, nullptr, nullptr, nullptr);
    while(child)
    {
        HWND found = findChildRecursive(child, className);
        if(found)
        {
            return found;
        }
        child = ::FindWindowExW(parent, child, nullptr, nullptr);
    }
    return nullptr;
}

// ============================================================
//  记事本路径（优先 System32 下的经典记事本）
// ============================================================

static QString findNotepad()
{
    const QStringList paths =
    {
        QStringLiteral(R"(C:\Windows\System32\notepad.exe)"),
        QStringLiteral(R"(C:\Windows\notepad.exe)")
    };
    for(const auto &p : paths)
        if(QFile::exists(p))
        {
            return p;
        }
    return QStringLiteral("notepad.exe");
}

// ============================================================
//  通过 Win32 消息直接读取 Edit 控件文本
//  不抢焦点，不发键盘，完全后台静默
// ============================================================

static QString readFromEditControl(HWND notepadHwnd)
{
    static const wchar_t *classNames[] =
    {
        L"Edit", L"RichEdit50W", L"RichEditD2D", nullptr
    };

    // 找编辑区控件
    HWND editHwnd = nullptr;
    for(int i = 0; classNames[i] && !editHwnd; ++i)
    {
        editHwnd = ::FindWindowExW(notepadHwnd, nullptr, classNames[i], nullptr);
    }

    if(!editHwnd)
    {
        for(int i = 0; classNames[i] && !editHwnd; ++i)
        {
            editHwnd = findChildRecursive(notepadHwnd, classNames[i]);
        }
    }

    if(!editHwnd)
        return {};

    // 轮询等待内容就绪（大文件需要更多加载时间）
    for(int retry = 0; retry < POLL_MAX_RETRY; ++retry)
    {
        LRESULT textLen = ::SendMessageW(editHwnd, WM_GETTEXTLENGTH, 0, 0);
        if(textLen > 0)
        {
            int bufLen = static_cast<int>(textLen) + 1;
            auto *buf  = new wchar_t[bufLen] {};
            ::SendMessageW(editHwnd, WM_GETTEXT,
                           static_cast<WPARAM>(bufLen),
                           reinterpret_cast<LPARAM>(buf));
            QString result = QString::fromWCharArray(buf, static_cast<int>(textLen));
            delete[] buf;
            return result;
        }
        QThread::msleep(WAIT_POLL_MS);
    }
    return {};
}

// ============================================================
//  核心：CreateProcessW + SW_HIDE 启动记事本
//  窗口从创建起就是隐藏的，用户完全看不到
//  通过消息读取内容，全程不抢焦点
// ============================================================

static QString openAndRead(const QString &filePath, const QString &notepadPath)
{
    // ---- 1. 以 SW_HIDE 启动记事本 ----
    // STARTUPINFO 指定 SW_HIDE，记事本的 nCmdShow 就是隐藏
    // 窗口在创建时就不会显示在屏幕上

    STARTUPINFOW si = {};
    si.cb           = sizeof(si);
    si.dwFlags      = STARTF_USESHOWWINDOW;
    si.wShowWindow  = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    // CreateProcessW 需要可写的命令行缓冲区
    std::wstring cmdLine = QString("\"%1\" \"%2\"")
                           .arg(notepadPath, filePath)
                           .toStdWString();

    BOOL ok = ::CreateProcessW(
                  nullptr,
                  cmdLine.data(),
                  nullptr, nullptr,
                  FALSE,
                  CREATE_NO_WINDOW,      // 不创建控制台窗口
                  nullptr, nullptr,
                  &si, &pi);

    if(!ok)
        return {};

    // ---- 2. 等待记事本加载文件（自动解密）----
    QThread::msleep(WAIT_OPEN_MS);

    // ---- 3. 通过 PID 找到窗口（SW_HIDE 下窗口不可见但仍存在）----
    HWND hwnd = findWindowByPid(pi.dwProcessId);
    if(!hwnd)
    {
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return {};
    }

    // ---- 4. 双重保险：移到屏幕外 + 设为工具窗口 ----
    ::SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW;    // 任务栏不显示
    exStyle &= ~WS_EX_APPWINDOW;    // 不显示为应用窗口
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    // ---- 5. 通过消息读取解密后的明文（不抢焦点）----
    QString content = readFromEditControl(hwnd);

    // ---- 6. 关闭记事本 ----
    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    ::WaitForSingleObject(pi.hProcess, 3000);

    // 强制结束（如果还没退出）
    DWORD exitCode = 0;
    if(::GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == STILL_ACTIVE)
    {
        ::TerminateProcess(pi.hProcess, 1);
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    return content;
}

// ============================================================
//  类型筛选
// ============================================================

static QStringList parseExtensions(const QString &filterStr)
{
    QStringList exts;
    if(filterStr.trimmed().isEmpty())
    {
        return exts;
    }

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
        exts.append(ext);
    }
    return exts;
}

static bool matchesExtension(const QString &path, const QStringList &allowed)
{
    if(allowed.isEmpty())
    {
        return true;
    }
    return allowed.contains(QFileInfo(path).suffix().toLower());
}

static QStringList collectFiles(const QString &srcDir, const QStringList &ext)
{
    QStringList files;
    const QDir src(srcDir);
    QDirIterator it(srcDir, QDir::Files, QDirIterator::Subdirectories);
    while(it.hasNext())
    {
        it.next();
        if(matchesExtension(it.filePath(), ext))
        {
            files.append(src.relativeFilePath(it.filePath()));
        }
    }
    return files;
}

// ============================================================
//  WorkerThread 实现
// ============================================================

WorkerThread::WorkerThread(QObject *parent)
    : QThread(parent), m_stopRequested(0)
{
}

void WorkerThread::setConfig(const QString &srcDir,
                             const QString &dstDir,
                             const QStringList &allowedExt,
                             const QString &notepadPath)
{
    m_srcDir      = srcDir;
    m_dstDir      = dstDir;
    m_allowedExt  = allowedExt;
    m_notepadPath = notepadPath;
}

void WorkerThread::requestStop()
{
    m_stopRequested.store(1);
}

void WorkerThread::run()
{
    m_stopRequested.store(0);

    emit logMessage("==================================================");
    emit logMessage("  加密文件批量解密工具 (后台静默模式)");
    emit logMessage("==================================================");
    emit logMessage("");
    emit logMessage("起始文件夹: " + QDir(m_srcDir).absolutePath());
    emit logMessage("保存文件夹: " + QDir(m_dstDir).absolutePath());
    emit logMessage("文件筛选:   "
                    + (m_allowedExt.isEmpty()
                       ? QString("无 (处理所有)")
                       : m_allowedExt.join("|")));
    emit logMessage("");

    const QStringList files = collectFiles(m_srcDir, m_allowedExt);
    const int total = files.size();

    if(total == 0)
    {
        emit logMessage("没有找到符合条件的文件");
        return;
    }

    emit logMessage(QString("共 %1 个文件待处理").arg(total));
    emit logMessage("");
    emit logMessage("开始处理...\n");

    QElapsedTimer timer;
    timer.start();

    int success = 0;
    int fail    = 0;
    QStringList failedList;

    for(int i = 0; i < total; ++i)
    {
        if(m_stopRequested.loadAcquire())
        {
            emit logMessage("\n>>> 用户请求停止 <<<");
            break;
        }

        const QString &rel    = files.at(i);
        const QString absPath = QDir(m_srcDir).absoluteFilePath(rel);
        const QString outPath = QDir(m_dstDir).absoluteFilePath(rel);

        emit logMessage(QString("[%1/%2] %3").arg(i + 1).arg(total).arg(rel));

        QDir().mkpath(QFileInfo(outPath).absolutePath());

        const QString content = openAndRead(absPath, m_notepadPath);

        if(content.isEmpty())
        {
            emit logMessage("  -> 空/读取失败，跳过");
            ++fail;
            failedList.append(rel);
            continue;
        }

        QFile outFile(outPath);
        if(outFile.open(QIODevice::WriteOnly))
        {
            outFile.write(content.toUtf8());
            outFile.close();
            emit logMessage(QString("  -> %1 字符  ok").arg(content.size()));
            ++success;
        }
        else
        {
            emit logMessage("  -> 写入失败: " + outFile.errorString());
            ++fail;
            failedList.append(rel);
        }
    }

    const qint64 elapsed = timer.elapsed();

    emit logMessage("");
    emit logMessage("==================================================");
    emit logMessage(QString("  完成! 成功 %1 个, 失败 %2 个, 共 %3 个")
                    .arg(success).arg(fail).arg(total));
    emit logMessage(QString("  耗时 %1 秒")
                    .arg(QString::number(elapsed / 1000.0, 'f', 1)));

    if(!failedList.isEmpty())
    {
        emit logMessage("  失败文件:");
        for(const auto &name : failedList)
        {
            emit logMessage("    - " + name);
        }
    }

    emit logMessage("==================================================");
}

// ============================================================
//  MainWindow 实现
// ============================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    if(m_worker && m_worker->isRunning())
    {
        m_worker->requestStop();
        m_worker->wait(5000);
    }
    delete ui;
}

void MainWindow::appendLog(const QString &msg)
{
    if(ui->textEdit)
    {
        ui->textEdit->append(msg);
    }
    QTextStream out(stdout);
    out << msg << "\n";
    out.flush();
}

void MainWindow::setRunning(bool running)
{
    ui->pushButton->setEnabled(!running);
    ui->pushButton_stop->setEnabled(running);
}

void MainWindow::on_pushButton_clicked()
{
    const QString srcDir    = ui->lineEdit->text().trimmed();
    const QString dstDir    = ui->lineEdit_2->text().trimmed();
    const QString filterStr = ui->lineEdit_3->text().trimmed();

    if(srcDir.isEmpty() || dstDir.isEmpty())
    {
        appendLog("请填写起始文件夹和保存文件夹");
        return;
    }

    if(!QDir(srcDir).exists())
    {
        appendLog("起始文件夹不存在: " + srcDir);
        return;
    }

    if(ui->textEdit)
    {
        ui->textEdit->clear();
    }

    setRunning(true);

    m_worker = new WorkerThread(this);

    connect(m_worker, &WorkerThread::logMessage,
            this, &MainWindow::onLogMessage);
    connect(m_worker, &WorkerThread::finished,
            this, [this]()
    {
        setRunning(false);
        m_worker->deleteLater();
        m_worker = nullptr;
    });

    const QStringList allowedExt = parseExtensions(filterStr);
    m_worker->setConfig(srcDir, dstDir, allowedExt, findNotepad());
    m_worker->start();
}

void MainWindow::on_pushButton_stop_clicked()
{
    if(m_worker && m_worker->isRunning())
    {
        m_worker->requestStop();
        appendLog("正在停止...");
    }
}

void MainWindow::onLogMessage(const QString &msg)
{
    appendLog(msg);
}
