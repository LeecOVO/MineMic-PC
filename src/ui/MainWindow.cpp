#include "ui/MainWindow.h"
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkInterface>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace minemic {
QIcon applicationIcon() {
    QPixmap image(64, 64); image.fill(Qt::transparent);
    QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(42, 104, 210));
    painter.drawRoundedRect(QRectF(1, 1, 62, 62), 14, 14);
    painter.setBrush(Qt::white); painter.drawRoundedRect(QRectF(25, 12, 14, 27), 7, 7);
    painter.setBrush(Qt::NoBrush); painter.setPen(QPen(Qt::white, 4, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(QRectF(18, 23, 28, 23), 180 * 16, 180 * 16);
    painter.drawLine(32, 46, 32, 53); painter.drawLine(24, 53, 40, 53);
    return QIcon(image);
}

MainWindow::MainWindow() : output_(createAudioOutput()) {
    setWindowTitle(QStringLiteral("MineMic %1 · 开发预览").arg(QApplication::applicationVersion()));
    setWindowIcon(applicationIcon());
    setMinimumSize(680, 640);
    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root); layout->setContentsMargins(24, 20, 24, 20); layout->setSpacing(14);
    auto* title = new QLabel(QStringLiteral("MineMic"));
    QFont titleFont = title->font(); titleFont.setPointSize(24); titleFont.setBold(true); title->setFont(titleFont);
    layout->addWidget(title);
    auto* subtitle = new QLabel(tr("%1 · 开发预览 · 音频增强\n把手机变成电脑麦克风 · 局域网连接").arg(QApplication::applicationVersion()));
    layout->addWidget(subtitle);

    auto* routing = new QGroupBox(tr("音频输出"));
    auto* routingLayout = new QVBoxLayout(routing);
    auto* row = new QHBoxLayout;
    devices_ = new QComboBox; devices_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    devices_->setMinimumContentsLength(30); row->addWidget(devices_, 1);
    refresh_ = new QPushButton(tr("刷新")); row->addWidget(refresh_);
    auto* referenceButton = new QPushButton(tr("外放参考…")); row->addWidget(referenceButton);
    routingLayout->addLayout(row);
    auto* routingHint = new QLabel(tr("选择虚拟声卡的播放端；在会议或游戏中选择它对应的录音端。\n选择实体扬声器时将直接播放手机声音。"));
    routingHint->setWordWrap(true); routingHint->setMinimumHeight(44);
    routingLayout->addWidget(routingHint);
    referenceStatus_ = new QLabel(tr("外放参考：自动跟随系统默认外放设备，手机开启回声抑制后使用"));
    referenceStatus_->setWordWrap(true); referenceStatus_->setTextFormat(Qt::PlainText);
    routingLayout->addWidget(referenceStatus_);
    auto* gainRow = new QHBoxLayout;
    gainRow->addWidget(new QLabel(tr("音量")));
    gain_ = new QSlider(Qt::Horizontal); gain_->setRange(0, 200);
    QSettings settings;
    gain_->setValue(std::clamp(settings.value(QStringLiteral("gain"), 100).toInt(), 0, 200));
    buffer_.setGain(gain_->value() / 100.0f);
    gainRow->addWidget(gain_, 1);
    gainText_ = new QLabel(QStringLiteral("%1%").arg(gain_->value())); gainText_->setMinimumWidth(45);
    gainRow->addWidget(gainText_);
    muted_ = new QCheckBox(tr("静音")); gainRow->addWidget(muted_);
    routingLayout->addLayout(gainRow);
    meter_ = new QProgressBar; meter_->setRange(0, 32768); meter_->setValue(0);
    meter_->setTextVisible(false); meter_->setMaximumHeight(10); routingLayout->addWidget(meter_);
    layout->addWidget(routing);
    auto* reference = new QDialog(this);
    reference->setWindowTitle(tr("回声抑制 · 外放参考")); reference->resize(520, 180);
    auto* referenceLayout = new QVBoxLayout(reference);
    referenceDevices_ = new QComboBox; referenceLayout->addWidget(referenceDevices_);
    auto* referenceHint = new QLabel(tr("默认自动跟随 Windows 当前外放设备，无需每次设置。若游戏或会议单独指定了输出设备，可在此手动选择；选择会自动保存。仅在手机开启回声抑制时回传音频参考。"));
    referenceHint->setWordWrap(true); referenceLayout->addWidget(referenceHint);
#ifndef Q_OS_WIN
    referenceHint->setText(tr("回声抑制参考目前需要 Windows WASAPI；此平台继续支持原始音频与手机降噪。"));
    referenceDevices_->setEnabled(false);
#endif
    auto* referenceClose = new QDialogButtonBox(QDialogButtonBox::Close, reference);
    referenceLayout->addWidget(referenceClose);
    connect(referenceClose, &QDialogButtonBox::rejected, reference, &QDialog::hide);
    connect(referenceButton, &QPushButton::clicked, this, [reference] {
        reference->show(); reference->raise(); reference->activateWindow();
    });

    auto* pairing = new QGroupBox(tr("手机连接"));
    auto* form = new QFormLayout(pairing);
    addresses_ = new QComboBox;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) || interface.flags() & QNetworkInterface::IsLoopBack) continue;
        for (const auto& entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                addresses_->addItem(entry.ip().toString());
        }
    }
    if (!addresses_->count()) addresses_->addItem(QStringLiteral("127.0.0.1"));
    form->addRow(tr("电脑地址"), addresses_);
    port_ = new QSpinBox; port_->setRange(48721, 48721);
    port_->setReadOnly(true); port_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    port_->setValue(std::clamp(settings.value(QStringLiteral("port"), 48721).toInt(), 1024, 65535));
    form->addRow(tr("连接端口"), port_);
    connection_ = new QLineEdit; connection_->setReadOnly(true); connection_->setAccessibleName(tr("6 位配对码"));
    QFont codeFont = connection_->font(); codeFont.setPointSize(26); codeFont.setBold(true);
    connection_->setFont(codeFont); connection_->setAlignment(Qt::AlignCenter);
    connection_->setFixedHeight(56);
    auto* pairingButtons = new QHBoxLayout;
    auto* copy = new QPushButton(tr("复制配对码"));
    auto* regenerate = new QPushButton(tr("更新配对码"));
    regenerate->setToolTip(tr("更换配对码，已连接的手机不受影响"));
    disconnect_ = new QPushButton(tr("断开当前手机")); disconnect_->setEnabled(false);
    pairingButtons->addWidget(copy); pairingButtons->addWidget(regenerate); pairingButtons->addWidget(disconnect_); pairingButtons->addStretch();
    auto* pairingField = new QWidget; pairingField->setMinimumHeight(92);
    auto* pairingFieldLayout = new QVBoxLayout(pairingField);
    pairingFieldLayout->setContentsMargins(0, 0, 0, 0); pairingFieldLayout->setSpacing(8);
    pairingFieldLayout->addWidget(connection_); pairingFieldLayout->addLayout(pairingButtons);
    form->addRow(tr("配对码"), pairingField);
    auto* networkHint = new QLabel(tr("手机输入 6 位配对码即可连接，也可手动输入上方 PC 地址。\n首台手机连接后保持独占；配对码仅在停止接收或手动更新时改变。"));
    networkHint->setWordWrap(true); networkHint->setMinimumHeight(48); form->addRow(networkHint);
    layout->addWidget(pairing);
    status_ = new QLabel(tr("未启动")); status_->setWordWrap(true); status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    statistics_ = new QLabel(tr("48 kHz · 单声道 · PCM16 · 10 ms/包"));
    statistics_->setWordWrap(true); layout->addWidget(statistics_);
    auto* bottom = new QHBoxLayout;
    auto* about = new QPushButton(tr("关于 MineMic"));
    connect(about, &QPushButton::clicked, this, &MainWindow::showAbout);
    bottom->addWidget(about); bottom->addStretch(); start_ = new QPushButton(tr("开始接收"));
    start_->setDefault(true); start_->setMinimumHeight(34); bottom->addWidget(start_);
    layout->addLayout(bottom); layout->addStretch(); setCentralWidget(root);

    receiver_ = new Receiver(buffer_, nullptr, {}, settings.value(QStringLiteral("pairingCode")).toString());
    receiver_->moveToThread(&networkThread_);
    connect(&networkThread_, &QThread::finished, receiver_, &QObject::deleteLater);
    connect(receiver_, &Receiver::listening, this, [this](quint16) {
        if (!busy_) return;
        busy_ = false; setRunning(true); status_->setText(tr("正在等待手机连接"));
    });
    pairingCode_ = receiver_->pairingCode(); updateConnectionInfo();
    QSettings().setValue(QStringLiteral("pairingCode"), pairingCode_);
    connect(receiver_, &Receiver::pairingCodeChanged, this, [this](const QString& code) {
        pairingCode_ = code; updateConnectionInfo();
        QSettings().setValue(QStringLiteral("pairingCode"), code);
    });
    connect(receiver_, &Receiver::referenceStatusChanged, referenceStatus_, &QLabel::setText);
    connect(receiver_, &Receiver::peerChanged, this, [this](const QString& peer) { peerLabel_ = peer; });
    connect(receiver_, &Receiver::connectionChanged, this, [this](bool locked, bool online) {
        locked_ = locked; disconnect_->setEnabled(locked && running_);
        if (running_) status_->setText(!locked ? tr("正在等待手机连接") : online
            ? tr("已连接：%1 · 已独占").arg(peerLabel_)
            : tr("连接中断，仍为 %1 保留；可等待原手机重连或手动断开。").arg(peerLabel_));
    });
    connect(receiver_, &Receiver::discoveryWarning, this, [this](const QString& message) { statistics_->setText(message); });
    connect(receiver_, &Receiver::errorOccurred, this, &MainWindow::showError);
    connect(receiver_, &Receiver::statistics, this,
            [this](quint64 accepted, quint64 rejected, quint64 missing, quint64 late, quint64 resyncs, int queuedMs, int peak) {
        if (!running_) return;
        // No repaint or meter animation while the application is hidden in the tray.
        if (isVisible()) {
            meter_->setValue(peak);
            statistics_->setText(tr("已收 %1 包 · 拒绝 %2 · 缺包 %3 · 迟到 %4 · 重缓冲 %5 · 排队 %6 ms")
                .arg(accepted).arg(rejected).arg(missing).arg(late).arg(resyncs).arg(queuedMs));
        }
        const auto error = output_->takeError();
        if (!error.isEmpty()) showError(error);
    });
    networkThread_.setObjectName(QStringLiteral("MineMic UDP")); networkThread_.start();
    connect(start_, &QPushButton::clicked, this, [this] { if (running_) stop(); else start(); });
    connect(refresh_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(referenceDevices_, &QComboBox::currentIndexChanged, this, [this] {
        const auto id = referenceDevices_->currentData().toString();
        QSettings().setValue(QStringLiteral("referenceDevice"), id);
        QMetaObject::invokeMethod(receiver_, [this, id] { receiver_->setReferenceDevice(id); }, Qt::QueuedConnection);
    });
    connect(port_, &QSpinBox::valueChanged, this, &MainWindow::updateConnectionInfo);
    connect(addresses_, &QComboBox::currentTextChanged, this, &MainWindow::updateConnectionInfo);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(connection_->text()); });
    connect(regenerate, &QPushButton::clicked, this, [this] {
        QMetaObject::invokeMethod(receiver_, &Receiver::refreshPairingCode, Qt::QueuedConnection);
    });
    connect(disconnect_, &QPushButton::clicked, this, [this] {
        QMetaObject::invokeMethod(receiver_, &Receiver::manualDisconnect, Qt::QueuedConnection);
    });
    connect(gain_, &QSlider::valueChanged, this, [this](int value) {
        buffer_.setGain(value / 100.0f); gainText_->setText(QStringLiteral("%1%").arg(value));
    });
    connect(muted_, &QCheckBox::toggled, this, [this](bool value) { buffer_.setMuted(value); });
    refreshDevices();

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = new QSystemTrayIcon(windowIcon(), this); tray_->setToolTip(QStringLiteral("MineMic"));
        auto* menu = new QMenu(this);
        menu->addAction(tr("显示 MineMic"), this, [this] { showNormal(); raise(); activateWindow(); });
        menu->addAction(tr("停止接收"), this, &MainWindow::stop);
        menu->addSeparator(); menu->addAction(tr("关于 MineMic"), this, &MainWindow::showAbout);
        menu->addAction(tr("退出"), qApp, &QApplication::quit);
        tray_->setContextMenu(menu);
        connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) { showNormal(); raise(); activateWindow(); }
        });
        tray_->show();
        auto* hint = new QLabel(tr("关闭窗口后继续在托盘运行；从托盘菜单退出。"));
        hint->setWordWrap(true); layout->addWidget(hint);
        QApplication::setQuitOnLastWindowClosed(false);
    }
}

