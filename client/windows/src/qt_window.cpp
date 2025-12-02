#include "qt_window.hpp"

#include <algorithm>
#include <unordered_map>
#include <QAbstractItemView>
#include <QAbstractAnimation>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QGraphicsOpacityEffect>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QLinearGradient>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QDateTime>
#include <QDate>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QPropertyAnimation>
#include <QPoint>
#include <QTextCursor>
#include <QMenu>
#include <QAction>
#include <QPushButton>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QShortcut>
#include <QMouseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <deque>
#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QUrl>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>
#include <QVariant>
#include <QElapsedTimer>
#include <QProcess>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QNetworkConfigurationManager>
#include <QNetworkSession>
#include <QNetworkConfiguration>
#endif
#ifdef MI_ENABLE_QTMULTIMEDIA
#include <QMediaPlayer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QVideoFrame>
#include <QVideoSink>
#else
#include <QVideoWidget>
#endif
#endif

#include "client/client_runner.hpp"

namespace
{
bool ParseHostPort(const QString& text, std::wstring& host, uint16_t& port)
{
    const int idx = text.lastIndexOf(':');
    if (idx <= 0)
    {
        return false;
    }
    host = text.left(idx).toStdWString();
    bool ok = false;
    const int p = text.mid(idx + 1).toInt(&ok);
    if (!ok || p <= 0 || p > 65535)
    {
        return false;
    }
    port = static_cast<uint16_t>(p);
    return true;
}

QString ModeLabel(mi::client::SendMode mode)
{
    switch (mode)
    {
    case mi::client::SendMode::Data:
        return QStringLiteral("data");
    case mi::client::SendMode::Both:
        return QStringLiteral("both");
    default:
        return QStringLiteral("chat");
    }
}
}  // namespace

// TODO: 当服务端开放 HTTP/WS 接口时，替换 SimulateSessionPullFromServer/FetchRemoteSessions 为真实调用
// 1) 拉取会话列表/在线状态 -> 更新 sessionItems_/lastSeen_
// 2) 订阅在线心跳/已读回执 -> 进入 OnPresenceTick/OnReadTick
// 3) 已读/送达协议 -> 将 UpdateMessageStatus 改为协议驱动

