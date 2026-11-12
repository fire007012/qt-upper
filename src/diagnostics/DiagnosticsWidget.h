#pragma once
#include <QWidget>
#include <QTimer>
class Controller;
class QPlainTextEdit;
class QComboBox;
class QLabel;
class QPushButton;
class DiagnosticsWidget : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsWidget(Controller *controller, QWidget *parent = nullptr);
private:
    void refresh();
    Controller *m_controller;
    QTimer m_timer;
    QPlainTextEdit *m_details;
    QComboBox *m_requests;
    QLabel *m_status;
    QPushButton *m_recover, *m_acknowledge;
};