MainWindow::~MainWindow() {
    stop();
    if (networkThread_.isRunning()) {
        // Allow the bounded final notification to drain when quitting too.
        QMetaObject::invokeMethod(receiver_, [this] {
            QTimer::singleShot(200, receiver_, [this] { networkThread_.quit(); });
        }, Qt::QueuedConnection);
        networkThread_.wait();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("port"), port_->value());
    settings.setValue(QStringLiteral("device"), devices_->currentData());
    settings.setValue(QStringLiteral("gain"), gain_->value());
    settings.setValue(QStringLiteral("referenceDevice"), referenceDevices_->currentData());

}

void MainWindow::showAbout() {
    // Modeless: a user can inspect project information while audio keeps running.
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("关于 MineMic"));
    dialog->resize(490, 360);
    auto* layout = new QVBoxLayout(dialog);
    auto* info = new QTextBrowser(dialog);
    info->setOpenExternalLinks(true);
    info->setHtml(tr(
        "<h2>MineMic-PC</h2>"
        "<p><b>版本：%1</b><br>开发预览 · 音频增强<br>音频协议：v2</p>"
        "<p>将手机作为电脑麦克风的局域网音频接收端。<br>C++17 + Qt Widgets，无需云服务器。</p>"
        "<p>作者：LeeC<br>原创代码许可：MIT License<br>© 2026 LeeC</p>"
        "<p><a href=\"https://github.com/LeecOVO/MineMic-PC\">PC 项目 / 问题反馈</a> · "
        "<a href=\"https://github.com/LeecOVO/MineMic\">手机项目</a> · "
        "<a href=\"https://github.com/LeecOVO/MineMic-PC/blob/master/LICENSE\">MIT 协议</a></p>"
        "<p>系统麦克风路由需另行安装虚拟声卡。Qt 与第三方驱动各自遵循其许可证。</p>"
    ).arg(QApplication::applicationVersion().toHtmlEscaped()));
    layout->addWidget(info);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}