QtClientWindow::QtClientWindow(QWidget* parent)
    : QWidget(parent),
      statusLabel_(nullptr),
      serverEdit_(nullptr),
      userEdit_(nullptr),
      passEdit_(nullptr),
      messageEdit_(nullptr),
      mediaEdit_(nullptr),
    targetSpin_(nullptr),
    modeCombo_(nullptr),
    revokeCheck_(nullptr),
    reconnectSpin_(nullptr),
    reconnectDelaySpin_(nullptr),
      startButton_(nullptr),
      stopButton_(nullptr),
      browseButton_(nullptr),
      emojiButton_(nullptr),
      mediaProgress_(nullptr),
      mediaStatusLabel_(nullptr),
      speedStatusLabel_(nullptr),
      speedPeakLabel_(nullptr),
      speedSparkline_(nullptr),
      statsHistoryChart_(nullptr),
      statsRefreshButton_(nullptr),
      logEdit_(nullptr),
      messageView_(nullptr),
      feedList_(nullptr),
      sidebar_(nullptr),
      mainPanel_(nullptr),
      hSplit_(nullptr),
      sessionLabel_(nullptr),
      themeSwitch_(nullptr),
      accentSwitch_(nullptr),
      paletteGroupBox_(nullptr),
      accentInput_(nullptr),
      accentPaletteBox_(nullptr),
      accentAddButton_(nullptr),
      boldButton_(nullptr),
      italicButton_(nullptr),
      codeButton_(nullptr),
      networkStatusLabel_(nullptr),
      alertBanner_(nullptr),
      alertLabel_(nullptr),
      alertRetryButton_(nullptr),
      statusLabels_(),
      statusBadges_(),
      sessionItems_(),
      mediaPreviewCache_(),
      mediaOverlay_(),
      lastSeen_(),
      pendingMessages_(),
      unreadMessages_(),
      presenceTimer_(this),
      pendingTimer_(this),
      readTimer_(this),
      ackTimer_(this),
      sessionRefreshTimer_(this),
      darkTheme_(true),
      lastSenderKey_(),
      lastDateKey_(),
      accentColor_(QStringLiteral("#2563eb")),
      progressTimer_(),
      speedLogTimer_(),
      lastSpeedMbps_(0.0),
      lastProgressBytes_(0),
      networkManager_(std::make_unique<QNetworkAccessManager>(this)),
      emojiMenu_(nullptr),
      worker_(),
      cancelled_(false),
      cachedSessionList_(),
      customPalette_(),
      speedHistory_(),
      remoteThroughput_(),
      certBytes_(),
      certFingerprint_(),
      certPassword_(),
      certAllowSelfSigned_(true),
      paletteSwatchLayout_(nullptr),
      sidebarCollapsed_(false),
      activeGroupPalette_(),
      currentPaletteGroup_(QStringLiteral("默认合集")),
      speedHistoryPersisted_(),
      preserveHistoryNextRun_(false)
{
    BuildUi();
    ApplyStyle();
    connect(startButton_, &QPushButton::clicked, this, &QtClientWindow::OnStartClicked);
    connect(stopButton_, &QPushButton::clicked, this, &QtClientWindow::OnStopClicked);
    connect(browseButton_, &QPushButton::clicked, this, &QtClientWindow::OnBrowseMedia);
    connect(targetSpin_, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, [this]() { BootstrapSessionList(); });
    connect(&presenceTimer_, &QTimer::timeout, this, &QtClientWindow::OnPresenceTick);
    connect(&pendingTimer_, &QTimer::timeout, this, &QtClientWindow::OnPendingTick);
    connect(&readTimer_, &QTimer::timeout, this, &QtClientWindow::OnReadTick);
    connect(&ackTimer_, &QTimer::timeout, this, &QtClientWindow::OnAckTick);
    connect(&sessionRefreshTimer_, &QTimer::timeout, this, [this]() {
        if (networkStatusLabel_ && networkStatusLabel_->text().contains(QStringLiteral("在线")))
        {
            FetchRemoteSessions();
        }
    });
    // 模拟速率/失败统计上报节流，未来由协议层替换
    QTimer* rateTimer = new QTimer(this);
    connect(rateTimer, &QTimer::timeout, this, &QtClientWindow::OnRateTick);
    rateTimer->start(5000);
    QTimer* retryTimer = new QTimer(this);
    connect(retryTimer, &QTimer::timeout, this, &QtClientWindow::OnRetryTick);
    retryTimer->start(4000);
    presenceTimer_.start(5000);
    pendingTimer_.start(1500);
    readTimer_.start(2000);
    ackTimer_.start(3000);
    sessionRefreshTimer_.start(30'000);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (networkManager_)
    {
        auto* mgr = new QNetworkConfigurationManager(this);
        connect(mgr, &QNetworkConfigurationManager::onlineStateChanged, this, [this](bool online) {
            if (online)
            {
                AppendLog(QStringLiteral("[ui] 网络在线，尝试刷新会话列表"));
                FetchRemoteSessions();
            }
            else
            {
                AppendLog(QStringLiteral("[ui] 网络离线，使用本地会话缓存"));
            }
            if (networkStatusLabel_)
            {
                networkStatusLabel_->setText(online ? QStringLiteral("网络: 在线") : QStringLiteral("网络: 离线"));
                networkStatusLabel_->setStyleSheet(online ? QStringLiteral("color:#22c55e;")
                                                          : QStringLiteral("color:#f87171;"));
            }
        });
    }
#endif
    LoadDraft();
    BootstrapSessionList();
    FetchCertMemory();
    FetchStatsHistory();
}

QtClientWindow::~QtClientWindow()
{
    StopWorker();
}

void QtClientWindow::BuildUi()
{
    setWindowTitle(QStringLiteral("mi_client Qt UI"));

    sidebar_ = new QFrame(this);
    sidebar_->setObjectName(QStringLiteral("Sidebar"));
    auto sideLayout = new QVBoxLayout(sidebar_);
    auto title = new QLabel(QStringLiteral("Mi 聊天"));
    title->setObjectName(QStringLiteral("SidebarTitle"));
    statusLabel_ = new QLabel(QStringLiteral("状态：空闲"));
    auto connectionsLabel = new QLabel(QStringLiteral("会话列表"));
    feedList_ = new QListWidget(this);
    refreshSessions_ = new QPushButton(QStringLiteral("拉取会话"), this);
    connect(refreshSessions_, &QPushButton::clicked, this, [this]() {
        FetchRemoteSessions();
        SaveSettings();
    });
    QPushButton* exportSessions = new QPushButton(QStringLiteral("导出会话"), this);
    connect(exportSessions, &QPushButton::clicked, this, [this]() { ExportSessionSnapshot(); });
    QPushButton* importSessions = new QPushButton(QStringLiteral("导入会话"), this);
    connect(importSessions, &QPushButton::clicked, this, [this]() { ImportSessionSnapshot(); });
    accentInput_ = new QLineEdit(this);
    accentInput_->setPlaceholderText(QStringLiteral("#RRGGBB 自定义色"));
    QPushButton* applyAccentBtn = new QPushButton(QStringLiteral("应用色"), this);
    connect(applyAccentBtn, &QPushButton::clicked, this, [this]() {
        const QString val = accentInput_->text().trimmed();
        if (!val.startsWith('#') || val.length() < 4)
        {
            return;
        }
        accentColor_ = val;
        ApplyTheme();
        SaveSettings();
    });
    paletteSwatchLayout_ = new QHBoxLayout();
    paletteSwatchLayout_->setContentsMargins(0, 4, 0, 4);
    paletteSwatchLayout_->setSpacing(6);
    auto* paletteSwatchContainer = new QWidget(this);
    paletteSwatchContainer->setLayout(paletteSwatchLayout_);
    auto* palettePreviewLabel = new QLabel(QStringLiteral("收藏色预览"), this);

    sideLayout->addWidget(title);
    sideLayout->addSpacing(4);
    sideLayout->addWidget(statusLabel_);
    sideLayout->addSpacing(10);
    sideLayout->addWidget(connectionsLabel);
    sideLayout->addWidget(refreshSessions_);
    sideLayout->addWidget(exportSessions);
    sideLayout->addWidget(importSessions);
    QPushButton* markAllRead = new QPushButton(QStringLiteral("全部已读"), this);
    connect(markAllRead, &QPushButton::clicked, this, &QtClientWindow::MarkAllRead);
    sideLayout->addWidget(markAllRead);
    sideLayout->addWidget(palettePreviewLabel);
    sideLayout->addWidget(paletteSwatchContainer);
    sideLayout->addWidget(feedList_);
    sideLayout->addStretch();
    connect(feedList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item)
        {
            return;
        }
        QRegularExpression re(QStringLiteral("(\\d+)"));
        auto m = re.match(item->text());
        if (m.hasMatch())
        {
            targetSpin_->setValue(m.captured(1).toInt());
            sessionLabel_->setText(QStringLiteral("目标: %1").arg(m.captured(1)));
            SaveSettings();
            unreadCount_[m.captured(1)] = 0;
            UpdateSessionPresence(m.captured(1));
        }
    });

    serverEdit_ = new QLineEdit(QStringLiteral("127.0.0.1:7845"), this);
    userEdit_ = new QLineEdit(QStringLiteral("user"), this);
    passEdit_ = new QLineEdit(QStringLiteral("pass"), this);
    passEdit_->setEchoMode(QLineEdit::Password);
    messageEdit_ = new QPlainTextEdit(QStringLiteral("secure_payload"), this);
    mediaEdit_ = new QLineEdit(this);
    targetSpin_ = new QSpinBox(this);
    targetSpin_->setRange(0, 1'000'000'000);
    modeCombo_ = new QComboBox(this);
    revokeCheck_ = new QCheckBox(QStringLiteral("接收后自动撤回"), this);
    startButton_ = new QPushButton(QStringLiteral("开始发送"), this);
    stopButton_ = new QPushButton(QStringLiteral("停止"), this);
    stopButton_->setObjectName(QStringLiteral("StopButton"));
    browseButton_ = new QPushButton(QStringLiteral("选择媒体"), this);
    emojiButton_ = new QPushButton(QStringLiteral("表情"), this);
    emojiButton_->setFixedWidth(60);
    boldButton_ = new QPushButton(QStringLiteral("B"), this);
    boldButton_->setFixedWidth(36);
    italicButton_ = new QPushButton(QStringLiteral("I"), this);
    italicButton_->setFixedWidth(36);
    codeButton_ = new QPushButton(QStringLiteral("`"), this);
    codeButton_->setFixedWidth(36);
    mediaProgress_ = new QProgressBar(this);
    mediaProgress_->setRange(0, 100);
    mediaProgress_->setValue(0);
    mediaProgress_->setTextVisible(true);
    mediaProgress_->setFormat(QStringLiteral("准备就绪"));
    mediaStatusLabel_ = new QLabel(QStringLiteral("媒体进度：等待开始"), this);
    speedStatusLabel_ = new QLabel(QStringLiteral("速率：--"), this);
    speedPeakLabel_ = new QLabel(QStringLiteral("峰值：--"), this);
    speedSparkline_ = new QLabel(this);
    speedSparkline_->setFixedHeight(28);
    speedSparkline_->setFixedWidth(120);
    statsHistoryChart_ = new QLabel(this);
    statsHistoryChart_->setObjectName(QStringLiteral("StatsChart"));
    statsHistoryChart_->setFixedHeight(120);
    statsHistoryChart_->setMinimumWidth(260);
    statsHistoryChart_->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    statsRefreshButton_ = new QPushButton(QStringLiteral("拉取统计"), this);
    statsRefreshButton_->setObjectName(QStringLiteral("StatsRefresh"));
    connect(statsRefreshButton_, &QPushButton::clicked, this, &QtClientWindow::FetchStatsHistory);
    logEdit_ = new QPlainTextEdit(this);
    logEdit_->setReadOnly(true);

    modeCombo_->addItem(QStringLiteral("聊天"), QVariant::fromValue(static_cast<int>(mi::client::SendMode::Chat)));
    modeCombo_->addItem(QStringLiteral("数据"), QVariant::fromValue(static_cast<int>(mi::client::SendMode::Data)));
    modeCombo_->addItem(QStringLiteral("双发"), QVariant::fromValue(static_cast<int>(mi::client::SendMode::Both)));

    mainPanel_ = new QFrame(this);
    mainPanel_->setObjectName(QStringLiteral("MainPanel"));
    auto formLayout = new QVBoxLayout(mainPanel_);
    auto headerRow = new QHBoxLayout();
    auto headline = new QLabel(QStringLiteral("端到端加密聊天"));
    headline->setObjectName(QStringLiteral("Headline"));
    headerRow->addWidget(headline);
    headerRow->addStretch();
    toggleSidebarButton_ = new QPushButton(QStringLiteral("折叠侧栏"), this);
    toggleSidebarButton_->setObjectName(QStringLiteral("SidebarToggle"));
    toggleSidebarButton_->setFixedWidth(90);
    connect(toggleSidebarButton_, &QPushButton::clicked, this, &QtClientWindow::ToggleSidebar);
    headerRow->addWidget(toggleSidebarButton_);
    headerRow->addSpacing(6);
    networkStatusLabel_ = new QLabel(QStringLiteral("网络: 未知"));
    networkStatusLabel_->setObjectName(QStringLiteral("StatusPill"));
    headerRow->addWidget(networkStatusLabel_);
    headerRow->addSpacing(6);
    themeSwitch_ = new QComboBox(this);
    themeSwitch_->addItem(QStringLiteral("深色"), QVariant::fromValue(1));
    themeSwitch_->addItem(QStringLiteral("浅色"), QVariant::fromValue(0));
    accentSwitch_ = new QComboBox(this);
    accentSwitch_->addItem(QStringLiteral("蓝"), QVariant::fromValue(QStringLiteral("#2563eb")));
    accentSwitch_->addItem(QStringLiteral("绿"), QVariant::fromValue(QStringLiteral("#22c55e")));
    accentSwitch_->addItem(QStringLiteral("橙"), QVariant::fromValue(QStringLiteral("#fb923c")));
    accentSwitch_->addItem(QStringLiteral("紫"), QVariant::fromValue(QStringLiteral("#a855f7")));
    accentSwitch_->addItem(QStringLiteral("红"), QVariant::fromValue(QStringLiteral("#ef4444")));
    paletteGroupBox_ = new QComboBox(this);
    paletteGroupBox_->addItem(QStringLiteral("默认合集"));
    paletteGroupBox_->addItem(QStringLiteral("活力"));
    paletteGroupBox_->addItem(QStringLiteral("冷静"));
    paletteGroupBox_->addItem(QStringLiteral("自然"));
    accentPaletteBox_ = new QComboBox(this);
    accentPaletteBox_->setMinimumWidth(80);
    accentPaletteBox_->setPlaceholderText(QStringLiteral("收藏色"));
    accentAddButton_ = new QPushButton(QStringLiteral("收藏"), this);
    accentAddButton_->setFixedWidth(52);
    connect(themeSwitch_, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        darkTheme_ = themeSwitch_->itemData(idx).toInt() == 1;
        ApplyTheme();
    });
    connect(accentSwitch_, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        accentColor_ = accentSwitch_->itemData(idx).toString();
        ApplyTheme();
        SaveSettings();
    });
    accentInput_ = new QLineEdit(this);
    accentInput_->setPlaceholderText(QStringLiteral("#RRGGBB"));
    connect(accentInput_, &QLineEdit::returnPressed, this, [this]() {
        const QString val = accentInput_->text().trimmed();
        if (!val.startsWith('#') || val.size() < 4)
        {
            return;
        }
        accentColor_ = val;
        ApplyTheme();
        SaveSettings();
    });
    connect(accentAddButton_, &QPushButton::clicked, this, [this]() {
        const QString val = accentInput_->text().trimmed();
        if (!val.startsWith('#') || val.size() < 4)
        {
            return;
        }
        if (std::find(customPalette_.begin(), customPalette_.end(), val) == customPalette_.end())
        {
            customPalette_.push_back(val);
            accentPaletteBox_->addItem(val, val);
            RefreshPaletteSwatches();
        }
    });
    connect(accentPaletteBox_, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0)
        {
            return;
        }
        const QString val = accentPaletteBox_->itemData(idx).toString();
        if (val.startsWith('#'))
        {
            accentColor_ = val;
            accentInput_->setText(val);
            ApplyTheme();
        }
    });
    connect(paletteGroupBox_, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        const QString key = paletteGroupBox_->itemText(idx);
        currentPaletteGroup_ = key;
        static const QMap<QString, QStringList> groups = {
            {QStringLiteral("默认合集"), {QStringLiteral("#2563eb"), QStringLiteral("#22c55e"), QStringLiteral("#f59e0b")}},
            {QStringLiteral("活力"), {QStringLiteral("#ef4444"), QStringLiteral("#f97316"), QStringLiteral("#a855f7")}},
            {QStringLiteral("冷静"), {QStringLiteral("#0ea5e9"), QStringLiteral("#6366f1"), QStringLiteral("#14b8a6")}},
            {QStringLiteral("自然"), {QStringLiteral("#16a34a"), QStringLiteral("#65a30d"), QStringLiteral("#0ea5e9")}},
        };
        activeGroupPalette_ = groups.value(key, groups.first());
        if (!activeGroupPalette_.isEmpty())
        {
            accentColor_ = activeGroupPalette_.front();
            accentInput_->setText(accentColor_);
            ApplyTheme();
        }
        RefreshPaletteSwatches();
        SaveSettings();
    });
    headerRow->addWidget(themeSwitch_);
    headerRow->addWidget(accentSwitch_);
    headerRow->addWidget(paletteGroupBox_);
    headerRow->addWidget(accentInput_);
    headerRow->addWidget(accentAddButton_);
    headerRow->addWidget(accentPaletteBox_);
    headerRow->addSpacing(6);
    sessionLabel_ = new QLabel(QStringLiteral("目标: 自己"));
    sessionLabel_->setObjectName(QStringLiteral("StatusPill"));
    headerRow->addWidget(sessionLabel_);
    headerRow->addSpacing(8);
    headerRow->addWidget(new QLabel(QStringLiteral("模式")));
    headerRow->addWidget(modeCombo_);
    formLayout->addLayout(headerRow);
    alertBanner_ = new QFrame(this);
    alertBanner_->setObjectName(QStringLiteral("AlertBanner"));
    auto* alertLayout = new QHBoxLayout(alertBanner_);
    alertLayout->setContentsMargins(10, 6, 10, 6);
    alertLayout->setSpacing(8);
    alertLabel_ = new QLabel(QStringLiteral(""), alertBanner_);
    alertLabel_->setObjectName(QStringLiteral("AlertLabel"));
    alertRetryButton_ = new QPushButton(QStringLiteral("重试"), alertBanner_);
    alertRetryButton_->setObjectName(QStringLiteral("AlertRetry"));
    alertRetryButton_->setVisible(false);
    connect(alertRetryButton_, &QPushButton::clicked, this, [this]() {
        preserveHistoryNextRun_ = true;
        HideErrorBanner();
        OnStartClicked();
    });
    alertLayout->addWidget(alertLabel_);
    alertLayout->addStretch();
    alertLayout->addWidget(alertRetryButton_);
    alertBanner_->setVisible(false);
    formLayout->addWidget(alertBanner_);
    auto topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(QStringLiteral("服务器")));
    topRow->addWidget(serverEdit_);
    topRow->addWidget(new QLabel(QStringLiteral("目标 Session")));
    topRow->addWidget(targetSpin_);
    formLayout->addLayout(topRow);

    auto credRow = new QHBoxLayout();
    credRow->addWidget(new QLabel(QStringLiteral("用户名")));
    credRow->addWidget(userEdit_);
    credRow->addWidget(new QLabel(QStringLiteral("密码")));
    credRow->addWidget(passEdit_);
    formLayout->addLayout(credRow);

    auto modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel(QStringLiteral("撤回选项")));
    modeRow->addWidget(revokeCheck_);
    reconnectSpin_ = new QSpinBox(this);
    reconnectSpin_->setRange(0, 10);
    reconnectSpin_->setValue(2);
    reconnectDelaySpin_ = new QSpinBox(this);
    reconnectDelaySpin_->setRange(100, 10000);
    reconnectDelaySpin_->setValue(2000);
    modeRow->addWidget(new QLabel(QStringLiteral("重连次数")));
    modeRow->addWidget(reconnectSpin_);
    modeRow->addWidget(new QLabel(QStringLiteral("间隔ms")));
    modeRow->addWidget(reconnectDelaySpin_);
    modeRow->addStretch();
    formLayout->addLayout(modeRow);

    auto mediaRow = new QHBoxLayout();
    mediaRow->addWidget(new QLabel(QStringLiteral("媒体文件")));
    mediaRow->addWidget(mediaEdit_);
    mediaRow->addWidget(browseButton_);
    formLayout->addLayout(mediaRow);
    auto mediaProgressRow = new QHBoxLayout();
    mediaProgressRow->addWidget(mediaProgress_, 3);
    mediaProgressRow->addWidget(mediaStatusLabel_, 2);
    mediaProgressRow->addWidget(speedStatusLabel_, 1);
    mediaProgressRow->addWidget(speedPeakLabel_, 1);
    mediaProgressRow->addWidget(statsRefreshButton_, 1);
    formLayout->addLayout(mediaProgressRow);
    auto statsRow = new QHBoxLayout();
    statsRow->addWidget(statsHistoryChart_, 4);
    statsRow->addWidget(speedSparkline_, 1);
    formLayout->addLayout(statsRow);

    formLayout->addWidget(new QLabel(QStringLiteral("消息内容")));
    formLayout->addWidget(messageEdit_);

    formLayout->addWidget(new QLabel(QStringLiteral("对话流")));
    messageView_ = new QListWidget(this);
    messageView_->setFrameShape(QFrame::NoFrame);
    messageView_->setSpacing(8);
    messageView_->setSelectionMode(QAbstractItemView::NoSelection);
    messageView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    formLayout->addWidget(messageView_);

    auto actionRow = new QHBoxLayout();
    actionRow->addWidget(emojiButton_);
    actionRow->addWidget(boldButton_);
    actionRow->addWidget(italicButton_);
    actionRow->addWidget(codeButton_);
    actionRow->addStretch();
    actionRow->addWidget(startButton_);
    actionRow->addWidget(stopButton_);
    formLayout->addLayout(actionRow);

    formLayout->addWidget(new QLabel(QStringLiteral("运行日志")));
    formLayout->addWidget(logEdit_);

    hSplit_ = new QSplitter(Qt::Horizontal, this);
    hSplit_->addWidget(sidebar_);
    hSplit_->addWidget(mainPanel_);
    hSplit_->setStretchFactor(0, 1);
    hSplit_->setStretchFactor(1, 3);

    auto root = new QHBoxLayout(this);
    root->addWidget(hSplit_);
    setLayout(root);
    resize(860, 620);

    auto* sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(sendShortcut, &QShortcut::activated, this, &QtClientWindow::OnStartClicked);
    auto* sendShortcut2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), this);
    connect(sendShortcut2, &QShortcut::activated, this, &QtClientWindow::OnStartClicked);
    emojiMenu_ = new QMenu(this);
    const QStringList emojis = {QStringLiteral("😀"), QStringLiteral("😉"), QStringLiteral("😂"), QStringLiteral("🥰"),
                                QStringLiteral("👍"), QStringLiteral("🎉"), QStringLiteral("🔥"), QStringLiteral("❤️")};
    for (const auto& e : emojis)
    {
        QAction* act = emojiMenu_->addAction(e);
        connect(act, &QAction::triggered, this, [this, e]() {
            if (messageEdit_)
            {
                messageEdit_->insertPlainText(e);
            }
        });
    }
    connect(emojiButton_, &QPushButton::clicked, this, [this]() {
        if (emojiMenu_)
        {
            emojiMenu_->exec(QCursor::pos());
        }
    });
    connect(boldButton_, &QPushButton::clicked, this, [this]() { ApplyFormatting(QStringLiteral("**"), QStringLiteral("**")); });
    connect(italicButton_, &QPushButton::clicked, this, [this]() { ApplyFormatting(QStringLiteral("_"), QStringLiteral("_")); });
    connect(codeButton_, &QPushButton::clicked, this, [this]() { ApplyFormatting(QStringLiteral("`"), QStringLiteral("`")); });

    auto* boldShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_B), this);
    connect(boldShortcut, &QShortcut::activated, this, [this]() { ApplyFormatting(QStringLiteral("**"), QStringLiteral("**")); });
    auto* italicShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_I), this);
    connect(italicShortcut, &QShortcut::activated, this, [this]() { ApplyFormatting(QStringLiteral("_"), QStringLiteral("_")); });
    auto* codeShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
    connect(codeShortcut, &QShortcut::activated, this, [this]() { ApplyFormatting(QStringLiteral("`"), QStringLiteral("`")); });
    LoadSettings();
    LoadStatsHistory();
    static const QMap<QString, QStringList> groups = {
        {QStringLiteral("默认合集"), {QStringLiteral("#2563eb"), QStringLiteral("#22c55e"), QStringLiteral("#f59e0b")}},
        {QStringLiteral("活力"), {QStringLiteral("#ef4444"), QStringLiteral("#f97316"), QStringLiteral("#a855f7")}},
        {QStringLiteral("冷静"), {QStringLiteral("#0ea5e9"), QStringLiteral("#6366f1"), QStringLiteral("#14b8a6")}},
        {QStringLiteral("自然"), {QStringLiteral("#16a34a"), QStringLiteral("#65a30d"), QStringLiteral("#0ea5e9")}},
    };
    activeGroupPalette_ = groups.value(currentPaletteGroup_, groups.first());
    int groupIdx = paletteGroupBox_->findText(currentPaletteGroup_);
    if (groupIdx >= 0)
    {
        paletteGroupBox_->setCurrentIndex(groupIdx);
    }
    else
    {
        paletteGroupBox_->setCurrentIndex(0);
        currentPaletteGroup_ = paletteGroupBox_->currentText();
    }
    RefreshPaletteSwatches();
    themeSwitch_->setCurrentIndex(darkTheme_ ? 0 : 1);
    int accentIdx = accentSwitch_->findData(accentColor_);
    if (accentIdx >= 0)
    {
        accentSwitch_->setCurrentIndex(accentIdx);
    }
    if (accentInput_ != nullptr)
    {
        accentInput_->setText(accentColor_);
    }
    ApplyTheme();
    if (sidebarCollapsed_)
    {
        ToggleSidebar();
    }
}

