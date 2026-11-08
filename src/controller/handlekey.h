#ifndef HANDLEKEY_H
#define HANDLEKEY_H

#include <QObject>
#include <QTimer>

#ifdef USE_SDL3_GAMEPAD
typedef struct SDL_Gamepad SDL_Gamepad;
#endif

struct ControllerState {
    bool buttonA;
    bool buttonB;
    bool buttonX;
    bool buttonY;

    bool buttonStart;
    bool buttonBack;

    bool dpadUp;
    bool dpadDown;
    bool dpadLeft;
    bool dpadRight;

    bool leftThumb;
    bool rightThumb;
    bool leftShoulder;
    bool rightShoulder;

    int16_t sThumbLX;      // 左摇杆 X 轴 (-32768 到 32767)
    int16_t sThumbLY;      // 左摇杆 Y 轴
    int16_t sThumbRX;      // 右摇杆 X 轴
    int16_t sThumbRY;      // 右摇杆 Y 轴

    uint8_t bLeftTrigger;  // 左扳机 (0-255)
    uint8_t bRightTrigger; // 右扳机 (0-255)

};

class HandleKey : public QObject
{
    Q_OBJECT

public:
    explicit HandleKey(QObject *parent = nullptr);
    ~HandleKey() override;

    bool isConnected() const { return m_connected; }
    ControllerState sampleNow() { readGamepad(); return state; }
    void startPolling();
    void stopPolling();

signals:
    void getHandleKey(const ControllerState &state);
    void connectionChanged(bool connected);

private slots:
    void readGamepad();

private:
    void setConnected(bool connected);
    void publishStateIfChanged(const ControllerState &newState);

#ifdef USE_SDL3_GAMEPAD
    bool initSdl();
    void shutdownSdl();
    bool openFirstSdlGamepad();
    void closeSdlGamepad();
    bool readSdlGamepad(ControllerState &newState);
    SDL_Gamepad *m_sdlGamepad = nullptr;
    bool m_sdlInitialized = false;
#endif

#ifdef Q_OS_WIN
    bool readXInputGamepad(ControllerState &newState);
#endif

    QTimer *timer;
    ControllerState state = {};
    bool m_connected = false;
};

#endif // HANDLEKEY_H