void MainWindow::refreshDevices() {
    const auto previous = devices_->currentData().toString();
    const auto preferred = previous.isEmpty() ? QSettings().value(QStringLiteral("device")).toString() : previous;
    const auto referencePreferred = referenceDevices_->currentIndex() < 0
        ? QSettings().value(QStringLiteral("referenceDevice")).toString() : referenceDevices_->currentData().toString();
    const QSignalBlocker referenceSignals(referenceDevices_);
    referenceDevices_->clear(); referenceDevices_->addItem(tr("自动 · 跟随系统默认外放设备（推荐）"), QString());
    devices_->clear(); devices_->addItem(tr("请选择输出设备"), QString());
    QString error;
    for (const auto& device : output_->devices(error)) {
        devices_->addItem(device.name, device.id); referenceDevices_->addItem(device.name, device.id);
    }
    const int referenceIndex = referenceDevices_->findData(referencePreferred);
    if (referenceIndex > 0) referenceDevices_->setCurrentIndex(referenceIndex);
    const auto referenceId = referenceDevices_->currentData().toString();
    QSettings().setValue(QStringLiteral("referenceDevice"), referenceId);
    QMetaObject::invokeMethod(receiver_, [this, referenceId] { receiver_->setReferenceDevice(referenceId); }, Qt::QueuedConnection);
    const int index = devices_->findData(preferred);
    if (index > 0) devices_->setCurrentIndex(index);
    if (!error.isEmpty()) status_->setText(error);
}