void QtClientWindow::ApplyStyle()
{
    ApplyTheme();
}

void QtClientWindow::ApplyTheme()
{
    const QString bg = darkTheme_ ? QStringLiteral("#0b1221") : QStringLiteral("#f8fafc");
    const QString fg = darkTheme_ ? QStringLiteral("#e2e8f0") : QStringLiteral("#0f172a");
    const QString panel = darkTheme_ ? QStringLiteral("#0f172a") : QStringLiteral("#ffffff");
    const QString border = darkTheme_ ? QStringLiteral("#1f2a3a") : QStringLiteral("#e2e8f0");
    const QString accent = accentColor_.isEmpty() ? QStringLiteral("#2563eb") : accentColor_;
    const QString sidebarGradTop = darkTheme_ ? QStringLiteral("#101827") : QStringLiteral("#f1f5f9");
    const QString sidebarGradBot = darkTheme_ ? QStringLiteral("#0a1120") : QStringLiteral("#e2e8f0");
    setStyleSheet(QStringLiteral(R"(
        QWidget { background-color: %1; color: %2; font-family: "Segoe UI"; }
        QFrame#Sidebar {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 %6, stop:1 %7);
            border: 1px solid %4;
            border-radius: 14px;
            padding: 14px;
            min-width: 220px;
        }
        QFrame#MainPanel {
            background: %3;
            border: 1px solid %4;
            border-radius: 14px;
            padding: 10px;
        }
        QLabel#SidebarTitle { font-size: 18px; font-weight: 700; color: #38bdf8; }
        QLabel#Headline { font-size: 20px; font-weight: 700; color: #cbd5e1; }
        QLabel#StatusPill {
            background: %3;
            border-radius: 10px;
            padding: 4px 10px;
            border: 1px solid %4;
            color: #38bdf8;
        }
        QPushButton#SidebarToggle { background: transparent; color: #cbd5e1; border: 1px solid %4; padding: 8px 10px; }
        QPushButton#SidebarToggle:hover { background: %4; }
        QListWidget { background: %3; border: 1px solid %4; border-radius: 8px; color: %2; }
        QListWidget::item:selected { background: %4; }
        QLineEdit, QPlainTextEdit, QComboBox, QSpinBox {
            background: %3;
            color: %2;
            border: 1px solid %4;
            border-radius: 10px;
            padding: 10px;
        }
        QPlainTextEdit { min-height: 140px; }
        QPushButton {
            background-color: %5;
            color: #ffffff;
            border: none;
            border-radius: 12px;
            padding: 12px 18px;
            font-weight: 600;
        }
        QPushButton:hover { background-color: %5; }
        QPushButton#StopButton { background-color: #dc2626; }
        QPushButton#StopButton:hover { background-color: #b91c1c; }
        QCheckBox { padding-left: 4px; }
        QScrollBar:vertical { background: %1; width: 10px; }
        QLabel#Avatar {
            background: %3;
            border-radius: 16px;
            min-width: 32px;
            min-height: 32px;
            max-width: 32px;
            max-height: 32px;
            font-weight: 700;
            color: #38bdf8;
        }
        QFrame#BubbleOutbound {
            background: %5;
            border-radius: 12px;
            padding: 6px;
        }
        QFrame#BubbleInbound {
            background: %3;
            border: 1px solid %4;
            border-radius: 12px;
            padding: 6px;
        }
        QLabel#BubbleTitle { font-weight: 700; color: %2; }
        QLabel#BubbleBody { color: %2; }
        QLabel#BubbleMeta { color: #94a3b8; font-size: 11px; }
        QFrame#AlertBanner {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 %5, stop:1 %6);
            border: 1px solid %4;
            border-radius: 12px;
        }
        QLabel#AlertLabel { color: %2; font-weight: 600; }
        QPushButton#AlertRetry { background-color: %5; color: #ffffff; border-radius: 10px; padding: 6px 12px; }
        QLabel#StatsChart {
            background: %3;
            border: 1px solid %4;
            border-radius: 12px;
            padding: 6px;
        }
        QProgressBar {
            background: %3;
            border-radius: 10px;
            border: 1px solid %4;
            text-align: center;
            color: #cbd5e1;
        }
        QProgressBar::chunk {
            background-color: %5;
            border-radius: 10px;
        }
    )").arg(bg, fg, panel, border, accent, sidebarGradTop, sidebarGradBot));
    RenderStatsHistory();
    SaveSettings();
}

void QtClientWindow::FetchCertMemory()
{
    if (!networkManager_)
    {
        networkManager_ = std::make_unique<QNetworkAccessManager>(this);
    }
    QString host = serverEdit_ ? serverEdit_->text() : QStringLiteral("127.0.0.1:7845");
    if (!host.startsWith(QStringLiteral("http")))
    {
        host = QStringLiteral("http://") + host;
    }
    QUrl url(host + QStringLiteral("/cert"));
    if (!url.isValid())
    {
        AppendLog(QStringLiteral("[ui] 证书地址无效，跳过"));
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    auto* reply = networkManager_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> guard(reply);
        if (reply->error() != QNetworkReply::NoError)
        {
            AppendLog(QStringLiteral("[ui] 拉取证书失败: ") + reply->errorString());
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject())
        {
            AppendLog(QStringLiteral("[ui] 证书响应格式错误"));
            return;
        }
        const auto certVal = doc.object().value(QStringLiteral("cert"));
        if (!certVal.isString())
        {
            AppendLog(QStringLiteral("[ui] 证书字段缺失"));
            return;
        }
        const QByteArray decoded = QByteArray::fromBase64(certVal.toString().toUtf8());
        const auto shaVal = doc.object().value(QStringLiteral("sha256"));
        if (shaVal.isString())
        {
            certFingerprint_ = shaVal.toString();
        }
        const auto pwdVal = doc.object().value(QStringLiteral("password"));
        if (pwdVal.isString())
        {
            certPassword_ = pwdVal.toString();
        }
        else
        {
            certPassword_.clear();
        }
        const auto selfVal = doc.object().value(QStringLiteral("allowSelfSigned"));
        if (selfVal.isBool())
        {
            certAllowSelfSigned_ = selfVal.toBool();
        }
        else
        {
            certAllowSelfSigned_ = true;
        }
        ApplyCertMemory(decoded);
    });
}

void QtClientWindow::ApplyCertMemory(const QByteArray& cert)
{
    if (cert.isEmpty())
    {
        return;
    }
    certBytes_ = cert;
    if (networkStatusLabel_ && !certFingerprint_.isEmpty())
    {
        networkStatusLabel_->setText(QStringLiteral("证书: ") + certFingerprint_);
    }
    AppendLog(QStringLiteral("[ui] 已在内存加载证书 %1 字节（不落地）").arg(certBytes_.size()));
}

