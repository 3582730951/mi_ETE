#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <thread>
#include <deque>

#include <QEvent>
#include <QObject>
#include <QDateTime>
#include <QMap>
#include <QPixmap>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QElapsedTimer>
#include <memory>

#include "client/client_runner.hpp"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QFrame;
class QMenu;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QLabel;
class QProgressBar;
class QCloseEvent;
class QHBoxLayout;
class QNetworkAccessManager;
class QTabWidget;
class QButtonGroup;
class QStackedWidget;
class QToolButton;

class QtClientWindow : public QWidget
{
    Q_OBJECT

public:
    explicit QtClientWindow(QWidget* parent = nullptr);
    ~QtClientWindow() override;

private slots:
    void OnStartClicked();
    void OnBrowseMedia();
    void OnStopClicked();

private:
    void BuildUi();
    void ApplyStyle();
    void AppendLog(const QString& text);
    void AppendEvent(const QString& text, mi::client::ClientCallbacks::EventLevel level);
    void AddChatBubble(const QString& content,
                       const QString& title,
                       const QString& meta,
                       bool outbound,
                       bool isMedia,
                       std::uint64_t messageId,
                       const std::vector<std::uint8_t>& payload,
                       const std::vector<QString>& attachments = {},
                       std::uint8_t format = 0);
    void UpdateMediaProgress(const mi::client::ClientCallbacks::ProgressEvent& progress);
    void UpdateMessageStatus(std::uint64_t messageId, const QString& status);
    void UpdateSessionPresence(const QString& peer);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void BootstrapSessionList();
    void OnPresenceTick();
    void OnPendingTick();
    void MarkMediaRevoked(std::uint64_t messageId);
    QPixmap BuildPlaceholderPreview(const QString& title) const;
    QPixmap BuildVideoPreview(const std::vector<std::uint8_t>& payload) const;
    void LoadDraft();
    void SaveDraft();
    void LoadSessionCache();
    void PersistSessionsToFile(const QJsonArray& arr);
    QJsonArray BuildDemoSessions() const;
    void SimulateSessionPullFromServer();
    void FetchRemoteSessions();
    void ApplyTheme();
    void SaveSettings();
    void LoadSettings();
    QString ToHtml(const QString& text) const;
    QString MarkdownToHtml(const QString& text) const;
    void OnReadTick();
    void OnAckTick();
    void OnRateTick();
    void OnRetryTick();
    void ExportSessionSnapshot() const;
    void ImportSessionSnapshot();
    void ApplySessionList(const std::vector<std::pair<std::uint32_t, std::wstring>>& sessions);
    void ApplyStats(const mi::client::ClientCallbacks::StatsEvent& stats);
    void MarkAllRead();
    void ApplyFormatting(const QString& prefix, const QString& suffix);
    void RenderSpeedSparkline(double throughputMbps);
    void RenderStatsHistory();
    void FetchStatsHistory();
    void ShowErrorBanner(const QString& text, int severity, std::uint32_t retryAfterMs);
    void HideErrorBanner();
    void FetchCertMemory();
    void ApplyCertMemory(const QByteArray& cert);
    QPixmap PrepareMediaThumb(const QString& path) const;
    void closeEvent(QCloseEvent* event) override;
    void SetUiEnabled(bool enabled);
    void ApplySessionWidget(const QString& peer, QListWidgetItem* item, bool online);
    void UpdateSessionBadge(const QString& peer);
    void ShowListPage();
    void ShowChatPage(const QString& peer, bool isGroup);
    void ToggleSettings();
    void ShowLoginPage();
    void ApplyLogin();
    void UpdateAccountUi();
    void StartWorker(bool preserveHistory = false);
    void StopWorker();
    void RefreshPaletteSwatches();
    void ToggleSidebar();
    void SaveStatsHistory();
    void LoadStatsHistory();

