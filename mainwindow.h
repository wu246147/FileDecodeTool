#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QAtomicInt>

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

// ============================================================
//  工作线程
// ============================================================
class WorkerThread : public QThread
{
    Q_OBJECT
public:
    WorkerThread(QObject *parent = nullptr);

    void setConfig(const QString &srcDir,
                   const QString &dstDir,
                   const QStringList &allowedExt,
                   const QString &notepadPath);

    void requestStop();

signals:
    void logMessage(const QString &msg);

protected:
    void run() override;

private:
    QString m_srcDir;
    QString m_dstDir;
    QStringList m_allowedExt;
    QString m_notepadPath;
    QAtomicInt m_stopRequested;
};

// ============================================================
//  主窗口
// ============================================================
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_pushButton_clicked();
    void on_pushButton_stop_clicked();
    void onLogMessage(const QString &msg);

private:
    Ui::MainWindow *ui;
    WorkerThread *m_worker = nullptr;

    void appendLog(const QString &msg);
    void setRunning(bool running);
};

#endif // MAINWINDOW_H