void QtClientWindow::AddChatBubble(const QString& content,
                                   const QString& title,
                                   const QString& meta,
                                   bool outbound,
                                   bool isMedia,
                                   std::uint64_t messageId,
                                   const std::vector<std::uint8_t>& payload,
                                   const std::vector<QString>& attachments,
                                   std::uint8_t format)
{
    if (messageView_ == nullptr)
    {
        return;
    }
    const QString senderKey = outbound ? QStringLiteral("me") : title;
    if (lastSenderKey_ != senderKey)
    {
        // 新发言者，插入时间分隔标签
        QListWidgetItem* divider = new QListWidgetItem(messageView_);
        divider->setFlags(Qt::NoItemFlags);
        QLabel* dividerLabel = new QLabel(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), messageView_);
        dividerLabel->setAlignment(Qt::AlignCenter);
        dividerLabel->setStyleSheet(QStringLiteral("color:#94a3b8; font-size:10px;"));
        divider->setSizeHint(dividerLabel->sizeHint());
        messageView_->addItem(divider);
        messageView_->setItemWidget(divider, dividerLabel);
    }
    QListWidgetItem* item = new QListWidgetItem(messageView_);
    QWidget* row = new QWidget(messageView_);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(8);

    const QString dateKey = QDate::currentDate().toString(Qt::ISODate);
    if (lastDateKey_ != dateKey)
    {
        QListWidgetItem* dateDivider = new QListWidgetItem(messageView_);
        dateDivider->setFlags(Qt::NoItemFlags);
        QLabel* dateLabel = new QLabel(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")), messageView_);
        dateLabel->setAlignment(Qt::AlignCenter);
        dateLabel->setStyleSheet(QStringLiteral("color:#94a3b8; font-size:11px;"));
        dateDivider->setSizeHint(dateLabel->sizeHint());
        messageView_->addItem(dateDivider);
        messageView_->setItemWidget(dateDivider, dateLabel);
        lastDateKey_ = dateKey;
    }

    QLabel* avatar = new QLabel(outbound ? QStringLiteral("我") : QStringLiteral("友"), row);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setObjectName(QStringLiteral("Avatar"));

    QFrame* bubble = new QFrame(row);
    bubble->setObjectName(outbound ? QStringLiteral("BubbleOutbound") : QStringLiteral("BubbleInbound"));
    auto* vbox = new QVBoxLayout(bubble);
    vbox->setContentsMargins(10, 8, 10, 8);
    vbox->setSpacing(4);

    QLabel* titleLabel = new QLabel(title, bubble);
    titleLabel->setObjectName(QStringLiteral("BubbleTitle"));
    QLabel* bodyLabel = new QLabel(content, bubble);
    bodyLabel->setTextFormat(Qt::RichText);
    bodyLabel->setWordWrap(true);
    bodyLabel->setObjectName(QStringLiteral("BubbleBody"));
    bodyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse | Qt::TextBrowserInteraction);
    bodyLabel->setOpenExternalLinks(true);
    if (format == 0)
    {
        bodyLabel->setText(ToHtml(content));
    }
    else
    {
        bodyLabel->setText(MarkdownToHtml(content));
    }
    QLabel* metaLabel = new QLabel(meta, bubble);
    metaLabel->setObjectName(QStringLiteral("BubbleMeta"));
    QLabel* statusBadge = new QLabel(QStringLiteral(""), bubble);
    statusBadge->setObjectName(QStringLiteral("StatusBadge"));
    statusBadge->setAlignment(Qt::AlignCenter);
    statusBadge->setStyleSheet(QStringLiteral("color:#cbd5e1; font-size:11px; padding:2px 6px; border-radius:8px;"));

    vbox->addWidget(titleLabel);
    vbox->addWidget(bodyLabel);
    if (!attachments.empty())
    {
        auto* attachRow = new QHBoxLayout();
        attachRow->setContentsMargins(0, 2, 0, 2);
        attachRow->setSpacing(6);
        for (const auto& att : attachments)
        {
            auto* chip = new QLabel(QStringLiteral("📎 ") + att, bubble);
            chip->setStyleSheet(QStringLiteral("color:#cbd5e1; background:rgba(255,255,255,0.04); padding:4px 8px; border-radius:10px;"));
            attachRow->addWidget(chip);
        }
        attachRow->addStretch();
        vbox->addLayout(attachRow);
    }
    if (!meta.isEmpty())
    {
        auto* metaRow = new QHBoxLayout();
        metaRow->setContentsMargins(0, 0, 0, 0);
        metaRow->setSpacing(8);
        metaRow->addWidget(metaLabel);
        metaRow->addStretch();
        metaRow->addWidget(statusBadge);
        vbox->addLayout(metaRow);
    }

    if (isMedia)
    {
        QPixmap pix;
        bool hasPreview = false;
        const bool videoLike = content.contains(QStringLiteral(".mp4"), Qt::CaseInsensitive) ||
                               content.contains(QStringLiteral(".mov"), Qt::CaseInsensitive) ||
                               content.contains(QStringLiteral(".mkv"), Qt::CaseInsensitive) ||
                               content.contains(QStringLiteral(".avi"), Qt::CaseInsensitive);
        if (outbound && !preparedMediaThumbs_.empty())
        {
            pix = preparedMediaThumbs_.front();
            preparedMediaThumbs_.pop_front();
            hasPreview = !pix.isNull();
        }
        if (messageId != 0)
        {
            const QString thumbPath =
                QDir(QStringLiteral("media_cache/thumbs")).filePath(QString::number(messageId) + QStringLiteral(".png"));
            if (QFileInfo::exists(thumbPath) && pix.load(thumbPath))
            {
                hasPreview = true;
            }
        }
        if (!hasPreview && !payload.empty())
        {
            QImage img;
            if (img.loadFromData(payload.data(), static_cast<int>(payload.size())))
            {
                pix = QPixmap::fromImage(img);
                const int maxW = 220;
                if (pix.width() > maxW)
                {
                    pix = pix.scaledToWidth(maxW, Qt::SmoothTransformation);
                }
                hasPreview = true;
            }
        }
        if (!hasPreview && videoLike)
        {
            pix = BuildVideoPreview(payload);
            hasPreview = true;
        }
        if (!hasPreview)
        {
            const bool likelyVideo =
                videoLike || (!payload.empty()) || (mediaSizes_.count(messageId) && mediaSizes_[messageId] > 3 * 1024 * 1024);
            pix = BuildPlaceholderPreview(likelyVideo ? QStringLiteral("视频") : QStringLiteral("媒体"));
            hasPreview = true;
        }

        QLabel* preview = new QLabel(bubble);
        preview->setAlignment(Qt::AlignCenter);
        preview->setPixmap(pix);
        preview->setCursor(Qt::PointingHandCursor);
        preview->installEventFilter(this);
        mediaPreviewCache_[preview] = pix;
        if (messageId != 0)
        {
            mediaPreviewById_[messageId] = preview;
        }
        vbox->addWidget(preview);
        if (messageId != 0 && hasPreview)
        {
            const QDir cacheDir(QStringLiteral("media_cache/thumbs"));
            cacheDir.mkpath(QStringLiteral("."));
            const QString thumbPath = cacheDir.filePath(QString::number(messageId) + QStringLiteral(".png"));
            pix.save(thumbPath, "PNG");
        }
    }

    if (outbound)
    {
        layout->addStretch();
        layout->addWidget(bubble, /*stretch*/ 4);
        if (lastSenderKey_ != senderKey)
        {
            layout->addWidget(avatar);
        }
    }
    else
    {
        if (lastSenderKey_ != senderKey)
        {
            layout->addWidget(avatar);
        }
        layout->addWidget(bubble, /*stretch*/ 4);
        layout->addStretch();
    }

    if (isMedia)
    {
        bubble->setProperty("isMedia", true);
    }
    if (!outbound)
    {
        const QString peerKey = title;
        unreadCount_[peerKey] += 1;
        UpdateSessionPresence(peerKey);
    }
    auto* shadow = new QGraphicsDropShadowEffect(bubble);
    shadow->setBlurRadius(18);
    shadow->setOffset(outbound ? 6 : -6, 10);
    shadow->setColor(QColor(0, 0, 0, 80));
    bubble->setGraphicsEffect(shadow);

    row->setLayout(layout);
    item->setSizeHint(row->sizeHint());
    messageView_->addItem(item);
    messageView_->setItemWidget(item, row);
    messageView_->scrollToBottom();
    lastSenderKey_ = senderKey;

    if (messageId != 0)
    {
        statusLabels_[messageId] = metaLabel;
        statusBadges_[messageId] = statusBadge;
        pendingMessages_[messageId] = QDateTime::currentDateTime();
        if (outbound)
        {
            UpdateMessageStatus(messageId, QStringLiteral("待回执"));
        }
        else
        {
            metaLabel->setStyleSheet(QStringLiteral("color:#fef08a; font-size:11px;"));
            UpdateMessageStatus(messageId, QStringLiteral("未读"));
            unreadMessages_[messageId] = QDateTime::currentDateTime();
        }
        if (isMedia)
        {
            auto sizeIt = mediaSizes_.find(messageId);
            if (sizeIt != mediaSizes_.end())
            {
                const double mb = static_cast<double>(sizeIt->second) / (1024.0 * 1024.0);
                metaLabel->setText(metaLabel->text() + QStringLiteral(" · %1 MB").arg(QString::number(mb, 'f', 2)));
            }
        }
    }

    const QPoint endPos = row->pos();
    const QPoint startPos = endPos + QPoint(outbound ? 24 : -24, 16);
    row->move(startPos);

    auto* effect = new QGraphicsOpacityEffect(row);
    row->setGraphicsEffect(effect);
    auto* anim = new QPropertyAnimation(effect, "opacity", row);
    anim->setDuration(160);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    auto* slide = new QPropertyAnimation(row, "pos", row);
    slide->setDuration(220);
    slide->setStartValue(startPos);
    slide->setEndValue(endPos);
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void QtClientWindow::UpdateMediaProgress(const mi::client::ClientCallbacks::ProgressEvent& progress)
{
    if (mediaProgress_ == nullptr || mediaStatusLabel_ == nullptr)
    {
        return;
    }
    const int percent = std::clamp(static_cast<int>(progress.value * 100.0), 0, 100);
    mediaProgress_->setValue(percent);
    const QString directionText =
        (progress.direction == mi::client::ClientCallbacks::Direction::Inbound) ? QStringLiteral("下载") : QStringLiteral("上传");
    QString format = QStringLiteral("%1 %2% · #%3").arg(directionText).arg(percent).arg(progress.mediaId);
    if (progress.totalChunks > 0)
    {
        format.append(QStringLiteral(" · %1/%2").arg(progress.chunkIndex).arg(progress.totalChunks));
    }
    if (progress.totalBytes > 0)
    {
        const double mb = static_cast<double>(progress.totalBytes) / (1024.0 * 1024.0);
        const double done = static_cast<double>(progress.bytesTransferred) / (1024.0 * 1024.0);
        format.append(QStringLiteral(" · %1/%2 MB").arg(QString::number(done, 'f', 2), QString::number(mb, 'f', 2)));
        mediaSizes_[progress.mediaId] = progress.totalBytes;
    }
    if (!progressTimer_.isValid())
    {
        progressTimer_.start();
        lastProgressBytes_ = progress.bytesTransferred;
        speedLogTimer_.invalidate();
        lastSpeedMbps_ = 0.0;
    }
    else
    {
        const qint64 elapsedMs = progressTimer_.elapsed();
        const std::uint64_t deltaBytes =
            (progress.bytesTransferred > lastProgressBytes_) ? (progress.bytesTransferred - lastProgressBytes_) : 0;
        if (elapsedMs > 0)
        {
            const double mbps = (static_cast<double>(deltaBytes) * 1000.0) / (static_cast<double>(elapsedMs) * 1024.0 * 1024.0);
            format.append(QStringLiteral(" · %1 MB/s").arg(QString::number(mbps, 'f', 2)));
            lastSpeedMbps_ = mbps;
            if (!speedLogTimer_.isValid())
            {
                speedLogTimer_.start();
            }
            else if (speedLogTimer_.elapsed() > 4000 && mbps > 0.01)
            {
                AppendLog(QStringLiteral("[ui] 速率 %1 MB/s").arg(QString::number(mbps, 'f', 2)));
                speedLogTimer_.restart();
            }
        }
        progressTimer_.restart();
        lastProgressBytes_ = progress.bytesTransferred;
    }
    mediaProgress_->setFormat(format);
    mediaStatusLabel_->setText(QStringLiteral("媒体进度：") + format);
    if (speedStatusLabel_)
    {
        speedStatusLabel_->setText(lastSpeedMbps_ > 0.0
                                       ? QStringLiteral("速率：%1 MB/s").arg(QString::number(lastSpeedMbps_, 'f', 2))
                                       : QStringLiteral("速率：--"));
    }
    // 记录媒体进度对应的会话
    if (progress.mediaId != 0 && !sessionLabel_->text().isEmpty())
    {
            UpdateMessageStatus(progress.mediaId, directionText == QStringLiteral("上传") ? QStringLiteral("传输中")
                                                                                          : QStringLiteral("接收中"));
        }
    }

void QtClientWindow::UpdateMessageStatus(std::uint64_t messageId, const QString& status)
{
    auto it = statusLabels_.find(messageId);
    if (it == statusLabels_.end() || it->second == nullptr)
    {
        return;
    }
    QLabel* label = it->second;
    if (label->text().contains(status))
    {
        return;
    }
    label->setText(label->text() + QStringLiteral(" · ") + status);

    auto badgeIt = statusBadges_.find(messageId);
    if (badgeIt != statusBadges_.end() && badgeIt->second != nullptr)
    {
        QLabel* badge = badgeIt->second;
        QString color = QStringLiteral("#94a3b8");
        if (status.contains(QStringLiteral("已读")) || status.contains(QStringLiteral("送达")))
        {
            color = QStringLiteral("#22c55e");
        }
        else if (status.contains(QStringLiteral("重试")))
        {
            color = QStringLiteral("#fbbf24");
        }
        else if (status.contains(QStringLiteral("失败")))
        {
            color = QStringLiteral("#f87171");
        }
        badge->setStyleSheet(QStringLiteral("color:%1; font-size:11px; padding:2px 6px; border-radius:8px; background:rgba(255,255,255,0.08);").arg(color));
        badge->setText(status);
    }
}

void QtClientWindow::UpdateSessionPresence(const QString& peer)
{
    if (peer.isEmpty() || feedList_ == nullptr)
    {
        return;
    }
    lastSeen_[peer] = QDateTime::currentDateTime();
    auto it = sessionItems_.find(peer);
    if (it == sessionItems_.end())
    {
        QListWidgetItem* item = new QListWidgetItem(QStringLiteral("会话 %1").arg(peer), feedList_);
        item->setForeground(QColor("#38bdf8"));
        sessionItems_[peer] = item;
        feedList_->addItem(item);
    }
    else
    {
        if (it->second != nullptr)
        {
            const int unread = unreadCount_[peer];
            const QString badge = unread > 0 ? QStringLiteral(" · 未读 %1").arg(unread) : QString();
            it->second->setText(QStringLiteral("会话 %1 · 在线%2").arg(peer, badge));
            it->second->setForeground(QColor("#22c55e"));
        }
    }
}

bool QtClientWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease)
    {
        auto it = mediaPreviewCache_.find(qobject_cast<QLabel*>(watched));
        if (it != mediaPreviewCache_.end())
        {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("媒体预览"));
            auto* v = new QVBoxLayout(&dialog);
            QLabel* img = new QLabel(&dialog);
            img->setAlignment(Qt::AlignCenter);
            QPixmap pix = it->second;
            const int maxW = 720;
            const int maxH = 520;
            if (pix.width() > maxW || pix.height() > maxH)
            {
                pix = pix.scaled(maxW, maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            img->setPixmap(pix);
            v->addWidget(img);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, &dialog);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            v->addWidget(buttons);
            dialog.exec();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtClientWindow::MarkMediaRevoked(std::uint64_t messageId)
{
    auto it = statusLabels_.find(messageId);
    if (it == statusLabels_.end() || it->second == nullptr)
    {
        return;
    }
    QLabel* label = it->second;
    auto* badge = new QLabel(QStringLiteral("已撤回"), label->parentWidget());
    badge->setStyleSheet(QStringLiteral("color:#fca5a5; font-size:11px; background: rgba(12,16,32,0.6); padding:2px 6px; border-radius:6px;"));
    badge->move(label->x(), label->y());
    badge->show();
    mediaOverlay_[messageId] = badge;

    auto previewIt = mediaPreviewById_.find(messageId);
    if (previewIt != mediaPreviewById_.end() && !previewIt->second.isNull())
    {
        QLabel* preview = previewIt->second;
        const QPixmap current = preview->pixmap(Qt::ReturnByValue);
        QPixmap pix = !current.isNull() ? current.copy() : BuildPlaceholderPreview(QStringLiteral("已撤回"));
        QPainter painter(&pix);
        painter.fillRect(pix.rect(), QColor(0, 0, 0, 120));
        painter.setPen(QPen(QColor("#fca5a5")));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 12, QFont::Bold));
        painter.drawText(pix.rect(), Qt::AlignCenter, QStringLiteral("已撤回"));
        preview->setPixmap(pix);
    }
}

QPixmap QtClientWindow::BuildPlaceholderPreview(const QString& title) const
{
    QPixmap pix(220, 140);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    QLinearGradient grad(0, 0, 0, 140);
    grad.setColorAt(0, QColor(darkTheme_ ? "#1e293b" : "#e2e8f0"));
    grad.setColorAt(1, QColor(darkTheme_ ? "#0f172a" : "#cbd5e1"));
    p.fillRect(pix.rect(), grad);
    p.setPen(QColor("#38bdf8"));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 14, QFont::Bold));
    p.drawText(pix.rect(), Qt::AlignCenter, title);
    // Play icon overlay
    QPainterPath triangle;
    triangle.moveTo(pix.width() / 2 + 12, pix.height() / 2);
    triangle.lineTo(pix.width() / 2 - 8, pix.height() / 2 - 12);
    triangle.lineTo(pix.width() / 2 - 8, pix.height() / 2 + 12);
    triangle.closeSubpath();
    p.fillPath(triangle, QColor("#38bdf8"));
    return pix;
}