    QLabel* statusLabel_;
    QLineEdit* serverEdit_;
    QLineEdit* userEdit_;
    QLineEdit* passEdit_;
    QPlainTextEdit* messageEdit_;
    QLineEdit* mediaEdit_;
    QSpinBox* targetSpin_;
    QComboBox* modeCombo_;
    QCheckBox* revokeCheck_;
    QSpinBox* reconnectSpin_;
    QSpinBox* reconnectDelaySpin_;
    QPushButton* startButton_;
    QPushButton* stopButton_;
    QPushButton* browseButton_;
    QPushButton* emojiButton_;
    QPushButton* toggleSidebarButton_;
    QPushButton* toggleSettingsButton_;
    QPushButton* switchAccountButton_;
    QPushButton* backButton_;
    QPushButton* callButton_;
    QPushButton* videoButton_;
    QPushButton* screenShareButton_;
    QPushButton* fileActionButton_;
    QPushButton* moreActionButton_;
    QButtonGroup* navGroup_;
    QProgressBar* mediaProgress_;
    QLabel* mediaStatusLabel_;
    QLabel* speedStatusLabel_;
    QLabel* speedPeakLabel_;
    QLabel* speedSparkline_;
    QLabel* statsHistoryChart_;
    QPushButton* statsRefreshButton_;
    QPlainTextEdit* logEdit_;
    QListWidget* messageView_;
    QListWidget* feedList_;
    QLineEdit* sessionSearch_;
    QStackedWidget* mainStack_;
    QWidget* loginPage_;
    QWidget* mainPage_;
    QWidget* listPage_;
    QLabel* loginServerLabel_;
    QLineEdit* loginUserEdit_;
    QLineEdit* loginPassEdit_;
    QCheckBox* loginRemember_;
    QLabel* accountLabel_;
    QLabel* accountNameLabel_;
    QLabel* accountServerLabel_;
    QFrame* sidebar_;
    QFrame* navRail_;
    QFrame* mainPanel_;
    QFrame* settingsPanel_;
    QSplitter* hSplit_;
    QLabel* sessionLabel_;
    QLabel* channelStatusLabel_;
    QComboBox* themeSwitch_;
    QComboBox* accentSwitch_;
    QComboBox* paletteGroupBox_;
    QLineEdit* accentInput_;
    QComboBox* accentPaletteBox_;
    QPushButton* accentAddButton_;
    QPushButton* boldButton_;
    QPushButton* italicButton_;
    QPushButton* codeButton_;
    QPushButton* sendMenuButton_;
    QLabel* networkStatusLabel_;
    QFrame* alertBanner_;
    QLabel* alertLabel_;
    QPushButton* alertRetryButton_;
    QMenu* sendMenu_;
    QShortcut* sendShortcutEnter_;
    QShortcut* sendShortcutCtrlEnter_;
    std::unordered_map<std::uint64_t, QLabel*> statusLabels_;
    std::unordered_map<std::uint64_t, QLabel*> statusBadges_;
    std::unordered_map<QString, QListWidgetItem*> sessionItems_;
    std::unordered_map<QString, QLabel*> sessionBadgeLabels_;
    std::unordered_map<QString, QLabel*> sessionNameLabels_;
    std::unordered_map<QString, QLabel*> sessionMetaLabels_;
    std::unordered_map<QLabel*, QPixmap> mediaPreviewCache_;
    std::unordered_map<std::uint64_t, QPointer<QLabel>> mediaPreviewById_;
    std::unordered_map<std::uint64_t, QPointer<QLabel>> mediaOverlay_;
    std::unordered_map<std::uint64_t, std::uint32_t> mediaSizes_;
    std::unordered_map<QString, QDateTime> lastSeen_;
    std::unordered_map<QString, int> unreadCount_;
    std::unordered_map<std::uint64_t, QDateTime> pendingMessages_;
    std::unordered_map<std::uint64_t, QDateTime> unreadMessages_;
    std::unordered_map<std::uint64_t, int> resendAttempts_;
    std::unordered_map<std::uint64_t, int> ackRetries_;
    QTimer presenceTimer_;
    QTimer pendingTimer_;
    QTimer readTimer_;
    QTimer ackTimer_;
    QTimer sessionRefreshTimer_;
    bool darkTheme_;
    QString lastSenderKey_;
    QString lastDateKey_;
    QPushButton* refreshSessions_;
    QString accentColor_;
    QElapsedTimer progressTimer_;
    std::uint64_t lastProgressBytes_;
    QElapsedTimer speedLogTimer_;
    double lastSpeedMbps_;
    std::unique_ptr<QNetworkAccessManager> networkManager_;
    QMenu* emojiMenu_;
    std::thread worker_;
    std::atomic<bool> cancelled_;
    std::vector<std::pair<std::uint32_t, std::wstring>> cachedSessionList_;
    std::vector<QString> customPalette_;
    std::deque<double> speedHistory_;
    std::deque<double> remoteThroughput_;
    QByteArray certBytes_;
    QString certFingerprint_;
    QString certPassword_;
    bool certAllowSelfSigned_ = true;
    QHBoxLayout* paletteSwatchLayout_;
    QTabWidget* settingsTabs_;
    std::deque<QPixmap> preparedMediaThumbs_;
    bool sidebarCollapsed_;
    bool settingsCollapsed_;
    int lastSettingsWidth_;
    bool loggedIn_;
    int activeNavIndex_ = 0;
    bool sendOnEnter_ = false;
    QStringList activeGroupPalette_;
    QString currentPaletteGroup_;
    std::deque<double> speedHistoryPersisted_;
    bool preserveHistoryNextRun_ = false;
    QString currentPeer_;
    bool currentPeerIsGroup_ = false;
};