void MainWindow::updateConnectionInfo() {
    connection_->setText(pairingCode_);
    connection_->setCursorPosition(0);
}

void MainWindow::setRunning(bool running) {
    running_ = running; start_->setEnabled(!busy_);
    start_->setText(running ? tr("停止接收") : tr("开始接收"));
    devices_->setEnabled(!running && !busy_); refresh_->setEnabled(!running && !busy_);
    #ifdef Q_OS_WIN
    referenceDevices_->setEnabled(!busy_);
#endif
    port_->setEnabled(false); disconnect_->setEnabled(running && locked_);
    if (tray_) tray_->setToolTip(running ? tr("MineMic · 接收中") : QStringLiteral("MineMic"));
}

void MainWindow::start() {
    if (busy_ || running_) return;
    const auto id = devices_->currentData().toString();
    if (id.isEmpty()) { status_->setText(tr("请先选择虚拟声卡播放端，或选择扬声器进行试听。")); return; }
    buffer_.reset();
    QString error;
    if (!output_->start(id, buffer_, error)) { showError(error); return; }
    busy_ = true; setRunning(false); status_->setText(tr("正在启动接收…"));
    const auto port = static_cast<quint16>(port_->value());
    const auto referenceId = referenceDevices_->currentData().toString();
    QMetaObject::invokeMethod(receiver_, [this, port, referenceId] {
        receiver_->setReferenceDevice(referenceId); receiver_->startListening(port);
    }, Qt::QueuedConnection);
}

void MainWindow::stop() {
    QString stoppedCode;
    if (networkThread_.isRunning()) QMetaObject::invokeMethod(receiver_, [this, &stoppedCode] {
        receiver_->stopListening(); stoppedCode = receiver_->pairingCode();
    }, Qt::BlockingQueuedConnection);
    // During destruction the UI event loop may no longer deliver code-change signals.
    if (!stoppedCode.isEmpty()) {
        pairingCode_ = stoppedCode; updateConnectionInfo();
        QSettings().setValue(QStringLiteral("pairingCode"), stoppedCode);
    }
    output_->stop(); buffer_.reset(); busy_ = false; setRunning(false);
    meter_->setValue(0); status_->setText(tr("已停止"));
}

void MainWindow::showError(const QString& error) {
    // Device failures must not release a connected phone's ownership.
    output_->stop(); busy_ = false; setRunning(running_);
    status_->setText(tr("音频或网络异常：%1。已连接手机的占用保留，需手动断开。").arg(error));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (tray_ && tray_->isVisible()) { hide(); event->ignore(); }
    else { stop(); event->accept(); }
}
}