QPixmap QtClientWindow::BuildVideoPreview(const std::vector<std::uint8_t>& payload) const
{
    QPixmap pix(220, 140);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);

    auto pickByte = [&](std::size_t idx) -> std::uint8_t {
        if (payload.empty())
        {
            return static_cast<std::uint8_t>((idx * 73) % 255);
        }
        return payload[std::min<std::size_t>(idx % payload.size(), payload.size() - 1)];
    };

    const QColor a = QColor::fromHsl(pickByte(3), 180, darkTheme_ ? 90 : 140);
    const QColor b = QColor::fromHsl(pickByte(17), 200, darkTheme_ ? 70 : 120);
    QLinearGradient grad(0, 0, pix.width(), pix.height());
    grad.setColorAt(0.0, a);
    grad.setColorAt(1.0, b);
    painter.fillRect(pix.rect(), grad);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor("#0f172a"), 2));
    painter.drawRoundedRect(pix.rect().adjusted(6, 6, -6, -6), 10, 10);

    const int bars = 14;
    for (int i = 0; i < bars; ++i)
    {
        const int x = 12 + i * 12;
        const int h = 20 + static_cast<int>((pickByte(i + 20) % 80));
        const int y = pix.height() - h - 12;
        const QColor barColor = QColor::fromHsl((pickByte(i + 40) % 255), 180, darkTheme_ ? 150 : 120);
        painter.fillRect(QRect(x, y, 8, h), barColor);
    }

    QPainterPath triangle;
    triangle.moveTo(pix.width() / 2 + 12, pix.height() / 2);
    triangle.lineTo(pix.width() / 2 - 10, pix.height() / 2 - 16);
    triangle.lineTo(pix.width() / 2 - 10, pix.height() / 2 + 16);
    triangle.closeSubpath();
    painter.fillPath(triangle, QColor("#ffffff"));

    painter.setPen(QPen(QColor("#e2e8f0")));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 12, QFont::DemiBold));
    painter.drawText(QRect(10, 10, pix.width() - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("视频预览/占位"));
    return pix;
}

QPixmap QtClientWindow::PrepareMediaThumb(const QString& path) const
{
    QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    const bool videoLike = suffix == QStringLiteral("mp4") || suffix == QStringLiteral("mov") ||
                           suffix == QStringLiteral("mkv") || suffix == QStringLiteral("avi");
    if (info.exists() && !videoLike)
    {
        QImage img;
        if (img.load(path))
        {
            QPixmap pix = QPixmap::fromImage(img);
            if (pix.width() > 220)
            {
                pix = pix.scaledToWidth(220, Qt::SmoothTransformation);
            }
            return pix;
        }
    }
    // 尝试提取视频首帧
#ifdef MI_ENABLE_QTMULTIMEDIA
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QMediaPlayer player;
    QVideoSink sink;
    player.setVideoOutput(&sink);
    QImage frameImage;
    QEventLoop loop;
    connect(&sink, &QVideoSink::videoFrameChanged, &loop, [&](const QVideoFrame& frame) {
        if (!frame.isValid())
        {
            return;
        }
        QVideoFrame clone(frame);
        if (!clone.map(QVideoFrame::ReadOnly))
        {
            return;
        }
        frameImage = clone.toImage();
        clone.unmap();
        loop.quit();
    });
    player.setSource(QUrl::fromLocalFile(path));
    player.play();
    QTimer::singleShot(1200, &loop, &QEventLoop::quit);
    loop.exec();
    player.stop();
    if (!frameImage.isNull())
    {
        QPixmap pix = QPixmap::fromImage(frameImage);
        if (pix.width() > 220)
        {
            pix = pix.scaledToWidth(220, Qt::SmoothTransformation);
        }
        return pix;
    }
#endif
#endif

    // 若无 Qt 多媒体，尝试调用本地 ffmpeg 获取首帧
    if (videoLike)
    {
        const QString thumbDir = QStringLiteral("media_cache/thumbs");
        QDir().mkpath(thumbDir);
        const QString tmpPath = QDir(thumbDir).filePath(QStringLiteral("ffmpeg_tmp_%1.png").arg(QCoreApplication::applicationPid()));
        QProcess proc;
        proc.start(QStringLiteral("ffmpeg"),
                   {QStringLiteral("-y"),
                    QStringLiteral("-i"),
                    path,
                    QStringLiteral("-frames:v"),
                    QStringLiteral("1"),
                    QStringLiteral("-q:v"),
                    QStringLiteral("2"),
                    tmpPath});
        proc.waitForFinished(2000);
        QImage img;
        if (QFileInfo::exists(tmpPath) && img.load(tmpPath))
        {
            QPixmap pix = QPixmap::fromImage(img);
            if (pix.width() > 220)
            {
                pix = pix.scaledToWidth(220, Qt::SmoothTransformation);
            }
            QFile::remove(tmpPath);
            return pix;
        }
    }

    std::vector<std::uint8_t> seed;
    const QByteArray utf8 = path.toUtf8();
    seed.assign(utf8.begin(), utf8.end());
    QPixmap fallback = BuildVideoPreview(seed);
    QPainter overlay(&fallback);
    overlay.setPen(QPen(QColor("#fbbf24")));
    overlay.setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::Bold));
    overlay.drawText(fallback.rect(), Qt::AlignBottom | Qt::AlignHCenter, QStringLiteral("未提取首帧"));
    return fallback;
}

void QtClientWindow::LoadDraft()
{
    const QString path = QStringLiteral("chat_cache/draft.txt");
    QFile f(path);
    if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const QString data = QString::fromUtf8(f.readAll());
        if (messageEdit_ != nullptr && !data.isEmpty())
        {
            messageEdit_->setPlainText(data);
        }
    }
}

void QtClientWindow::SaveDraft()
{
    if (messageEdit_ == nullptr)
    {
        return;
    }
    QDir().mkpath(QStringLiteral("chat_cache"));
    const QString path = QStringLiteral("chat_cache/draft.txt");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        f.write(messageEdit_->toPlainText().toUtf8());
    }
}

void QtClientWindow::LoadSessionCache()
{
    const QString path = QStringLiteral("chat_cache/settings.json");
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
    {
        return;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
    {
        return;
    }
    const auto arr = doc.object().value(QStringLiteral("sessions"));
    if (!arr.isArray())
    {
        return;
    }
    for (const auto& v : arr.toArray())
    {
        if (!v.isObject())
        {
            continue;
        }
        const auto obj = v.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString();
        const QString name = obj.value(QStringLiteral("name")).toString(id);
        const bool online = obj.value(QStringLiteral("online")).toBool(false);
        const int unread = obj.value(QStringLiteral("unread")).toInt(0);
        if (id.isEmpty() || sessionItems_.find(id) != sessionItems_.end())
        {
            continue;
        }
        unreadCount_[id] = unread;
        const QString badge = unread > 0 ? QStringLiteral(" · 未读 %1").arg(unread) : QString();
        auto* item = new QListWidgetItem(QStringLiteral("%1 (%2)%3").arg(name, id, badge), feedList_);
        item->setForeground(online ? QColor("#22c55e") : QColor("#94a3b8"));
        feedList_->addItem(item);
        sessionItems_[id] = item;
        if (online)
        {
            lastSeen_[id] = QDateTime::currentDateTime();
        }
    }
}

void QtClientWindow::PersistSessionsToFile(const QJsonArray& arr)
{
    QDir().mkpath(QStringLiteral("chat_cache"));
    QFile out(QStringLiteral("chat_cache/sessions.json"));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        out.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }
    QFile settings(QStringLiteral("chat_cache/settings.json"));
    QJsonDocument base = settings.exists() && settings.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(settings.readAll())
                                                                                : QJsonDocument(QJsonObject());
    QJsonObject merged = base.isObject() ? base.object() : QJsonObject();
    merged.insert(QStringLiteral("sessions"), arr);
    QFile outSettings(QStringLiteral("chat_cache/settings.json"));
    if (outSettings.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        outSettings.write(QJsonDocument(merged).toJson(QJsonDocument::Compact));
    }
}

QJsonArray QtClientWindow::BuildDemoSessions() const
{
    QJsonArray demo;
    QJsonObject a;
    a.insert(QStringLiteral("id"), QStringLiteral("10001"));
    a.insert(QStringLiteral("name"), QStringLiteral("演示 A"));
    a.insert(QStringLiteral("online"), true);
    QJsonObject b;
    b.insert(QStringLiteral("id"), QStringLiteral("10002"));
    b.insert(QStringLiteral("name"), QStringLiteral("演示 B"));
    b.insert(QStringLiteral("online"), false);
    demo.append(a);
    demo.append(b);
    return demo;
}

void QtClientWindow::FetchRemoteSessions()
{
    if (!networkManager_)
    {
        networkManager_ = std::make_unique<QNetworkAccessManager>(this);
    }
    QString host = serverEdit_ ? serverEdit_->text() : QStringLiteral("127.0.0.1:7845");
    if (!host.startsWith(QStringLiteral("http")))
    {
        host = QStringLiteral("http://") + host;
    }
    QUrl url(host + QStringLiteral("/sessions"));
    if (!url.isValid())
    {
        SimulateSessionPullFromServer();
        LoadSessionCache();
        return;
    }
    AppendLog(QStringLiteral("[ui] 请求会话列表: ") + url.toString());
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    auto* reply = networkManager_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> guard(reply);
        if (reply->error() != QNetworkReply::NoError)
        {
            AppendLog(QStringLiteral("[ui] 拉取会话失败，回退本地: ") + reply->errorString());
            SimulateSessionPullFromServer();
            LoadSessionCache();
            return;
        }
        const QByteArray data = reply->readAll();
        const auto doc = QJsonDocument::fromJson(data);
        if (doc.isArray())
        {
            PersistSessionsToFile(doc.array());
            LoadSessionCache();
            SaveSettings();
            AppendLog(QStringLiteral("[ui] 已从远端加载 %1 个会话").arg(doc.array().size()));
            if (networkStatusLabel_)
            {
                networkStatusLabel_->setText(QStringLiteral("网络: 在线"));
                networkStatusLabel_->setStyleSheet(QStringLiteral("color:#22c55e;"));
            }
        }
        else if (doc.isObject() && doc.object().contains(QStringLiteral("sessions")) &&
                 doc.object().value(QStringLiteral("sessions")).isArray())
        {
            PersistSessionsToFile(doc.object().value(QStringLiteral("sessions")).toArray());
            LoadSessionCache();
            SaveSettings();
            AppendLog(QStringLiteral("[ui] 已从远端加载会话列表"));
            if (networkStatusLabel_)
            {
                networkStatusLabel_->setText(QStringLiteral("网络: 在线"));
                networkStatusLabel_->setStyleSheet(QStringLiteral("color:#22c55e;"));
            }
        }
        else
        {
            AppendLog(QStringLiteral("[ui] 会话响应格式不符，回退本地"));
            SimulateSessionPullFromServer();
            LoadSessionCache();
            if (networkStatusLabel_)
            {
                networkStatusLabel_->setText(QStringLiteral("网络: 回退本地"));
                networkStatusLabel_->setStyleSheet(QStringLiteral("color:#fbbf24;"));
            }
        }
    });
}

void QtClientWindow::SimulateSessionPullFromServer()
{
    // 模拟拉取：优先读取本地 sessions.json，没有则生成示例
    QFile f(QStringLiteral("chat_cache/sessions.json"));
    if (f.exists() && f.open(QIODevice::ReadOnly))
    {
        const auto doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isArray())
        {
            PersistSessionsToFile(doc.array());
        }
    }
    else
    {
        PersistSessionsToFile(BuildDemoSessions());
    }
}

void QtClientWindow::ExportSessionSnapshot() const
{
    QFileDialog dialog(nullptr, QStringLiteral("导出会话"), QStringLiteral("."), QStringLiteral("JSON (*.json)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.selectFile(QStringLiteral("sessions_export.json"));
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const QString path = dialog.selectedFiles().value(0);
    QFile in(QStringLiteral("chat_cache/sessions.json"));
    if (!in.exists() || !in.open(QIODevice::ReadOnly))
    {
        return;
    }
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        out.write(in.readAll());
    }
}

void QtClientWindow::ImportSessionSnapshot()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入会话"), QStringLiteral("."), QStringLiteral("JSON (*.json)"));
    if (path.isEmpty())
    {
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        return;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isArray())
    {
        PersistSessionsToFile(doc.array());
        LoadSessionCache();
        SaveSettings();
    }
}

void QtClientWindow::ApplySessionList(const std::vector<std::pair<std::uint32_t, std::wstring>>& sessions)
{
    cachedSessionList_ = sessions;
    for (auto it = sessionItems_.begin(); it != sessionItems_.end();)
    {
        QListWidgetItem* item = it->second;
        if (item != nullptr)
        {
            delete item;
        }
        it = sessionItems_.erase(it);
    }
    lastSeen_.clear();
    QJsonArray arr;
    for (const auto& item : sessions)
    {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), QString::number(item.first));
        obj.insert(QStringLiteral("name"), QString::fromWCharArray(item.second.c_str()));
        obj.insert(QStringLiteral("online"), true);
        arr.append(obj);
        lastSeen_[QString::number(item.first)] = QDateTime::currentDateTime();
    }
    PersistSessionsToFile(arr);
    LoadSessionCache();
    SaveSettings();
    AppendLog(QStringLiteral("[ui] 会话订阅更新：%1 项").arg(arr.size()));
    if (networkStatusLabel_)
    {
        networkStatusLabel_->setText(QStringLiteral("网络: 会话订阅 %1").arg(arr.size()));
        networkStatusLabel_->setStyleSheet(QStringLiteral("color:#22c55e;"));
    }
}

void QtClientWindow::ApplyStats(const mi::client::ClientCallbacks::StatsEvent& stats)
{
    const double durationSec = stats.durationMs > 0 ? (stats.durationMs / 1000.0) : 0.0;
    const double mbSent = static_cast<double>(stats.bytesSent) / (1024.0 * 1024.0);
    const double mbRecv = static_cast<double>(stats.bytesReceived) / (1024.0 * 1024.0);
    const double throughput = durationSec > 0.0 ? (mbSent + mbRecv) / durationSec : 0.0;
    QString text = QStringLiteral("速率：%1 MB/s · 出/入 %2/%3 MB · T=%4s · 重试 C%5/D%6/M%7 · 失败 C%8/D%9/M%10")
                       .arg(QString::number(throughput, 'f', 2))
                       .arg(QString::number(mbSent, 'f', 2))
                       .arg(QString::number(mbRecv, 'f', 2))
                       .arg(QString::number(durationSec, 'f', 2))
                       .arg(stats.chatAttempts)
                       .arg(stats.dataAttempts)
                       .arg(stats.mediaAttempts)
                       .arg(stats.chatFailures)
                       .arg(stats.dataFailures)
                       .arg(stats.mediaFailures);
    AppendLog(QStringLiteral("[ui] ") + text);
    if (speedStatusLabel_)
    {
        speedStatusLabel_->setText(text);
    }
    RenderSpeedSparkline(throughput);
    if (speedPeakLabel_)
    {
        const double peak = speedHistoryPersisted_.empty() ? throughput
                                                           : std::max(throughput, *std::max_element(speedHistoryPersisted_.begin(),
                                                                                                     speedHistoryPersisted_.end()));
        speedPeakLabel_->setText(QStringLiteral("峰值：%1 MB/s").arg(QString::number(peak, 'f', 2)));
    }
    speedHistoryPersisted_.push_back(throughput);
    if (speedHistoryPersisted_.size() > 200)
    {
        speedHistoryPersisted_.pop_front();
    }
    SaveStatsHistory();
    RenderStatsHistory();
    FetchStatsHistory();
    if (statusLabel_)
    {
        statusLabel_->setText(QStringLiteral("状态：完成 (%1 MB/s)").arg(QString::number(throughput, 'f', 2)));
    }
}

void QtClientWindow::ApplyFormatting(const QString& prefix, const QString& suffix)
{
    if (messageEdit_ == nullptr)
    {
        return;
    }
    QTextCursor cursor = messageEdit_->textCursor();
    const QString selected = cursor.selectedText();
    if (selected.isEmpty())
    {
        cursor.insertText(prefix + suffix);
        cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, suffix.size());
        messageEdit_->setTextCursor(cursor);
        return;
    }
    cursor.insertText(prefix + selected + suffix);
    messageEdit_->setTextCursor(cursor);
}

void QtClientWindow::RenderSpeedSparkline(double throughputMbps)
{
    if (speedSparkline_ == nullptr)
    {
        return;
    }
    const double clamped = std::max(0.0, throughputMbps);
    speedHistory_.push_back(clamped);
    if (speedHistory_.size() > 30)
    {
        speedHistory_.pop_front();
    }
    const int width = speedSparkline_->width();
    const int height = speedSparkline_->height();
    QPixmap pix(width, height);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    const QColor bg = darkTheme_ ? QColor("#0f172a") : QColor("#e2e8f0");
    const QColor bar = QColor(accentColor_.isEmpty() ? "#22c55e" : accentColor_);
    p.fillRect(pix.rect(), bg);
    const int count = static_cast<int>(speedHistory_.size());
    if (count > 0)
    {
        const double maxVal = *std::max_element(speedHistory_.begin(), speedHistory_.end());
        const double scale = (maxVal > 0.0) ? (static_cast<double>(height - 4) / maxVal) : 0.0;
        const int barWidth = std::max(2, width / std::max(1, count));
        for (int i = 0; i < count; ++i)
        {
            const double v = speedHistory_[i];
            const int h = static_cast<int>(v * scale);
            const int x = i * barWidth;
            p.fillRect(QRect(x, height - h - 2, barWidth - 1, h), bar);
        }
    }
    speedSparkline_->setPixmap(pix);
}

void QtClientWindow::RenderStatsHistory()
{
    if (statsHistoryChart_ == nullptr)
    {
        return;
    }
    const int width = statsHistoryChart_->width();
    const int height = statsHistoryChart_->height();
    QPixmap pix(width, height);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    const QColor bg = darkTheme_ ? QColor("#0f172a") : QColor("#e2e8f0");
    const QColor grid = darkTheme_ ? QColor(255, 255, 255, 24) : QColor(15, 23, 42, 36);
    const QColor line = QColor(accentColor_.isEmpty() ? "#38bdf8" : accentColor_);
    p.fillRect(pix.rect(), bg);
    const auto& data = !remoteThroughput_.empty() ? remoteThroughput_ : speedHistoryPersisted_;
    if (data.empty())
    {
        p.setPen(grid);
        p.drawText(pix.rect().adjusted(8, 0, -8, -8), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("等待统计..."));
        statsHistoryChart_->setPixmap(pix);
        return;
    }
    for (int i = 1; i <= 3; ++i)
    {
        const int y = height - 8 - ((height - 20) * i / 4);
        p.setPen(grid);
        p.drawLine(8, y, width - 8, y);
    }
    const double maxVal = std::max(0.1, *std::max_element(data.begin(), data.end()));
    const double stepX = (data.size() > 1) ? static_cast<double>(width - 16) / static_cast<double>(data.size() - 1)
                                           : static_cast<double>(width - 16);
    QPainterPath path;
    for (size_t i = 0; i < data.size(); ++i)
    {
        const double norm = std::clamp(data[i] / maxVal, 0.0, 1.0);
        const double x = 8.0 + stepX * static_cast<double>(i);
        const double y = height - 12.0 - norm * static_cast<double>(height - 20);
        if (i == 0)
        {
            path.moveTo(x, y);
        }
        else
        {
            path.lineTo(x, y);
        }
    }
    QPainterPath fill = path;
    fill.lineTo(8.0 + stepX * std::max<std::size_t>(0, data.size() - 1), height - 8.0);
    fill.lineTo(8.0, height - 8.0);
    QColor fillColor = line;
    fillColor.setAlpha(55);
    p.fillPath(fill, fillColor);
    QPen pen(line, 2.0);
    p.setPen(pen);
    p.drawPath(path);
    statsHistoryChart_->setPixmap(pix);
}

void QtClientWindow::FetchStatsHistory()
{
    if (!networkManager_)
    {
        networkManager_ = std::make_unique<QNetworkAccessManager>(this);
    }
    QString host = serverEdit_ ? serverEdit_->text() : QStringLiteral("127.0.0.1:7845");
    if (!host.startsWith(QStringLiteral("http")))
    {
        host = QStringLiteral("http://") + host;
    }
    const int targetId = targetSpin_ ? targetSpin_->value() : 0;
    QUrl url(host + QStringLiteral("/stats?session=%1").arg(targetId));
    if (!url.isValid())
    {
        AppendLog(QStringLiteral("[ui] 统计地址无效，跳过"));
        return;
    }
    AppendLog(QStringLiteral("[ui] 拉取统计: ") + url.toString());
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    auto* reply = networkManager_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> guard(reply);
        if (reply->error() != QNetworkReply::NoError)
        {
            AppendLog(QStringLiteral("[ui] 拉取统计失败: ") + reply->errorString());
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject())
        {
            AppendLog(QStringLiteral("[ui] 统计响应格式错误"));
            return;
        }
        const auto samplesVal = doc.object().value(QStringLiteral("samples"));
        if (!samplesVal.isArray())
        {
            AppendLog(QStringLiteral("[ui] 统计响应缺少 samples"));
            return;
        }
        remoteThroughput_.clear();
        for (const auto& v : samplesVal.toArray())
        {
            if (!v.isObject())
            {
                continue;
            }
            const auto obj = v.toObject();
            const double sent = obj.value(QStringLiteral("sent")).toDouble();
            const double recv = obj.value(QStringLiteral("recv")).toDouble();
            const double durMs = obj.value(QStringLiteral("dur")).toDouble();
            double throughput = 0.0;
            if (durMs > 0.0)
            {
                throughput = (sent + recv) / 1024.0 / 1024.0 / (durMs / 1000.0);
            }
            remoteThroughput_.push_back(throughput);
        }
        while (remoteThroughput_.size() > 90)
        {
            remoteThroughput_.pop_front();
        }
        RenderStatsHistory();
        AppendLog(QStringLiteral("[ui] 已同步历史统计 %1 条").arg(remoteThroughput_.size()));
    });
}

void QtClientWindow::ToggleSidebar()
{
    if (hSplit_ == nullptr || sidebar_ == nullptr)
    {
        return;
    }
    sidebarCollapsed_ = !sidebarCollapsed_;
    QList<int> sizes = hSplit_->sizes();
    if (sizes.size() < 2)
    {
        sizes = {200, 600};
    }
    QPropertyAnimation* anim = new QPropertyAnimation(hSplit_, "sizes");
    anim->setDuration(220);
    anim->setEasingCurve(QEasingCurve::InOutCubic);

    if (sidebarCollapsed_)
    {
        anim->setStartValue(QVariant::fromValue(sizes));
        anim->setEndValue(QVariant::fromValue(QList<int>({0, sizes[0] + sizes[1]})));
        toggleSidebarButton_->setText(QStringLiteral("展开侧栏"));
    }
    else
    {
        anim->setStartValue(QVariant::fromValue(QList<int>({0, sizes[0] + sizes[1]})));
        anim->setEndValue(QVariant::fromValue(QList<int>({220, std::max(320, sizes[1])})));
        toggleSidebarButton_->setText(QStringLiteral("折叠侧栏"));
    }
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    SaveSettings();
}

void QtClientWindow::SaveStatsHistory()
{
    QDir().mkpath(QStringLiteral("chat_cache"));
    QJsonArray arr;
    for (double v : speedHistoryPersisted_)
    {
        arr.append(v);
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("throughput"), arr);
    QFile f(QStringLiteral("chat_cache/stats.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }
}

void QtClientWindow::LoadStatsHistory()
{
    QFile f(QStringLiteral("chat_cache/stats.json"));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
    {
        return;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
    {
        return;
    }
    const auto arr = doc.object().value(QStringLiteral("throughput"));
    if (!arr.isArray())
    {
        return;
    }
    speedHistoryPersisted_.clear();
    for (const auto& v : arr.toArray())
    {
        speedHistoryPersisted_.push_back(v.toDouble());
    }
    for (double v : speedHistoryPersisted_)
    {
        RenderSpeedSparkline(v);
    }
    RenderStatsHistory();
}

void QtClientWindow::ShowErrorBanner(const QString& text, int severity, std::uint32_t retryAfterMs)
{
    if (alertBanner_ == nullptr || alertLabel_ == nullptr)
    {
        return;
    }
    QString message = text;
    if (retryAfterMs > 0)
    {
        message.append(QStringLiteral(" · 等待 %1 ms").arg(retryAfterMs));
    }
    alertLabel_->setText(message);
    if (alertRetryButton_ != nullptr)
    {
        const bool retryable = severity <= 1;
        alertRetryButton_->setVisible(retryable);
        alertRetryButton_->setEnabled(retryable);
        alertRetryButton_->setText(retryable ? QStringLiteral("重试发送") : QStringLiteral("已记录"));
    }
    alertBanner_->setVisible(true);
}

void QtClientWindow::HideErrorBanner()
{
    if (alertBanner_)
    {
        alertBanner_->setVisible(false);
    }
}

void QtClientWindow::SaveSettings()
{
    QDir().mkpath(QStringLiteral("chat_cache"));
    QJsonObject obj;
    obj.insert(QStringLiteral("darkTheme"), darkTheme_);
    obj.insert(QStringLiteral("accent"), accentColor_);
    obj.insert(QStringLiteral("sidebarCollapsed"), sidebarCollapsed_);
    obj.insert(QStringLiteral("paletteGroup"), currentPaletteGroup_);
    QJsonArray palette;
    for (const auto& c : customPalette_)
    {
        palette.append(c);
    }
    obj.insert(QStringLiteral("palette"), palette);
    obj.insert(QStringLiteral("server"), serverEdit_ ? serverEdit_->text() : QString());
    obj.insert(QStringLiteral("target"), static_cast<qint64>(targetSpin_ ? targetSpin_->value() : 0));
    obj.insert(QStringLiteral("lastSessionCount"), static_cast<qint64>(sessionItems_.size()));
    obj.insert(QStringLiteral("reconnectTimes"), reconnectSpin_ ? reconnectSpin_->value() : 0);
    obj.insert(QStringLiteral("reconnectDelay"), reconnectDelaySpin_ ? reconnectDelaySpin_->value() : 2000);
    obj.insert(QStringLiteral("certFingerprint"), certFingerprint_);
    QJsonArray sessions;
    for (auto it = sessionItems_.begin(); it != sessionItems_.end(); ++it)
    {
        QJsonObject s;
        const QString sid = it->first;
        s.insert(QStringLiteral("id"), sid);
        s.insert(QStringLiteral("name"), sid);
        const bool online = lastSeen_.find(sid) != lastSeen_.end();
        s.insert(QStringLiteral("online"), online);
        s.insert(QStringLiteral("unread"), unreadCount_[sid]);
        sessions.append(s);
    }
    obj.insert(QStringLiteral("sessions"), sessions);
    QFile f(QStringLiteral("chat_cache/settings.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }
}

void QtClientWindow::RefreshPaletteSwatches()
{
    if (paletteSwatchLayout_ == nullptr)
    {
        return;
    }
    while (QLayoutItem* item = paletteSwatchLayout_->takeAt(0))
    {
        if (auto* w = item->widget())
        {
            w->deleteLater();
        }
        delete item;
    }
    QStringList palette = activeGroupPalette_;
    for (const auto& c : customPalette_)
    {
        if (!palette.contains(c))
        {
            palette.push_back(c);
        }
    }
    for (const auto& c : palette)
    {
        auto* swatch = new QPushButton(this);
        swatch->setFixedSize(26, 26);
        swatch->setToolTip(c);
        swatch->setStyleSheet(QStringLiteral("border:1px solid #1f2937; border-radius:6px; background:%1;").arg(c));
        connect(swatch, &QPushButton::clicked, this, [this, c]() {
            accentColor_ = c;
            if (accentInput_)
            {
                accentInput_->setText(c);
            }
            ApplyTheme();
            SaveSettings();
        });
        paletteSwatchLayout_->addWidget(swatch);
    }
    paletteSwatchLayout_->addStretch();
}

void QtClientWindow::LoadSettings()
{
    QFile f(QStringLiteral("chat_cache/settings.json"));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
    {
        return;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
    {
        return;
    }
    const auto obj = doc.object();
    darkTheme_ = obj.value(QStringLiteral("darkTheme")).toBool(true);
    accentColor_ = obj.value(QStringLiteral("accent")).toString(accentColor_);
    sidebarCollapsed_ = obj.value(QStringLiteral("sidebarCollapsed")).toBool(false);
    currentPaletteGroup_ = obj.value(QStringLiteral("paletteGroup")).toString(currentPaletteGroup_);
    const auto paletteVal = obj.value(QStringLiteral("palette"));
    if (paletteVal.isArray())
    {
        customPalette_.clear();
        accentPaletteBox_->clear();
        for (const auto& v : paletteVal.toArray())
        {
            const QString c = v.toString();
            if (c.startsWith('#'))
            {
                customPalette_.push_back(c);
                accentPaletteBox_->addItem(c, c);
            }
        }
        RefreshPaletteSwatches();
    }
    if (serverEdit_)
    {
        const auto server = obj.value(QStringLiteral("server")).toString();
        if (!server.isEmpty())
        {
            serverEdit_->setText(server);
        }
    }
    if (targetSpin_)
    {
        targetSpin_->setValue(obj.value(QStringLiteral("target")).toInt(0));
    }
    if (reconnectSpin_)
    {
        reconnectSpin_->setValue(obj.value(QStringLiteral("reconnectTimes")).toInt(2));
    }
    if (reconnectDelaySpin_)
    {
        reconnectDelaySpin_->setValue(obj.value(QStringLiteral("reconnectDelay")).toInt(2000));
    }
    if (obj.contains(QStringLiteral("certFingerprint")))
    {
        certFingerprint_ = obj.value(QStringLiteral("certFingerprint")).toString();
    }
    const auto sessionsVal = obj.value(QStringLiteral("sessions"));
    if (sessionsVal.isArray())
    {
        PersistSessionsToFile(sessionsVal.toArray());
        LoadSessionCache();
    }
}

QString QtClientWindow::ToHtml(const QString& text) const
{
    QString normalized = MarkdownToHtml(text);
    QString escaped = normalized.toHtmlEscaped();
    // restore basic tags after escaping
    escaped.replace(QStringLiteral("&lt;b&gt;"), QStringLiteral("<b>"))
        .replace(QStringLiteral("&lt;/b&gt;"), QStringLiteral("</b>"))
        .replace(QStringLiteral("&lt;i&gt;"), QStringLiteral("<i>"))
        .replace(QStringLiteral("&lt;/i&gt;"), QStringLiteral("</i>"))
        .replace(QStringLiteral("&lt;code&gt;"), QStringLiteral("<code>"))
        .replace(QStringLiteral("&lt;/code&gt;"), QStringLiteral("</code>"));

    static const QRegularExpression urlRegex(QStringLiteral(R"((https?://[^\s<>]+))"));
    escaped.replace(urlRegex, QStringLiteral(R"(<a href="%1">%1</a>)").arg(QStringLiteral("\\1")));
    escaped.replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
    return escaped;
}

QString QtClientWindow::MarkdownToHtml(const QString& text) const
{
    QString out = text;
    out.replace(QRegularExpression(QStringLiteral(R"(\*\*(.+?)\*\*)")), QStringLiteral("<b>\\1</b>"));
    out.replace(QRegularExpression(QStringLiteral(R"(\*(.+?)\*))"), QStringLiteral("<i>\\1</i>"));
    out.replace(QRegularExpression(QStringLiteral(R"(`(.+?)`)")), QStringLiteral("<code>\\1</code>"));
    static const QMap<QString, QString> emojiMap = {
        {":)", "😊"},
        {":(", "☹️"},
        {":D", "😃"},
        {";)", "😉"},
        {":heart:", "❤️"},
        {":fire:", "🔥"},
    };
    for (auto it = emojiMap.constBegin(); it != emojiMap.constEnd(); ++it)
    {
        out.replace(it.key(), it.value());
    }
    return out;
}

void QtClientWindow::closeEvent(QCloseEvent* event)
{
    SaveDraft();
    SaveSettings();
    QWidget::closeEvent(event);
}

void QtClientWindow::OnStartClicked()
{
    const bool preserveHistory = preserveHistoryNextRun_;
    preserveHistoryNextRun_ = false;
    if (worker_.joinable())
    {
        AppendLog(QStringLiteral("[ui] 仍在运行"));
        return;
    }
    cancelled_.store(false);
    HideErrorBanner();
    if (!preserveHistory)
    {
        logEdit_->clear();
    }
    statusLabels_.clear();
    statusBadges_.clear();
    mediaPreviewCache_.clear();
    mediaOverlay_.clear();
    lastSeen_.clear();
    pendingMessages_.clear();
    ackRetries_.clear();
    lastSenderKey_.clear();
    lastDateKey_.clear();
    unreadMessages_.clear();
    if (messageView_ != nullptr && !preserveHistory)
    {
        messageView_->clear();
    }
    statusLabel_->setText(QStringLiteral("状态：运行中"));
    mediaProgress_->setValue(0);
    mediaProgress_->setFormat(QStringLiteral("上传/下载就绪"));
    mediaStatusLabel_->setText(QStringLiteral("媒体进度：准备就绪"));
    progressTimer_.invalidate();
    lastProgressBytes_ = 0;
    speedLogTimer_.invalidate();
    lastSpeedMbps_ = 0.0;
    mediaSizes_.clear();
    QFileInfoList mediaList;
    const QString mediaText = mediaEdit_->text();
    const QStringList mediaPaths = mediaText.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const auto& p : mediaPaths)
    {
        mediaList << QFileInfo(p.trimmed());
    }
    const auto targetId = targetSpin_->value();
    sessionLabel_->setText(targetId == 0 ? QStringLiteral("目标: 自己")
                                         : QStringLiteral("目标: %1").arg(targetId));
    SaveDraft();
    SaveSettings();
    if (!mediaList.isEmpty())
    {
        mediaStatusLabel_->setText(QStringLiteral("待发送媒体：%1").arg(mediaList.size()));
    }
    if (certBytes_.isEmpty())
    {
        FetchCertMemory();
    }
    StartWorker(preserveHistory);
    SetUiEnabled(false);
}

void QtClientWindow::OnBrowseMedia()
{
    QFileDialog dialog(this, QStringLiteral("选择媒体文件"));
    dialog.setFileMode(QFileDialog::ExistingFiles);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const QStringList files = dialog.selectedFiles();
    if (files.isEmpty())
    {
        return;
    }
    mediaEdit_->setText(files.join(QStringLiteral(";")));
    qint64 totalSize = 0;
    for (const auto& f : files)
    {
        QFileInfo info(f);
        if (info.exists())
        {
            totalSize += info.size();
        }
    }
    const auto mb = static_cast<double>(totalSize) / (1024.0 * 1024.0);
    mediaStatusLabel_->setText(QStringLiteral("媒体数量：%1 · 合计 %2 MB").arg(files.size()).arg(QString::number(mb, 'f', 2)));
}

void QtClientWindow::OnStopClicked()
{
    if (!worker_.joinable())
    {
        AppendLog(QStringLiteral("[ui] 未在运行"));
        return;
    }
    cancelled_.store(true);
    StopWorker();
    statusLabel_->setText(QStringLiteral("状态：已停止"));
    mediaProgress_->setFormat(QStringLiteral("已停止"));
    mediaStatusLabel_->setText(QStringLiteral("媒体进度：已停止"));
    SetUiEnabled(true);
}

void QtClientWindow::StartWorker(bool preserveHistory)
{
    Q_UNUSED(preserveHistory);
    mi::client::ClientOptions opts{};
    opts.username = userEdit_->text().toStdWString();
    opts.password = passEdit_->text().toStdWString();
    opts.message = messageEdit_->toPlainText().toStdWString();
    opts.targetSessionId = static_cast<std::uint32_t>(targetSpin_->value());
    const int modeIdx = modeCombo_->currentData().toInt();
    opts.sendMode = static_cast<mi::client::SendMode>(modeIdx);
    opts.revokeAfterReceive = revokeCheck_->isChecked();
    if (reconnectSpin_)
    {
        opts.reconnectAttempts = static_cast<std::uint32_t>(reconnectSpin_->value());
    }
    if (reconnectDelaySpin_)
    {
        opts.reconnectDelayMs = static_cast<std::uint32_t>(reconnectDelaySpin_->value());
    }

    if (!ParseHostPort(serverEdit_->text(), opts.serverHost, opts.serverPort))
    {
        AppendLog(QStringLiteral("[ui] 服务器地址无效"));
        SetUiEnabled(true);
        statusLabel_->setText(QStringLiteral("状态：地址错误"));
        return;
    }

    const QStringList mediaPaths = mediaEdit_->text().split(QStringLiteral(";"), Qt::SkipEmptyParts);
    preparedMediaThumbs_.clear();
    for (const auto& path : mediaPaths)
    {
        preparedMediaThumbs_.push_back(PrepareMediaThumb(path));
    }
    if (!certBytes_.isEmpty())
    {
        opts.certBytes.assign(reinterpret_cast<const std::uint8_t*>(certBytes_.constData()),
                              reinterpret_cast<const std::uint8_t*>(certBytes_.constData() + certBytes_.size()));
    }
    if (!certFingerprint_.isEmpty())
    {
        opts.certFingerprint = certFingerprint_.toStdString();
    }
    if (!certPassword_.isEmpty())
    {
        opts.certPassword = certPassword_.toStdString();
    }
    opts.certAllowSelfSigned = certAllowSelfSigned_;

    worker_ = std::thread([this, opts, mediaPaths]() {
        mi::shared::crypto::WhiteboxKeyInfo keyInfo = mi::shared::crypto::BuildKeyFromEnv();
    mi::client::ClientCallbacks cbs{};
    cbs.onLog = [](const std::wstring&) {};
    cbs.onEvent = [this](const mi::client::ClientCallbacks::ClientEvent& ev) {
        QMetaObject::invokeMethod(
            this,
            [this, ev]() {
                const QString category = QString::fromWCharArray(ev.category.c_str());
                const QString messageText = QString::fromWCharArray(ev.message.c_str());
                AppendEvent(messageText, ev.level);
                AppendLog(messageText);
        if (!ev.peer.empty())
        {
            UpdateSessionPresence(QString::fromWCharArray(ev.peer.c_str()));
        }

                const bool isRetry = category == QStringLiteral("retry");
                const bool isAck = category == QStringLiteral("chat") && messageText.contains(QStringLiteral("回执"));
                if (isRetry && ev.messageId != 0)
                {
                    int attempt = 0;
                    QRegularExpression re(QStringLiteral("第\\s*(\\d+)"));
                    auto m = re.match(messageText);
                    if (m.hasMatch())
                    {
                        attempt = m.captured(1).toInt();
                    }
                    UpdateMessageStatus(ev.messageId, QStringLiteral("重试%1").arg(attempt > 0 ? attempt : 1));
                    pendingMessages_[ev.messageId] = QDateTime::currentDateTime();
                    resendAttempts_[ev.messageId] = attempt;
                    return;
                }
                if (isAck && ev.messageId != 0)
                {
                    UpdateMessageStatus(ev.messageId, QStringLiteral("已送达"));
                    UpdateMessageStatus(ev.messageId, QStringLiteral("已读"));
                    pendingMessages_.erase(ev.messageId);
                    unreadMessages_.erase(ev.messageId);
                    ackRetries_.erase(ev.messageId);
                    resendAttempts_.erase(ev.messageId);
                    return;
                }

                if (category == QStringLiteral("error"))
                {
                    QString sev = QStringLiteral("提示");
                    if (ev.severity == 1)
                    {
                        sev = QStringLiteral("可重试");
                    }
                    else if (ev.severity >= 2)
                    {
                        sev = QStringLiteral("致命");
                    }
                    QString banner = QStringLiteral("[错误][%1] %2").arg(sev, messageText);
                    if (ev.retryAfterMs > 0)
                    {
                        banner.append(QStringLiteral(" · 建议等待 %1 ms").arg(ev.retryAfterMs));
                    }
                    statusLabel_->setText(banner);
                    statusLabel_->setStyleSheet(QStringLiteral("color:#f87171;"));
                    ShowErrorBanner(banner, ev.severity, ev.retryAfterMs);
                }
                else if (ev.level == mi::client::ClientCallbacks::EventLevel::Success)
                {
                    HideErrorBanner();
                }

                if ((category != QStringLiteral("chat") && category != QStringLiteral("media")) || messageView_ == nullptr)
                {
                    return;
                }
                const bool outbound = ev.direction == mi::client::ClientCallbacks::Direction::Outbound;
                QString content = messageText;
                if (!ev.payload.empty())
                {
                    content = QString::fromUtf8(reinterpret_cast<const char*>(ev.payload.data()),
                                                static_cast<int>(ev.payload.size()));
                }
                if (content.isEmpty())
                {
                    content = messageText;
                }
                const QString peer = ev.peer.empty() ? QStringLiteral("对端") : QString::fromWCharArray(ev.peer.c_str());
                const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
                const QString meta = QStringLiteral("%1 · %2 · #%3").arg(ts, peer).arg(ev.messageId);
                std::vector<QString> attachments;
                for (const auto& att : ev.attachments)
                {
                    attachments.push_back(QString::fromWCharArray(att.c_str()));
                }
                AddChatBubble(content,
                              outbound ? QStringLiteral("我") : peer,
                              meta,
                              outbound,
                              category == QStringLiteral("media"),
                              ev.messageId,
                              ev.payload,
                              attachments,
                              ev.format);
                if (outbound && ev.messageId != 0)
                {
                    UpdateMessageStatus(ev.messageId, QStringLiteral("已发送"));
                }
                if (category == QStringLiteral("chat") && messageText.contains(QStringLiteral("对端已读")))
                {
                    UpdateMessageStatus(ev.messageId, QStringLiteral("已读"));
                }

                if (ev.direction == mi::client::ClientCallbacks::Direction::Inbound && ev.messageId != 0)
                {
                    UpdateMessageStatus(ev.messageId, QStringLiteral("已送达"));
                    pendingMessages_.erase(ev.messageId);
                    ackRetries_.erase(ev.messageId);
                    resendAttempts_.erase(ev.messageId);
                }
                if (ev.level == mi::client::ClientCallbacks::EventLevel::Error && ev.messageId != 0)
                {
                    UpdateMessageStatus(ev.messageId, QStringLiteral("失败"));
                    pendingMessages_.erase(ev.messageId);
                    ackRetries_.erase(ev.messageId);
                    resendAttempts_.erase(ev.messageId);
                }
                if (messageText.contains(QStringLiteral("撤回")))
                {
                    UpdateMessageStatus(ev.messageId, QStringLiteral("已撤回"));
                    MarkMediaRevoked(ev.messageId);
                    pendingMessages_.erase(ev.messageId);
                    ackRetries_.erase(ev.messageId);
                    resendAttempts_.erase(ev.messageId);
                }
            },
            Qt::QueuedConnection);
    };
        cbs.onProgress = [this](const mi::client::ClientCallbacks::ProgressEvent& progress) {
            QMetaObject::invokeMethod(
                this,
                [this, progress]() {
                    UpdateMediaProgress(progress);
                },
                Qt::QueuedConnection);
        };
        cbs.onStats = [this](const mi::client::ClientCallbacks::StatsEvent& stats) {
            QMetaObject::invokeMethod(
                this,
                [this, stats]() {
                    ApplyStats(stats);
                },
                Qt::QueuedConnection);
        };
        cbs.onSessionList = [this](const std::vector<std::pair<std::uint32_t, std::wstring>>& sessions) {
            QMetaObject::invokeMethod(
                this,
                [this, sessions]() {
                    ApplySessionList(sessions);
                },
                Qt::QueuedConnection);
        };
        cbs.onFinished = [this](bool ok) {
            QMetaObject::invokeMethod(this,
                                      [this, ok]() {
                                          statusLabel_->setText(ok ? QStringLiteral("状态：完成") : QStringLiteral("状态：失败"));
                                          if (ok)
                                          {
                                              HideErrorBanner();
                                          }
                                          SetUiEnabled(true);
                                      },
                                      Qt::QueuedConnection);
        };
        cbs.isCancelled = [this]() { return cancelled_.load(); };
        int total = mediaPaths.isEmpty() ? 1 : mediaPaths.size();
        int index = 0;
        bool overallOk = true;
        QString failList;
        do
        {
            int attempt = 0;
            mi::client::ClientOptions runOpts = opts;
            if (!mediaPaths.isEmpty())
            {
                runOpts.mediaPath = mediaPaths.at(index).toStdWString();
                QMetaObject::invokeMethod(
                    this,
                    [this, index, total]() {
                        mediaStatusLabel_->setText(QStringLiteral("媒体发送中 %1/%2").arg(index + 1).arg(total));
                    },
                    Qt::QueuedConnection);
            }
            bool ok = false;
            do
            {
                ok = mi::client::RunClient(runOpts, keyInfo, cbs);
                if (ok || runOpts.reconnectAttempts == 0)
                {
                    break;
                }
                AppendLog(QStringLiteral("[ui] 运行失败，等待重连 %1/%2").arg(attempt + 1).arg(runOpts.reconnectAttempts));
                std::this_thread::sleep_for(std::chrono::milliseconds(runOpts.reconnectDelayMs));
                attempt++;
            } while (!ok && attempt < static_cast<int>(runOpts.reconnectAttempts));
            overallOk = overallOk && ok;
            if (!ok && !mediaPaths.isEmpty())
            {
                failList.append(mediaPaths.at(index) + QLatin1Char(';'));
            }
            ++index;
        } while (!mediaPaths.isEmpty() && index < mediaPaths.size() && !cancelled_.load());

        const bool finalOk = overallOk && !cancelled_.load();
        QMetaObject::invokeMethod(
            this,
            [this, finalOk, failList]() {
                statusLabel_->setText(finalOk ? QStringLiteral("状态：完成") : QStringLiteral("状态：失败/中断"));
                if (!failList.isEmpty())
                {
                    AppendLog(QStringLiteral("[ui] 失败媒体: ") + failList);
                }
                SetUiEnabled(true);
            },
            Qt::QueuedConnection);
        QMetaObject::invokeMethod(this, [this]() { StopWorker(); }, Qt::QueuedConnection);
    });
}

void QtClientWindow::StopWorker()
{
    cancelled_.store(true);
    if (worker_.joinable())
    {
        worker_.join();
    }
    SaveDraft();
    SaveSettings();
}

void QtClientWindow::AppendEvent(const QString& text, mi::client::ClientCallbacks::EventLevel level)
{
    if (feedList_ == nullptr)
    {
        return;
    }
    auto* item = new QListWidgetItem(text, feedList_);
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    item->setText(QStringLiteral("[%1] %2").arg(ts, text));
    switch (level)
    {
    case mi::client::ClientCallbacks::EventLevel::Success:
        item->setBackground(QColor("#0f5132"));
        item->setForeground(QColor("#d1fae5"));
        break;
    case mi::client::ClientCallbacks::EventLevel::Error:
        item->setBackground(QColor("#7f1d1d"));
        item->setForeground(QColor("#fee2e2"));
        break;
    default:
        item->setBackground(QColor("#111827"));
        item->setForeground(QColor("#e2e8f0"));
        break;
    }
    feedList_->addItem(item);
    feedList_->scrollToBottom();
}

void QtClientWindow::BootstrapSessionList()
{
    if (feedList_ == nullptr)
    {
        return;
    }
    sessionItems_.clear();
    lastSeen_.clear();
    feedList_->clear();
    auto self = new QListWidgetItem(QStringLiteral("会话 自己 · 在线"), feedList_);
    self->setForeground(QColor("#22c55e"));
    feedList_->addItem(self);
    sessionItems_[QStringLiteral("self")] = self;
    lastSeen_[QStringLiteral("self")] = QDateTime::currentDateTime();

    LoadSessionCache();

    if (targetSpin_->value() != 0 &&
        sessionItems_.find(QString::number(targetSpin_->value())) == sessionItems_.end())
    {
        const QString peerId = QString::number(targetSpin_->value());
        auto* peer = new QListWidgetItem(QStringLiteral("会话 %1 · 待激活").arg(peerId), feedList_);
        peer->setForeground(QColor("#94a3b8"));
        sessionItems_[peerId] = peer;
        feedList_->addItem(peer);
    }
}

void QtClientWindow::OnPresenceTick()
{
    const QDateTime now = QDateTime::currentDateTime();
    const int onlineWindow = 30;  // seconds
    for (auto it = lastSeen_.begin(); it != lastSeen_.end(); ++it)
    {
        const int secs = it->second.secsTo(now);
        auto itemIt = sessionItems_.find(it->first);
        if (itemIt != sessionItems_.end())
        {
            QListWidgetItem* item = itemIt->second;
            const bool online = secs <= onlineWindow;
            item->setText(QStringLiteral("会话 %1 · %2")
                              .arg(it->first, online ? QStringLiteral("在线") : QStringLiteral("离线")));
            item->setForeground(online ? QColor("#22c55e") : QColor("#64748b"));
        }
    }
}

void QtClientWindow::OnPendingTick()
{
    const QDateTime now = QDateTime::currentDateTime();
    const int warnSeconds = 5;
    const int failSeconds = 10;
    for (auto it = pendingMessages_.begin(); it != pendingMessages_.end();)
    {
        const int elapsed = it->second.secsTo(now);
        if (elapsed > failSeconds)
        {
            const int attempts = ++resendAttempts_[it->first];
            UpdateMessageStatus(it->first, QStringLiteral("等待回执·重试%1").arg(attempts));
            it->second = now;
            AppendLog(QStringLiteral("[ui] 消息 %1 回执超时，触发协议重试 %2").arg(it->first).arg(attempts));
            ++it;
        }
        else if (elapsed > warnSeconds)
        {
            UpdateMessageStatus(it->first, QStringLiteral("等待回执"));
            ++it;
        }
        else
        {
            ++it;
        }
    }
}

void QtClientWindow::OnReadTick()
{
    const QDateTime now = QDateTime::currentDateTime();
    const int readSeconds = 2;
    for (auto it = unreadMessages_.begin(); it != unreadMessages_.end();)
    {
        if (it->second.secsTo(now) >= readSeconds)
        {
            UpdateMessageStatus(it->first, QStringLiteral("已读"));
            auto lbl = statusLabels_.find(it->first);
            if (lbl != statusLabels_.end() && lbl->second)
            {
                lbl->second->setStyleSheet(QStringLiteral("color:#94a3b8; font-size:11px;"));
            }
            it = unreadMessages_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void QtClientWindow::MarkAllRead()
{
    for (const auto& kv : unreadMessages_)
    {
        UpdateMessageStatus(kv.first, QStringLiteral("已读"));
        auto lbl = statusLabels_.find(kv.first);
        if (lbl != statusLabels_.end() && lbl->second)
        {
            lbl->second->setStyleSheet(QStringLiteral("color:#94a3b8; font-size:11px;"));
        }
    }
    unreadMessages_.clear();
    for (auto& kv : unreadCount_)
    {
        kv.second = 0;
        UpdateSessionPresence(kv.first);
    }
}

void QtClientWindow::OnAckTick()
{
    const QDateTime now = QDateTime::currentDateTime();
    const int ackSeconds = 7;
    for (auto it = pendingMessages_.begin(); it != pendingMessages_.end();)
    {
        if (it->second.secsTo(now) >= ackSeconds)
        {
            int& mark = ackRetries_[it->first];
            if (mark == 0)
            {
                UpdateMessageStatus(it->first, QStringLiteral("等待协议回执/重试中"));
                AppendLog(QStringLiteral("[ui] 消息 #%1 等待回执已超过 %2 s，底层已自动重试")
                              .arg(it->first)
                              .arg(ackSeconds));
            }
            mark = 1;
            ++it;
        }
        else
        {
            ++it;
        }
    }
}

void QtClientWindow::OnRateTick()
{
    if (lastSpeedMbps_ > 0.01 && speedStatusLabel_)
    {
        AppendLog(QStringLiteral("[ui] 最近速率约 %1 MB/s").arg(QString::number(lastSpeedMbps_, 'f', 2)));
    }
}

void QtClientWindow::OnRetryTick()
{
    if (!pendingMessages_.empty())
    {
        AppendLog(QStringLiteral("[ui] 待回执消息 %1 条，核心已启用协议重试").arg(pendingMessages_.size()));
    }
}

void QtClientWindow::AppendLog(const QString& text)
{
    logEdit_->appendPlainText(text);
    logEdit_->verticalScrollBar()->setValue(logEdit_->verticalScrollBar()->maximum());
}

void QtClientWindow::SetUiEnabled(bool enabled)
{
    startButton_->setEnabled(enabled && !worker_.joinable());
    stopButton_->setEnabled(worker_.joinable());
    browseButton_->setEnabled(enabled);
    serverEdit_->setEnabled(enabled);
    userEdit_->setEnabled(enabled);
    passEdit_->setEnabled(enabled);
    messageEdit_->setEnabled(enabled);
    targetSpin_->setEnabled(enabled);
    modeCombo_->setEnabled(enabled);
    revokeCheck_->setEnabled(enabled);
    mediaEdit_->setEnabled(enabled);
}
